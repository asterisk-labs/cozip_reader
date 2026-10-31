// cozip-subfile:// and /vsisubfile/ VFS.
//
// WASM side-module constraint: this file avoids template paths whose
// instantiations duckdb-eh.wasm does not export — single-string throws,
// static_cast over NumericCast/Cast<T>, (*p).x over p->x for unique_ptr<FileHandle>.

#include "cozip_subfile_fs.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace duckdb {

static constexpr std::string_view COZIP_PREFIX = "cozip-subfile://";
static constexpr std::string_view GDAL_PREFIX = "/vsisubfile/";

static bool HasPrefix(const string &path, std::string_view prefix) {
	return path.size() > prefix.size() && path.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0;
}

bool CozipSubFileSystem::CanHandleFile(const string &fpath) {
	return HasPrefix(fpath, COZIP_PREFIX) || HasPrefix(fpath, GDAL_PREFIX);
}

static uint64_t ParseDecimalUint64(const string &s, const string &field, const string &path) {
	if (s.empty()) {
		throw IOException("malformed subfile path (empty " + field + "): " + path);
	}
	for (auto c : s) {
		if (c < '0' || c > '9') {
			throw IOException("malformed subfile path (non-digit in " + field + "): " + path);
		}
	}
	try {
		return std::stoull(s);
	} catch (std::out_of_range &) {
		throw IOException("malformed subfile path (" + field + " exceeds uint64): " + path);
	}
}

struct SubFileRange {
	string underlying;
	uint64_t offset = 0;
	uint64_t size = 0;
	bool to_end = false;
};

static SubFileRange ParseCozipPath(const string &path) {
	auto stripped = path.substr(COZIP_PREFIX.size());
	auto bang = stripped.find('!');
	if (bang == string::npos) {
		throw IOException("malformed subfile path (missing '!' separator): " + path);
	}
	auto header = stripped.substr(0, bang);
	SubFileRange range;
	range.underlying = stripped.substr(bang + 1);
	if (range.underlying.empty()) {
		throw IOException("malformed subfile path (empty underlying): " + path);
	}
	auto underscore = header.find('_');
	if (underscore == string::npos) {
		throw IOException("malformed subfile path (missing '_' separator): " + path);
	}
	range.offset = ParseDecimalUint64(header.substr(0, underscore), "offset", path);
	range.size = ParseDecimalUint64(header.substr(underscore + 1), "size", path);
	return range;
}

static string DuckDBPathFromGdal(const string &gdal_path, const string &path) {
	static const char *const PREFIXES[][2] = {{"/vsicurl/", ""},
	                                          {"/vsis3/", "s3://"},
	                                          {"/vsigs/", "gs://"},
	                                          {"/vsiaz/", "az://"},
	                                          {"/vsiadls/", "abfss://"},
	                                          {"/vsihf/", "hf://"},
	                                          {"/vsisource/", "https://data.source.coop/"}};
	if (HasPrefix(gdal_path, GDAL_PREFIX)) {
		return gdal_path;
	}
	for (auto &prefix : PREFIXES) {
		if (HasPrefix(gdal_path, prefix[0])) {
			return prefix[1] + gdal_path.substr(std::string_view(prefix[0]).size());
		}
	}
	if (gdal_path.compare(0, 4, "/vsi") == 0) {
		throw IOException("unsupported GDAL file system in /vsisubfile/ path: " + path);
	}
	return gdal_path;
}

static SubFileRange ParseGdalPath(const string &path) {
	auto stripped = path.substr(GDAL_PREFIX.size());
	auto comma = stripped.find(',');
	if (comma == string::npos) {
		throw IOException("malformed subfile path (missing ',' separator): " + path);
	}
	auto header = stripped.substr(0, comma);
	auto gdal_underlying = stripped.substr(comma + 1);
	if (gdal_underlying.empty()) {
		throw IOException("malformed subfile path (empty underlying): " + path);
	}
	SubFileRange range;
	auto underscore = header.find('_');
	range.offset = ParseDecimalUint64(header.substr(0, underscore), "offset", path);
	if (underscore != string::npos) {
		range.size = ParseDecimalUint64(header.substr(underscore + 1), "size", path);
	}
	range.to_end = range.size == 0;
	range.underlying = DuckDBPathFromGdal(gdal_underlying, path);
	return range;
}

