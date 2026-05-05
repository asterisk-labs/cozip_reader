#define DUCKDB_EXTENSION_MAIN

#include "cozip_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace duckdb {

static constexpr uint32_t ZIP_LOCAL_FILE_HEADER_SIGNATURE = 0x04034B50U;
static constexpr uint16_t ZIP_STORE_METHOD = 0;
static constexpr uint32_t ZIP32_SIZE_SENTINEL = 0xFFFFFFFFU;
static constexpr uint16_t ZIP_ENCRYPTED_FLAG = 1U << 0U;
static constexpr uint16_t ZIP_DATA_DESCRIPTOR_FLAG = 1U << 3U;
static constexpr uint16_t ZIP_STRONG_ENCRYPTION_FLAG = 1U << 6U;
static constexpr uint16_t ZIP_CD_ENCRYPTED_FLAG = 1U << 13U;
static constexpr uint16_t ZIP_FORBIDDEN_FLAGS =
    ZIP_ENCRYPTED_FLAG | ZIP_DATA_DESCRIPTOR_FLAG | ZIP_STRONG_ENCRYPTION_FLAG | ZIP_CD_ENCRYPTED_FLAG;

static constexpr idx_t ZIP_LOCAL_FILE_HEADER_SIZE = 30;
static constexpr idx_t COZIP_INDEX_OFFSET = 51;
static constexpr idx_t COZIP_INDEX_NAME_LEN = 9;
static constexpr idx_t COZIP_EXTRA_FIELD_SIZE = 12;
static constexpr idx_t COZIP_INTEGRITY_HASH_OFFSET = 43;
static constexpr uint16_t COZIP_EXTRA_HEADER_ID = 0xCA0C;
static constexpr uint16_t COZIP_EXTRA_DATA_SIZE = 8;
static constexpr idx_t COZIP_HASH_WINDOW_SIZE = 32768;
static constexpr idx_t COZIP_MIN_ARCHIVE_SIZE = COZIP_INDEX_OFFSET + COZIP_HASH_WINDOW_SIZE;
static constexpr idx_t COZIP_INDEX_HEADER_SIZE = 11;
static constexpr idx_t COZIP_MAX_PRIORITY_ENTRIES = 1000000;
static constexpr uint16_t COZIP_SUPPORTED_FORMAT_VERSION = 1;

static const std::string COZIP_INDEX_NAME = "__cozip__";

struct CozipEntry {
	std::string name;
	uint64_t offset;
	uint64_t size;
};

struct CozipIndex {
	uint16_t version;
	uint8_t profile;
	uint32_t index_payload_size;
	std::vector<CozipEntry> entries;
	uint64_t integrity_hash_stored;
	uint64_t integrity_hash_computed;
};

static void EnsureRange(idx_t pos, idx_t len, idx_t limit, const std::string &message) {
	if (pos > limit || len > limit - pos) {
		throw InvalidInputException(message);
	}
}

static uint16_t ReadU16(const std::vector<uint8_t> &buf, idx_t pos) {
	EnsureRange(pos, 2, buf.size(), "buffer too small for u16");
	return (uint16_t)buf[pos] | ((uint16_t)buf[pos + 1] << 8);
}

static uint32_t ReadU32(const std::vector<uint8_t> &buf, idx_t pos) {
	EnsureRange(pos, 4, buf.size(), "buffer too small for u32");
	return (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) | ((uint32_t)buf[pos + 2] << 16) |
	       ((uint32_t)buf[pos + 3] << 24);
}

static uint64_t ReadU64(const std::vector<uint8_t> &buf, idx_t pos) {
	auto lo = (uint64_t)ReadU32(buf, pos);
	auto hi = (uint64_t)ReadU32(buf, pos + 4);
	return lo | (hi << 32);
}

static std::string ReadString(const std::vector<uint8_t> &buf, idx_t pos, idx_t len) {
	EnsureRange(pos, len, buf.size(), "buffer too small for string");
	return std::string(reinterpret_cast<const char *>(buf.data() + pos), len);
}

static uint64_t FNV1a64Update(uint64_t hash, const uint8_t *data, idx_t len) {
	static constexpr uint64_t FNV_PRIME = 0x100000001B3ULL;
	for (idx_t i = 0; i < len; i++) {
		hash ^= (uint64_t)data[i];
		hash *= FNV_PRIME;
	}
	return hash;
}

