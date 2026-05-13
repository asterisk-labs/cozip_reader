#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"

namespace duckdb {

// A FileHandle that exposes a contiguous byte range [base_offset, base_offset +
// sub_size) of an underlying FileHandle as if it were a standalone file of
// size sub_size starting at byte 0.
//
// All reads are translated and delegated to inner_handle. No buffering, no
// extra IO at open time. Used internally by read_cozip to expose the
// __metadata__ Parquet payload embedded inside a cozip archive directly to
// ParquetReader, so range requests against the original archive (HTTP, S3,
// hf://, ...) flow through transparently and lazily.
class CozipSubFileHandle final : public FileHandle {
	friend class CozipSubFileSystem;

public:
	CozipSubFileHandle(FileSystem &file_system, const string &path, FileOpenFlags flags,
	                   unique_ptr<FileHandle> inner_handle_p, idx_t base_offset_p, idx_t sub_size_p)
	    : FileHandle(file_system, path, flags), inner_handle(std::move(inner_handle_p)), base_offset(base_offset_p),
	      sub_size(sub_size_p), seek_pos(0) {
	}

	void Close() override {
		if (inner_handle) {
			inner_handle->Close();
		}
	}

private:
	unique_ptr<FileHandle> inner_handle;
	idx_t base_offset;
	idx_t sub_size;
	idx_t seek_pos;
};

// FileSystem that handles the internal "cozip-subfile://" URL scheme.
//
// Path layout:
//   cozip-subfile://<offset>_<size>!<underlying_path>
//
//   <offset>           uint64, ASCII decimal, byte offset within underlying
//   <size>             uint64, ASCII decimal, payload size in bytes
//   <underlying_path>  any path that the surrounding VFS can OpenFile()
//                      (e.g. /local/file.zip, https://..., s3://..., hf://...)
//
// The "!" separator cannot appear in <offset>_<size> and is rare in URLs;
// the scheme is internal to the cozip extension and only ever constructed by
// read_cozip's bind, never written by users.
class CozipSubFileSystem final : public FileSystem {
public:
	CozipSubFileSystem() : FileSystem() {
	}

	std::string GetName() const override {
		return "CozipSubFileSystem";
	}
	bool CanHandleFile(const string &fpath) override;
	bool CanSeek() override {
		return true;
	}

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags, optional_ptr<FileOpener> opener) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	void Reset(FileHandle &handle) override;
	idx_t SeekPosition(FileHandle &handle) override;

	bool OnDiskFile(FileHandle &handle) override;
	FileType GetFileType(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;

	// The scheme is internal; glob and exists are never meaningful externally.
	// Return trivial answers based on the prefix instead of throwing, so any
	// VFS probing from inside DuckDB does not error out.
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener) override {
		if (path.size() >= 16 && path.substr(0, 16) == "cozip-subfile://") {
			return {OpenFileInfo(path)};
		}
		return {};
	}
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener) override {
		return filename.size() >= 16 && filename.substr(0, 16) == "cozip-subfile://";
	}
};

} // namespace duckdb
