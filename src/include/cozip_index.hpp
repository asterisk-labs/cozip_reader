#pragma once

// Byte-0 index of a cozip archive: parsing and lookup.
//
// This is the only place in the extension that reads raw archive bytes.
// Everything above it works with names, offsets and sizes.

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

// cozip spec 5.1 and 7: the index payload starts at byte 51, right after a
// fixed 51-byte Local File Header for the "__cozip__" entry.
static constexpr idx_t COZIP_LFH_SIZE = 51;
static constexpr idx_t COZIP_INDEX_HEADER_SIZE = 11;
// name_len(2) + offset(8) + size(8) + at least one name byte
static constexpr idx_t COZIP_INDEX_PER_ENTRY_MIN = 19;
static constexpr idx_t COZIP_HASH_WINDOW_SIZE = 32768;
static constexpr idx_t COZIP_MIN_SIZE = COZIP_LFH_SIZE + COZIP_HASH_WINDOW_SIZE;
static constexpr idx_t COZIP_BOOTSTRAP_SIZE = 65536;

static constexpr uint8_t COZIP_PROFILE_NONE = 0;
static constexpr uint8_t COZIP_PROFILE_FLAT = 1;
static constexpr uint8_t COZIP_PROFILE_TACO = 2;

// One priority entry: a byte range of the archive holding a whole file.
struct CozipEntry {
	string name;
	uint64_t offset;
	uint64_t size;
};

struct CozipIndex {
	uint8_t profile;
	uint16_t version;
	vector<CozipEntry> entries;

	//! Byte range of a named entry, or nullptr when the index does not list it.
	const CozipEntry *Find(const string &name) const;
	//! "<offset>_<size>", the header half of a cozip-subfile:// path.
	string OffsetSize(const string &name, const string &source) const;
};

//! Profile byte only. Reads the 58-byte prefix, never the whole index.
uint8_t ReadCozipProfile(FileHandle &handle, const string &source);

//! Full index. One read of at most 64 KiB for a typical archive.
CozipIndex ReadCozipIndex(FileHandle &handle, const string &source);

//! Open `source` through the context filesystem and read its index.
CozipIndex ReadCozipIndex(ClientContext &context, const string &source);

//! Map a storage URL onto its GDAL virtual filesystem equivalent.
string BuildVsiBase(const string &path);

//! A cozip-subfile:// path addressing one byte range of `source`.
string SubFilePath(const string &source, uint64_t offset, uint64_t size);

//! Human name of a profile byte: none, flat, taco or unknown:<n>.
string ProfileName(uint8_t profile);

} // namespace duckdb