static std::vector<uint8_t> ReadFileFully(const std::string &path) {
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) {
		throw IOException("Could not open file: " + path);
	}

	auto end_pos = in.tellg();
	if (end_pos < 0) {
		throw IOException("Could not determine file size: " + path);
	}

	std::vector<uint8_t> data((size_t)end_pos);
	in.seekg(0, std::ios::beg);
	if (!data.empty() && !in.read(reinterpret_cast<char *>(data.data()), (std::streamsize)data.size())) {
		throw IOException("Could not read file: " + path);
	}
	return data;
}

static std::string JsonEscape(const std::string &s) {
	std::ostringstream out;
	for (auto c : s) {
		switch (c) {
		case '\\':
			out << "\\\\";
			break;
		case '"':
			out << "\\\"";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			out << c;
			break;
		}
	}
	return out.str();
}

static uint64_t ComputeIntegrityHash(const std::vector<uint8_t> &buf, idx_t index_payload_size) {
	static constexpr uint64_t FNV_OFFSET_BASIS = 0xCBF29CE484222325ULL;

	auto archive_size = (idx_t)buf.size();
	auto index_start = COZIP_INDEX_OFFSET;
	auto index_end = COZIP_INDEX_OFFSET + index_payload_size;
	auto suffix_start = archive_size - COZIP_HASH_WINDOW_SIZE;

	auto hash = FNV_OFFSET_BASIS;
	if (index_end <= suffix_start) {
		hash = FNV1a64Update(hash, buf.data() + index_start, index_payload_size);
		hash = FNV1a64Update(hash, buf.data() + suffix_start, COZIP_HASH_WINDOW_SIZE);
	} else {
		hash = FNV1a64Update(hash, buf.data() + index_start, archive_size - index_start);
	}
	return hash;
}