// Follow nested views to the actual file. OpenFile reports malformed paths.
static bool ReadsRemoteFile(const string &path) {
	auto current = path;
	try {
		while (HasPrefix(current, COZIP_PREFIX) || HasPrefix(current, GDAL_PREFIX)) {
			if (HasPrefix(current, COZIP_PREFIX)) {
				current = ParseCozipPath(current).underlying;
			} else {
				current = ParseGdalPath(current).underlying;
			}
		}
	} catch (IOException &) {
		return false;
	}
	return FileSystem::IsRemoteFile(current);
}

// DuckDB does not classify subfile paths as remote. Carry the cache override
// through Glob so VALIDATE_REMOTE still checks the underlying file.
vector<OpenFileInfo> CozipSubFileSystem::Glob(const string &path, FileOpener *opener) {
	if (!CanHandleFile(path)) {
		return {};
	}
	OpenFileInfo info(path);
	auto context = opener ? opener->TryGetClientContext() : nullptr;
	Value mode;
	if (context && (*context).TryGetCurrentSetting("validate_external_file_cache", mode) &&
	    StringUtil::CIEquals(mode.ToString(), "VALIDATE_REMOTE") && ReadsRemoteFile(path)) {
		info.extended_info = make_shared_ptr<ExtendedOpenFileInfo>();
		info.extended_info->options["validate_external_file_cache"] = Value::BOOLEAN(true);
	}
	return {info};
}

// Avoid httpfs's 1 MiB read-ahead for remote ranges. Parquet needs that
// buffer when its own prefetching is disabled.
static FileOpenFlags InnerFlags(ClientContext &context, const string &underlying, FileOpenFlags flags) {
	if (!FileSystem::IsRemoteFile(underlying)) {
		return flags;
	}
	Value disable_prefetch = Value::BOOLEAN(false);
	context.TryGetCurrentSetting("disable_parquet_prefetching", disable_prefetch);
	if (!disable_prefetch.IsNull() && BooleanValue::Get(disable_prefetch)) {
		return flags;
	}
	flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
	return flags;
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
		throw IOException("not a subfile path: " + path);
	}

	auto range = HasPrefix(path, COZIP_PREFIX) ? ParseCozipPath(path) : ParseGdalPath(path);
	auto &underlying = range.underlying;

	auto context = opener->TryGetClientContext();
	if (!context) {
		throw IOException("cozip-subfile filesystem requires a ClientContext: " + path);
	}

	auto &fs = FileSystem::GetFileSystem(*context);
	auto inner = fs.OpenFile(underlying, InnerFlags(*context, underlying, flags));
	if (!inner) {
		throw IOException("could not open underlying file: " + underlying);
	}
	if (!(*inner).CanSeek()) {
		throw IOException("underlying file is not seekable, cozip requires range reads: " + underlying);
	}
	auto underlying_size = static_cast<uint64_t>((*inner).GetFileSize());
	if (range.offset <= underlying_size && range.to_end) {
		range.size = underlying_size - range.offset;
	}
	if (range.offset > underlying_size || range.size > underlying_size - range.offset) {
		throw IOException("subfile range is outside the underlying file of " + std::to_string(underlying_size) +
		                  " bytes: " + path);
	}

	return make_uniq<CozipSubFileHandle>(*this, path, flags, std::move(inner), static_cast<idx_t>(range.offset),
	                                     static_cast<idx_t>(range.size));
}

void CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &h = static_cast<CozipSubFileHandle &>(handle);
	if (nr_bytes < 0 || location > h.sub_size || static_cast<uint64_t>(nr_bytes) > h.sub_size - location) {
		throw IOException("cannot read outside cozip subfile: " + handle.path);
	}
	if (nr_bytes == 0) {
		return;
	}
	(*h.inner_handle).Read(buffer, nr_bytes, location + h.base_offset);
}

int64_t CozipSubFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &h = static_cast<CozipSubFileHandle &>(handle);
	if (nr_bytes < 0) {
		throw IOException("cannot read a negative byte count from cozip subfile: " + handle.path);
	}
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

string CozipSubFileSystem::GetVersionTag(FileHandle &handle) {
	auto &inner = *static_cast<CozipSubFileHandle &>(handle).inner_handle;
	return inner.file_system.GetVersionTag(inner);
}

} // namespace duckdb
