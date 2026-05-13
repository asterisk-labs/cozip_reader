#include "cozip_subfile_fs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"

#include <stdexcept>
#include <string>

namespace duckdb {

// Length of the literal "cozip-subfile://" (16 chars). sizeof-1 to strip the
// trailing NUL that the string literal carries.
static constexpr size_t PREFIX_LEN = sizeof("cozip-subfile://") - 1;

bool CozipSubFileSystem::CanHandleFile(const string &fpath) {
	return fpath.size() > PREFIX_LEN && fpath.compare(0, PREFIX_LEN, "cozip-subfile://") == 0;
}

// Parse a uint64 from a decimal-only ASCII string. The whole string must be
// consumed and must consist of digits only; std::stoull on its own would happily
// accept leading whitespace or stop early on a non-digit, both of which would
// silently produce a wrong offset/size.
static uint64_t ParseDecimalUint64(const string &s, const string &field, const string &path) {
	if (s.empty()) {
		throw IOException("malformed cozip-subfile path (empty %s): %s", field, path);
	}
	for (auto c : s) {
		if (c < '0' || c > '9') {
			throw IOException("malformed cozip-subfile path (non-digit in %s): %s", field, path);
		}
	}
	try {
		size_t consumed = 0;
		auto v = std::stoull(s, &consumed, 10);
		if (consumed != s.size()) {
			throw IOException("malformed cozip-subfile path (trailing chars in %s): %s", field, path);
		}
		return v;
	} catch (std::out_of_range &) {
		throw IOException("malformed cozip-subfile path (%s exceeds uint64): %s", field, path);
	} catch (std::invalid_argument &) {
		throw IOException("malformed cozip-subfile path (could not parse %s): %s", field, path);
	}
}

unique_ptr<FileHandle> CozipSubFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                    optional_ptr<FileOpener> opener) {
	if (!flags.OpenForReading() || flags.OpenForWriting()) {
		throw IOException("cozip-subfile filesystem is read-only");
	}
	if (!opener) {
		throw IOException("cozip-subfile filesystem requires a FileOpener for context: %s", path);
	}
	if (!CanHandleFile(path)) {
		throw IOException("not a cozip-subfile path: %s", path);
	}

	// Layout: cozip-subfile://<offset>_<size>!<underlying_path>
	auto stripped = path.substr(PREFIX_LEN);
	auto bang = stripped.find('!');
	if (bang == string::npos) {
		throw IOException("malformed cozip-subfile path (missing '!' separator): %s", path);
	}
	auto header = stripped.substr(0, bang);
	auto underlying = stripped.substr(bang + 1);
	if (underlying.empty()) {
		throw IOException("malformed cozip-subfile path (empty underlying): %s", path);
	}

	auto underscore = header.find('_');
	if (underscore == string::npos) {
		throw IOException("malformed cozip-subfile path (missing '_' separator): %s", path);
	}
	auto offset = ParseDecimalUint64(header.substr(0, underscore), "offset", path);
	auto size = ParseDecimalUint64(header.substr(underscore + 1), "size", path);

	auto context = opener->TryGetClientContext();
	if (!context) {
		throw IOException("cozip-subfile filesystem requires a ClientContext: %s", path);
	}

	auto &fs = FileSystem::GetFileSystem(*context);
	auto inner = fs.OpenFile(underlying, flags);
	if (!inner) {
		throw IOException("could not open underlying file: %s", underlying);
	}
	if (!inner->CanSeek()) {
		throw IOException("underlying file is not seekable, cozip requires range reads: %s", underlying);
	}

	return make_uniq<CozipSubFileHandle>(*this, path, flags, std::move(inner), NumericCast<idx_t>(offset),
	                                     NumericCast<idx_t>(size));
}

// Positioned read. ParquetReader uses this for footer fetches and parallel
// row group prefetching. Translate location into the underlying file and clamp
// to sub_size so we never leak bytes past the embedded payload.
void CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = handle.Cast<CozipSubFileHandle>();
	if (location >= h.sub_size) {
		return;
	}
	auto remaining = h.sub_size - location;
	auto to_read = MinValue(NumericCast<idx_t>(nr_bytes), remaining);
	if (to_read == 0) {
		return;
	}
	h.inner_handle->Read(buffer, NumericCast<int64_t>(to_read), location + h.base_offset);
}

// Sequential read from the current seek position. Same translation as above,
// plus we advance the wrapper's own seek_pos. inner_handle's seek position
// stays untouched because we always go through pread-style positioned reads.
int64_t CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = handle.Cast<CozipSubFileHandle>();
	if (h.seek_pos >= h.sub_size) {
		return 0;
	}
	auto remaining = h.sub_size - h.seek_pos;
	auto to_read = MinValue(NumericCast<idx_t>(nr_bytes), remaining);
	if (to_read == 0) {
		return 0;
	}
	h.inner_handle->Read(buffer, NumericCast<int64_t>(to_read), h.seek_pos + h.base_offset);
	h.seek_pos += to_read;
	return NumericCast<int64_t>(to_read);
}

int64_t CozipSubFileSystem::GetFileSize(FileHandle &handle) {
	return NumericCast<int64_t>(handle.Cast<CozipSubFileHandle>().sub_size);
}

void CozipSubFileSystem::Seek(FileHandle &handle, idx_t location) {
	// No clamp here: positioned reads clamp on their own, matching the
	// pattern used by ZipFileSystem and the local file system.
	handle.Cast<CozipSubFileHandle>().seek_pos = location;
}

void CozipSubFileSystem::Reset(FileHandle &handle) {
	handle.Cast<CozipSubFileHandle>().seek_pos = 0;
}

idx_t CozipSubFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<CozipSubFileHandle>().seek_pos;
}

// Metadata about the wrapped subfile is delegated to the underlying handle:
// the subfile lives inside it, so on-disk-ness, file type, and last-modified
// time are all properties of the archive containing it, not of the slice.
bool CozipSubFileSystem::OnDiskFile(FileHandle &handle) {
	return handle.Cast<CozipSubFileHandle>().inner_handle->OnDiskFile();
}

FileType CozipSubFileSystem::GetFileType(FileHandle &handle) {
	auto &h = handle.Cast<CozipSubFileHandle>();
	auto &inner = *h.inner_handle;
	return inner.file_system.GetFileType(inner);
}

timestamp_t CozipSubFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &h = handle.Cast<CozipSubFileHandle>();
	auto &inner = *h.inner_handle;
	return inner.file_system.GetLastModifiedTime(inner);
}

} // namespace duckdb
