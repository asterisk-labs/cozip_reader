// cozip-subfile:// VFS.
//
// WASM side-module constraint: this file avoids template paths whose
// instantiations duckdb-eh.wasm does not export — single-string throws,
// static_cast over NumericCast/Cast<T>, (*p).x over p->x for unique_ptr<FileHandle>.

#include "cozip_subfile_fs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/main/client_context.hpp"

#include <stdexcept>
#include <string>

namespace duckdb {

static constexpr size_t PREFIX_LEN = sizeof("cozip-subfile://") - 1;

bool CozipSubFileSystem::CanHandleFile(const string &fpath) {
	return fpath.size() > PREFIX_LEN && fpath.compare(0, PREFIX_LEN, "cozip-subfile://") == 0;
}

static uint64_t ParseDecimalUint64(const string &s, const string &field, const string &path) {
	if (s.empty()) {
		throw IOException("malformed cozip-subfile path (empty " + field + "): " + path);
	}
	for (auto c : s) {
		if (c < '0' || c > '9') {
			throw IOException("malformed cozip-subfile path (non-digit in " + field + "): " + path);
		}
	}
	try {
		size_t consumed = 0;
		auto v = std::stoull(s, &consumed, 10);
		if (consumed != s.size()) {
			throw IOException("malformed cozip-subfile path (trailing chars in " + field + "): " + path);
		}
		return v;
	} catch (std::out_of_range &) {
		throw IOException("malformed cozip-subfile path (" + field + " exceeds uint64): " + path);
	} catch (std::invalid_argument &) {
		throw IOException("malformed cozip-subfile path (could not parse " + field + "): " + path);
	}
}

unique_ptr<FileHandle> CozipSubFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                    optional_ptr<FileOpener> opener) {
	if (!flags.OpenForReading() || flags.OpenForWriting()) {
		throw IOException("cozip-subfile filesystem is read-only");
	}
	if (!opener) {
		throw IOException("cozip-subfile filesystem requires a FileOpener for context: " + path);
	}
	if (!CanHandleFile(path)) {
		throw IOException("not a cozip-subfile path: " + path);
	}

	auto stripped = path.substr(PREFIX_LEN);
	auto bang = stripped.find('!');
	if (bang == string::npos) {
		throw IOException("malformed cozip-subfile path (missing '!' separator): " + path);
	}
	auto header = stripped.substr(0, bang);
	auto underlying = stripped.substr(bang + 1);
	if (underlying.empty()) {
		throw IOException("malformed cozip-subfile path (empty underlying): " + path);
	}

	auto underscore = header.find('_');
	if (underscore == string::npos) {
		throw IOException("malformed cozip-subfile path (missing '_' separator): " + path);
	}
	auto offset = ParseDecimalUint64(header.substr(0, underscore), "offset", path);
	auto size = ParseDecimalUint64(header.substr(underscore + 1), "size", path);

	auto context = opener->TryGetClientContext();
	if (!context) {
		throw IOException("cozip-subfile filesystem requires a ClientContext: " + path);
	}

	auto &fs = FileSystem::GetFileSystem(*context);
	auto inner = fs.OpenFile(underlying, flags);
	if (!inner) {
		throw IOException("could not open underlying file: " + underlying);
	}
	if (!(*inner).CanSeek()) {
		throw IOException("underlying file is not seekable, cozip requires range reads: " + underlying);
	}

	return make_uniq<CozipSubFileHandle>(*this, path, flags, std::move(inner), static_cast<idx_t>(offset),
	                                     static_cast<idx_t>(size));
}

void CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = static_cast<CozipSubFileHandle &>(handle);
	if (location >= h.sub_size) {
		return;
	}
	auto to_read = MinValue(static_cast<idx_t>(nr_bytes), h.sub_size - location);
	if (to_read == 0) {
		return;
	}
	(*h.inner_handle).Read(buffer, static_cast<int64_t>(to_read), location + h.base_offset);
}

int64_t CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = static_cast<CozipSubFileHandle &>(handle);
	if (h.seek_pos >= h.sub_size) {
		return 0;
	}
	auto to_read = MinValue(static_cast<idx_t>(nr_bytes), h.sub_size - h.seek_pos);
	if (to_read == 0) {
		return 0;
	}
	(*h.inner_handle).Read(buffer, static_cast<int64_t>(to_read), h.seek_pos + h.base_offset);
	h.seek_pos += to_read;
	return static_cast<int64_t>(to_read);
}

int64_t CozipSubFileSystem::GetFileSize(FileHandle &handle) {
	return static_cast<int64_t>(static_cast<CozipSubFileHandle &>(handle).sub_size);
}

void CozipSubFileSystem::Seek(FileHandle &handle, idx_t location) {
	static_cast<CozipSubFileHandle &>(handle).seek_pos = location;
}

void CozipSubFileSystem::Reset(FileHandle &handle) {
	static_cast<CozipSubFileHandle &>(handle).seek_pos = 0;
}

idx_t CozipSubFileSystem::SeekPosition(FileHandle &handle) {
	return static_cast<CozipSubFileHandle &>(handle).seek_pos;
}

bool CozipSubFileSystem::OnDiskFile(FileHandle &handle) {
	auto &h = static_cast<CozipSubFileHandle &>(handle);
	return (*h.inner_handle).OnDiskFile();
}

FileType CozipSubFileSystem::GetFileType(FileHandle &handle) {
	auto &inner = *static_cast<CozipSubFileHandle &>(handle).inner_handle;
	return inner.file_system.GetFileType(inner);
}

timestamp_t CozipSubFileSystem::GetLastModifiedTime(FileHandle &handle) {
	auto &inner = *static_cast<CozipSubFileHandle &>(handle).inner_handle;
	return inner.file_system.GetLastModifiedTime(inner);
}

} // namespace duckdb