static CozipIndex ParseCozip(const std::string &path) {
	auto buf = ReadFileFully(path);
	if (buf.size() < COZIP_INDEX_OFFSET) {
		throw InvalidInputException("Archive too small to contain a cozip index Local File Header: " + path);
	}
	if (buf.size() < COZIP_MIN_ARCHIVE_SIZE) {
		throw InvalidInputException("Archive is smaller than the cozip minimum size of 32819 bytes: " + path);
	}
	if (ReadU32(buf, 0) != ZIP_LOCAL_FILE_HEADER_SIGNATURE) {
		throw InvalidInputException("Byte 0 is not a ZIP local file header: " + path);
	}

	auto flags = ReadU16(buf, 6);
	auto method = ReadU16(buf, 8);
	auto compressed_size = ReadU32(buf, 18);
	auto uncompressed_size = ReadU32(buf, 22);
	auto name_len = ReadU16(buf, 26);
	auto extra_len = ReadU16(buf, 28);

	if ((flags & ZIP_FORBIDDEN_FLAGS) != 0) {
		throw InvalidInputException("cozip index entry has forbidden ZIP general-purpose flags: " + path);
	}
	if (method != ZIP_STORE_METHOD) {
		throw InvalidInputException("cozip index entry must use ZIP STORE compression method: " + path);
	}
	if (compressed_size == 0 || compressed_size == ZIP32_SIZE_SENTINEL || compressed_size != uncompressed_size) {
		throw InvalidInputException("cozip index entry has invalid ZIP32 size fields: " + path);
	}
	if (name_len != COZIP_INDEX_NAME_LEN) {
		throw InvalidInputException("cozip index entry filename length must be 9 bytes: " + path);
	}
	if (extra_len != COZIP_EXTRA_FIELD_SIZE) {
		throw InvalidInputException("cozip index entry must have a 12-byte local extra field: " + path);
	}
	if (ReadString(buf, ZIP_LOCAL_FILE_HEADER_SIZE, COZIP_INDEX_NAME_LEN) != COZIP_INDEX_NAME) {
		throw InvalidInputException("First ZIP entry is not __cozip__: " + path);
	}
	if (ReadU16(buf, 39) != COZIP_EXTRA_HEADER_ID || ReadU16(buf, 41) != COZIP_EXTRA_DATA_SIZE) {
		throw InvalidInputException("cozip index entry is missing the 0xCA0C integrity extra field: " + path);
	}

	auto index_payload_size = (idx_t)compressed_size;
	auto payload_offset = COZIP_INDEX_OFFSET;
	auto payload_end = payload_offset + index_payload_size;
	EnsureRange(payload_offset, index_payload_size, buf.size(), "cozip index payload exceeds archive size: " + path);
	EnsureRange(payload_offset, COZIP_INDEX_HEADER_SIZE, payload_end, "truncated cozip index header: " + path);

	auto pos = payload_offset;
	if (ReadString(buf, pos, 4) != "CZIP") {
		throw InvalidInputException("Invalid cozip magic: " + path);
	}

	CozipIndex index;
	index.version = ReadU16(buf, pos + 4);
	index.profile = buf[pos + 6];
	index.index_payload_size = compressed_size;
	auto n_entries = (idx_t)ReadU32(buf, pos + 7);

	if (index.version > COZIP_SUPPORTED_FORMAT_VERSION) {
		throw InvalidInputException("Unsupported cozip format version: " + std::to_string(index.version));
	}
	if (n_entries > COZIP_MAX_PRIORITY_ENTRIES) {
		throw InvalidInputException("cozip index has too many priority entries: " + path);
	}
	pos += COZIP_INDEX_HEADER_SIZE;

	EnsureRange(pos, 2 * n_entries, payload_end, "truncated cozip name-length section: " + path);
	std::vector<uint16_t> name_lengths;
	name_lengths.reserve(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		name_lengths.push_back(ReadU16(buf, pos + 2 * i));
	}
	pos += 2 * n_entries;

	std::vector<std::string> names;
	names.reserve(n_entries);
	std::unordered_set<std::string> seen_names;
	for (idx_t i = 0; i < n_entries; i++) {
		auto len = (idx_t)name_lengths[i];
		if (len == 0) {
			throw InvalidInputException("cozip priority entry name cannot be empty: " + path);
		}
		EnsureRange(pos, len, payload_end, "truncated cozip names section: " + path);
		auto name = ReadString(buf, pos, len);
		if (name == COZIP_INDEX_NAME) {
			throw InvalidInputException("cozip index must not list __cozip__ as a priority entry: " + path);
		}
		if (!seen_names.insert(name).second) {
			throw InvalidInputException("duplicate cozip priority entry name: " + name);
		}
		names.push_back(std::move(name));
		pos += len;
	}

	EnsureRange(pos, 8 * n_entries, payload_end, "truncated cozip offsets section: " + path);
	std::vector<uint64_t> offsets;
	offsets.reserve(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		offsets.push_back(ReadU64(buf, pos + 8 * i));
	}
	pos += 8 * n_entries;

	EnsureRange(pos, 8 * n_entries, payload_end, "truncated cozip sizes section: " + path);
	std::vector<uint64_t> sizes;
	sizes.reserve(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		sizes.push_back(ReadU64(buf, pos + 8 * i));
	}
	pos += 8 * n_entries;
	if (pos != payload_end) {
		throw InvalidInputException("cozip index payload has trailing bytes: " + path);
	}

	index.entries.reserve(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		if (sizes[i] == 0) {
			throw InvalidInputException("cozip priority entry has zero payload size: " + names[i]);
		}
		if (offsets[i] > buf.size() || sizes[i] > buf.size() - offsets[i]) {
			throw InvalidInputException("cozip priority entry exceeds archive size: " + names[i]);
		}
		index.entries.push_back(CozipEntry {names[i], offsets[i], sizes[i]});
	}

	index.integrity_hash_stored = ReadU64(buf, COZIP_INTEGRITY_HASH_OFFSET);
	index.integrity_hash_computed = ComputeIntegrityHash(buf, index_payload_size);

	return index;
}

inline void CozipPriorityCountFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vector = args.data[0];
	UnaryExecutor::Execute<string_t, uint64_t>(path_vector, result, args.size(), [&](string_t path) {
		auto idx = ParseCozip(path.GetString());
		return (uint64_t)idx.entries.size();
	});
}

inline void CozipProfileFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vector = args.data[0];
	UnaryExecutor::Execute<string_t, uint64_t>(path_vector, result, args.size(), [&](string_t path) {
		auto idx = ParseCozip(path.GetString());
		return (uint64_t)idx.profile;
	});
}

inline void CozipMetadataOffsetFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vector = args.data[0];
	UnaryExecutor::Execute<string_t, uint64_t>(path_vector, result, args.size(), [&](string_t path) {
		auto idx = ParseCozip(path.GetString());
		for (auto &entry : idx.entries) {
			if (entry.name == "__metadata__") {
				return entry.offset;
			}
		}
		throw InvalidInputException("Priority entry '__metadata__' not found in archive: " + path.GetString());
	});
}

inline void CozipIntegrityHashOkFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vector = args.data[0];
	UnaryExecutor::Execute<string_t, bool>(path_vector, result, args.size(), [&](string_t path) {
		auto idx = ParseCozip(path.GetString());
		return idx.integrity_hash_stored == idx.integrity_hash_computed;
	});
}

inline void CozipIndexJsonFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &path_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(path_vector, result, args.size(), [&](string_t path) {
		auto idx = ParseCozip(path.GetString());

		std::ostringstream out;
		out << "{\"version\":" << idx.version << ",\"profile\":" << (uint64_t)idx.profile
		    << ",\"index_payload_size\":" << idx.index_payload_size << ",\"integrity_hash_ok\":"
		    << (idx.integrity_hash_stored == idx.integrity_hash_computed ? "true" : "false") << ",\"entries\":[";
		for (idx_t i = 0; i < idx.entries.size(); i++) {
			if (i > 0) {
				out << ",";
			}
			out << "{\"name\":\"" << JsonEscape(idx.entries[i].name) << "\",\"offset\":" << idx.entries[i].offset
			    << ",\"size\":" << idx.entries[i].size << "}";
		}
		out << "]}";

		return StringVector::AddString(result, out.str());
	});
}

struct CozipIndexBindData : public TableFunctionData {
	explicit CozipIndexBindData(CozipIndex index) : index(std::move(index)) {
	}

	CozipIndex index;
};

struct CozipIndexGlobalState : public GlobalTableFunctionState {
	idx_t offset = 0;
};

static duckdb::unique_ptr<FunctionData> CozipIndexBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("cozip_index path parameter cannot be NULL");
	}

	names.emplace_back("ordinal");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("name");
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("offset_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("size_bytes");
	return_types.emplace_back(LogicalType::UBIGINT);

	auto path = StringValue::Get(input.inputs[0]);
	return make_uniq<CozipIndexBindData>(ParseCozip(path));
}

static duckdb::unique_ptr<GlobalTableFunctionState> CozipIndexInit(ClientContext &context,
                                                                   TableFunctionInitInput &input) {
	return make_uniq<CozipIndexGlobalState>();
}

static void CozipIndexFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<CozipIndexBindData>();
	auto &state = (CozipIndexGlobalState &)*data_p.global_state;

	idx_t count = 0;
	while (state.offset < bind_data.index.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &entry = bind_data.index.entries[state.offset];
		output.SetValue(0, count, Value::UBIGINT(state.offset));
		output.SetValue(1, count, Value(entry.name));
		output.SetValue(2, count, Value::UBIGINT(entry.offset));
		output.SetValue(3, count, Value::UBIGINT(entry.size));
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.RegisterFunction(
	    ScalarFunction("cozip_priority_count", {LogicalType::VARCHAR}, LogicalType::UBIGINT, CozipPriorityCountFun));
	loader.RegisterFunction(
	    ScalarFunction("cozip_profile", {LogicalType::VARCHAR}, LogicalType::UBIGINT, CozipProfileFun));
	loader.RegisterFunction(
	    ScalarFunction("cozip_metadata_offset", {LogicalType::VARCHAR}, LogicalType::UBIGINT, CozipMetadataOffsetFun));
	loader.RegisterFunction(ScalarFunction("cozip_integrity_hash_ok", {LogicalType::VARCHAR}, LogicalType::BOOLEAN,
	                                       CozipIntegrityHashOkFun));
	loader.RegisterFunction(
	    ScalarFunction("cozip_tail_hash_ok", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, CozipIntegrityHashOkFun));
	loader.RegisterFunction(
	    ScalarFunction("cozip_index_json", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipIndexJsonFun));
	loader.RegisterFunction(
	    TableFunction("cozip_index", {LogicalType::VARCHAR}, CozipIndexFunction, CozipIndexBind, CozipIndexInit));
}

void CozipExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CozipExtension::Name() {
	return "cozip";
}

std::string CozipExtension::Version() const {
#ifdef EXT_VERSION_COZIP
	return EXT_VERSION_COZIP;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(cozip, loader) {
	duckdb::LoadInternal(loader);
}
}
