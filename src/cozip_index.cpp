#include "cozip_index.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

namespace duckdb {

static constexpr uint32_t ZIP_LFH_SIGNATURE = 0x04034B50U;
static constexpr uint16_t COZIP_EXTRA_HEADER_ID = 0xCA0C;
static constexpr uint16_t COZIP_FORMAT_VERSION = 1;
static const char *COZIP_INDEX_NAME = "__cozip__";
static constexpr idx_t COZIP_INDEX_NAME_LEN = 9;

static inline uint16_t ReadU16LE(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t ReadU32LE(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ReadU64LE(const uint8_t *p) {
	return (uint64_t)ReadU32LE(p) | ((uint64_t)ReadU32LE(p + 4) << 32);
}

// cozip spec 8.5 step 1: the fixed shape of the __cozip__ local file header.
static uint8_t ParseProfilePrefix(const uint8_t *head, idx_t size, const string &source) {
	if (size < COZIP_LFH_SIZE + 7) {
		throw InvalidInputException("cozip profile prefix is truncated: %s", source);
	}
	if (ReadU32LE(head) != ZIP_LFH_SIGNATURE) {
		throw InvalidInputException("byte 0 is not a ZIP Local File Header: %s", source);
	}
	if (ReadU16LE(head + 8) != 0) {
		throw InvalidInputException("__cozip__ compression method is not STORE: %s", source);
	}
	if (ReadU16LE(head + 26) != COZIP_INDEX_NAME_LEN || ReadU16LE(head + 28) != 12) {
		throw InvalidInputException("LFH does not match cozip layout: %s", source);
	}
	if (memcmp(head + 30, COZIP_INDEX_NAME, COZIP_INDEX_NAME_LEN) != 0) {
		throw InvalidInputException("first ZIP entry is not __cozip__: %s", source);
	}
	if (ReadU16LE(head + 39) != COZIP_EXTRA_HEADER_ID || ReadU16LE(head + 41) != 8) {
		throw InvalidInputException("cozip integrity extra field (0xCA0C) missing: %s", source);
	}
	auto payload = head + COZIP_LFH_SIZE;
	if (memcmp(payload, "CZIP", 4) != 0) {
		throw InvalidInputException("index payload magic is not 'CZIP': %s", source);
	}
	auto version = ReadU16LE(payload + 4);
	if (version > COZIP_FORMAT_VERSION) {
		throw InvalidInputException("unsupported cozip format version %d: %s", (int)version, source);
	}
	return payload[6];
}

static void CheckMinimumSize(idx_t file_size, const string &source) {
	if (file_size < COZIP_MIN_SIZE) {
		throw InvalidInputException("cozip archive too small (minimum is %llu bytes): %s", (uint64_t)COZIP_MIN_SIZE,
		                            source);
	}
}

uint8_t ReadCozipProfile(FileHandle &handle, const string &source) {
	CheckMinimumSize((idx_t)handle.GetFileSize(), source);
	vector<uint8_t> prefix(COZIP_LFH_SIZE + 7);
	handle.Read(prefix.data(), prefix.size(), 0);
	return ParseProfilePrefix(prefix.data(), prefix.size(), source);
}

CozipIndex ReadCozipIndex(FileHandle &handle, const string &source) {
	auto file_size = (idx_t)handle.GetFileSize();
	CheckMinimumSize(file_size, source);

	auto bootstrap = MinValue<idx_t>(COZIP_BOOTSTRAP_SIZE, file_size);
	vector<uint8_t> head(bootstrap);
	handle.Read(head.data(), bootstrap, 0);

	CozipIndex index;
	index.profile = ParseProfilePrefix(head.data(), head.size(), source);
	index.version = ReadU16LE(head.data() + COZIP_LFH_SIZE + 4);

	auto payload_size = (idx_t)ReadU32LE(head.data() + 18);
	if (payload_size == 0) {
		throw InvalidInputException("cozip index payload size is zero: %s", source);
	}
	auto payload_end = COZIP_LFH_SIZE + payload_size;
	if (payload_end > file_size) {
		throw InvalidInputException("cozip index payload is truncated: %s", source);
	}
	if (payload_end > head.size()) {
		auto extra = payload_end - head.size();
		vector<uint8_t> tail(extra);
		handle.Read(tail.data(), extra, head.size());
		head.insert(head.end(), tail.begin(), tail.end());
	}

	auto payload = head.data() + COZIP_LFH_SIZE;
	auto count = (idx_t)ReadU32LE(payload + 7);
	// Every entry costs at least 18 bytes of fixed width plus one name byte.
	if (count > payload_size / (COZIP_INDEX_PER_ENTRY_MIN)) {
		throw InvalidInputException("cozip index declares more entries than fit in its payload: %s", source);
	}

	auto cursor = payload + COZIP_INDEX_HEADER_SIZE;
	auto limit = payload + payload_size;
	vector<uint16_t> name_lengths(count);
	for (idx_t i = 0; i < count; i++) {
		if (cursor + 2 > limit) {
			throw InvalidInputException("cozip index is truncated in its name lengths: %s", source);
		}
		name_lengths[i] = ReadU16LE(cursor);
		if (name_lengths[i] == 0) {
			throw InvalidInputException("cozip index has an empty name: %s", source);
		}
		cursor += 2;
	}
	index.entries.resize(count);
	for (idx_t i = 0; i < count; i++) {
		if (cursor + name_lengths[i] > limit) {
			throw InvalidInputException("cozip index is truncated in its names: %s", source);
		}
		index.entries[i].name = string((const char *)cursor, name_lengths[i]);
		cursor += name_lengths[i];
	}
	for (idx_t i = 0; i < count; i++) {
		if (cursor + 8 > limit) {
			throw InvalidInputException("cozip index is truncated in its offsets: %s", source);
		}
		index.entries[i].offset = ReadU64LE(cursor);
		cursor += 8;
	}
	for (idx_t i = 0; i < count; i++) {
		if (cursor + 8 > limit) {
			throw InvalidInputException("cozip index is truncated in its sizes: %s", source);
		}
		index.entries[i].size = ReadU64LE(cursor);
		cursor += 8;
	}
	// cozip spec 7.5: a reader should reject ranges that leave the archive.
	for (auto &entry : index.entries) {
		if (entry.size == 0 || entry.offset < COZIP_LFH_SIZE || entry.offset > file_size ||
		    entry.size > file_size - entry.offset) {
			throw InvalidInputException("cozip index entry '%s' has a range outside the archive: %s", entry.name,
			                            source);
		}
	}
	return index;
}

CozipIndex ReadCozipIndex(ClientContext &context, const string &source) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(source, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("could not open %s", source);
	}
	return ReadCozipIndex(*handle, source);
}

const CozipEntry *CozipIndex::Find(const string &name) const {
	for (auto &entry : entries) {
		if (entry.name == name) {
			return &entry;
		}
	}
	return nullptr;
}

string CozipIndex::OffsetSize(const string &name, const string &source) const {
	auto entry = Find(name);
	if (!entry) {
		throw InvalidInputException("cozip index has no entry named '%s': %s", name, source);
	}
	return to_string(entry->offset) + "_" + to_string(entry->size);
}

string SubFilePath(const string &source, uint64_t offset, uint64_t size) {
	return "cozip-subfile://" + to_string(offset) + "_" + to_string(size) + "!" + source;
}

string ProfileName(uint8_t profile) {
	switch (profile) {
	case COZIP_PROFILE_NONE:
		return "none";
	case COZIP_PROFILE_FLAT:
		return "flat";
	case COZIP_PROFILE_TACO:
		return "taco";
	default:
		return "unknown:" + to_string((int)profile);
	}
}

string BuildVsiBase(const string &path) {
	if (StringUtil::StartsWith(path, "https://") || StringUtil::StartsWith(path, "http://")) {
		return "/vsicurl/" + path;
	}
	if (StringUtil::StartsWith(path, "s3://")) {
		return "/vsis3/" + path.substr(5);
	}
	if (StringUtil::StartsWith(path, "gcs://")) {
		return "/vsigs/" + path.substr(6);
	}
	if (StringUtil::StartsWith(path, "gs://")) {
		return "/vsigs/" + path.substr(5);
	}
	if (StringUtil::StartsWith(path, "abfss://")) {
		return "/vsiadls/" + path.substr(8);
	}
	if (StringUtil::StartsWith(path, "azure://")) {
		return "/vsiaz/" + path.substr(8);
	}
	if (StringUtil::StartsWith(path, "hf://")) {
		auto rest = path.substr(5);
		string ns_prefix;
		if (StringUtil::StartsWith(rest, "datasets/")) {
			ns_prefix = "datasets/";
			rest = rest.substr(9);
		} else if (StringUtil::StartsWith(rest, "spaces/")) {
			ns_prefix = "spaces/";
			rest = rest.substr(7);
		}
		auto first = rest.find('/');
		auto second = (first == string::npos) ? string::npos : rest.find('/', first + 1);
		if (first == string::npos || second == string::npos) {
			throw InvalidInputException("cannot parse hf:// URL for VSI mapping: %s", path);
		}
		auto owner_repo = rest.substr(0, second);
		auto inner = rest.substr(second + 1);
		return "/vsicurl/https://huggingface.co/" + ns_prefix + owner_repo + "/resolve/main/" + inner;
	}
	return path;
}

} // namespace duckdb
