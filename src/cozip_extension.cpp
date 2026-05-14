// read_cozip table macro: composes read_parquet over a cozip-subfile://
// virtual path resolved by CozipSubFileSystem. Plus two scalar helpers
// (cozip_offset_size, cozip_vsi_base) used by the macro body.
//
// WASM side-module constraint: avoid template paths whose instantiations
// duckdb-eh.wasm does not export — single-string throws, FlatVector loops
// (not UnaryExecutor), Parser+RegisterFunction (not Connection::Query).

#define DUCKDB_EXTENSION_MAIN

#include "cozip_extension.hpp"
#include "cozip_subfile_fs.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace duckdb {

static constexpr uint32_t ZIP_LFH_SIGNATURE = 0x04034B50U;
static constexpr idx_t COZIP_LFH_SIZE = 51;
static constexpr idx_t COZIP_INDEX_HEADER_SIZE = 11;
static constexpr idx_t COZIP_HASH_WINDOW_SIZE = 32768;
static constexpr idx_t COZIP_MIN_SIZE = COZIP_LFH_SIZE + COZIP_HASH_WINDOW_SIZE;
static constexpr idx_t COZIP_BOOTSTRAP_SIZE = 65536;
static constexpr uint16_t COZIP_EXTRA_HEADER_ID = 0xCA0C;
static constexpr uint8_t COZIP_PROFILE_FLAT = 1;
static constexpr uint16_t COZIP_FORMAT_VERSION = 1;

static const std::string COZIP_INDEX_NAME = "__cozip__";
static const std::string COZIP_METADATA_NAME = "__metadata__";

static inline uint16_t ReadU16LE(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t ReadU32LE(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ReadU64LE(const uint8_t *p) {
	return (uint64_t)ReadU32LE(p) | ((uint64_t)ReadU32LE(p + 4) << 32);
}

struct CozipMetadataEntry {
	uint64_t offset;
	uint64_t size;
};

static CozipMetadataEntry ParseCozipMetadataLocation(FileHandle &handle, const std::string &source) {
	auto file_size = (idx_t)handle.GetFileSize();
	if (file_size < COZIP_MIN_SIZE) {
		throw InvalidInputException(std::string("cozip archive too small (minimum is ") +
		                            std::to_string(COZIP_MIN_SIZE) + " bytes): " + source);
	}

	auto bootstrap = std::min<idx_t>(COZIP_BOOTSTRAP_SIZE, file_size);
	std::vector<uint8_t> head(bootstrap);
	handle.Read(head.data(), bootstrap, 0);

	if (ReadU32LE(head.data()) != ZIP_LFH_SIGNATURE) {
		throw InvalidInputException(std::string("byte 0 is not a ZIP Local File Header: ") + source);
	}
	auto name_len = ReadU16LE(head.data() + 26);
	auto extra_len = ReadU16LE(head.data() + 28);
	if (name_len != 9 || extra_len != 12) {
		throw InvalidInputException(std::string("LFH does not match cozip layout: ") + source);
	}
	if (memcmp(head.data() + 30, COZIP_INDEX_NAME.data(), 9) != 0) {
		throw InvalidInputException(std::string("first ZIP entry is not __cozip__: ") + source);
	}
	if (ReadU16LE(head.data() + 39) != COZIP_EXTRA_HEADER_ID) {
		throw InvalidInputException(std::string("cozip integrity extra field (0xCA0C) missing: ") + source);
	}

	auto index_payload_size = (idx_t)ReadU32LE(head.data() + 18);
	if (index_payload_size == 0) {
		throw InvalidInputException(std::string("cozip index payload size is zero: ") + source);
	}
	auto index_payload_end = COZIP_LFH_SIZE + index_payload_size;

	if (index_payload_end > head.size()) {
		auto extra = index_payload_end - head.size();
		std::vector<uint8_t> tail(extra);
		handle.Read(tail.data(), extra, head.size());
		head.insert(head.end(), tail.begin(), tail.end());
	}

	auto pp = head.data() + COZIP_LFH_SIZE;
	if (memcmp(pp, "CZIP", 4) != 0) {
		throw InvalidInputException(std::string("index payload magic is not 'CZIP': ") + source);
	}
	auto version = ReadU16LE(pp + 4);
	if (version > COZIP_FORMAT_VERSION) {
		throw InvalidInputException(std::string("unsupported cozip format version ") + std::to_string((int)version) +
		                            ": " + source);
	}
	auto profile = pp[6];
	if (profile != COZIP_PROFILE_FLAT) {
		throw InvalidInputException(std::string("read_cozip only supports the Flat profile (profile=1). Got profile=") +
		                            std::to_string((int)profile) + " in: " + source);
	}
	auto n_entries = (idx_t)ReadU32LE(pp + 7);

	auto cur = pp + COZIP_INDEX_HEADER_SIZE;
	std::vector<uint16_t> name_lens(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		name_lens[i] = ReadU16LE(cur);
		cur += 2;
	}
	std::vector<std::string> names(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		names[i] = std::string((const char *)cur, name_lens[i]);
		cur += name_lens[i];
	}
	std::vector<uint64_t> offsets(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		offsets[i] = ReadU64LE(cur);
		cur += 8;
	}
	std::vector<uint64_t> sizes(n_entries);
	for (idx_t i = 0; i < n_entries; i++) {
		sizes[i] = ReadU64LE(cur);
		cur += 8;
	}

	for (idx_t i = 0; i < n_entries; i++) {
		if (names[i] == COZIP_METADATA_NAME) {
			return CozipMetadataEntry {offsets[i], sizes[i]};
		}
	}
	throw InvalidInputException(std::string("Flat-profile cozip is missing __metadata__ entry: ") + source);
}

static std::string BuildVsiBase(const std::string &path) {
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
		std::string ns_prefix;
		if (StringUtil::StartsWith(rest, "datasets/")) {
			ns_prefix = "datasets/";
			rest = rest.substr(9);
		} else if (StringUtil::StartsWith(rest, "spaces/")) {
			ns_prefix = "spaces/";
			rest = rest.substr(7);
		}
		auto sl1 = rest.find('/');
		auto sl2 = (sl1 == std::string::npos) ? std::string::npos : rest.find('/', sl1 + 1);
		if (sl1 == std::string::npos || sl2 == std::string::npos) {
			throw InvalidInputException(std::string("cannot parse hf:// URL for VSI mapping: ") + path);
		}
		auto owner_repo = rest.substr(0, sl2);
		auto inner = rest.substr(sl2 + 1);
		return "/vsicurl/https://huggingface.co/" + ns_prefix + owner_repo + "/resolve/main/" + inner;
	}
	return path;
}

template <typename Body>
static void StringScalarLoop(DataChunk &args, Vector &result, const char *fn, Body body) {
	auto count = args.size();
	args.data[0].Flatten(count);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto src = FlatVector::GetData<string_t>(args.data[0]);
	auto dst = FlatVector::GetData<string_t>(result);
	auto &src_valid = FlatVector::Validity(args.data[0]);

	for (idx_t i = 0; i < count; i++) {
		if (!src_valid.RowIsValid(i)) {
			throw InvalidInputException(std::string(fn) + ": path argument is NULL");
		}
		dst[i] = StringVector::AddString(result, body(std::string(src[i].GetString())));
	}
}

static void CozipOffsetSizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &fs = FileSystem::GetFileSystem(state.GetContext());
	StringScalarLoop(args, result, "cozip_offset_size", [&](const std::string &path) {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		if (!handle) {
			throw IOException(std::string("cozip_offset_size: could not open ") + path);
		}
		auto md = ParseCozipMetadataLocation(*handle, path);
		return std::to_string(md.offset) + "_" + std::to_string(md.size);
	});
}

static void CozipVsiBaseFunction(DataChunk &args, ExpressionState &, Vector &result) {
	StringScalarLoop(args, result, "cozip_vsi_base", BuildVsiBase);
}

// The macro always emits a cozip:gdal_vsi column. When gdal_vsi is false
// the value is NULL; CASE short-circuits so cozip_vsi_base is not called.
static const char *kReadCozipMacro = R"sql(
CREATE OR REPLACE MACRO read_cozip(p, gdal_vsi := true) AS TABLE
SELECT *,
  CASE WHEN gdal_vsi
       THEN '/vsisubfile/' || "offset" || '_' || "size" || ',' || cozip_vsi_base(p)
       ELSE NULL
  END AS "cozip:gdal_vsi"
FROM read_parquet('cozip-subfile://' || cozip_offset_size(p) || '!' || p);
)sql";

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	db.GetFileSystem().RegisterSubSystem(make_uniq<CozipSubFileSystem>());

	ScalarFunction offset_size_fn("cozip_offset_size", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                              CozipOffsetSizeFunction);
	loader.RegisterFunction(offset_size_fn);

	ScalarFunction vsi_base_fn("cozip_vsi_base", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipVsiBaseFunction);
	loader.RegisterFunction(vsi_base_fn);

	// schema="main" + internal=true are required: ExtensionLoader installs
	// into the system catalog, which only accepts internal entries, and
	// the Parser leaves both fields default. Built-in DuckDB macros set
	// both the same way.
	Parser parser;
	parser.ParseQuery(kReadCozipMacro);
	if (parser.statements.empty()) {
		throw IOException("cozip: read_cozip macro SQL produced no statements");
	}
	auto &create_stmt = static_cast<CreateStatement &>(*parser.statements[0]);
	auto &macro_info = static_cast<CreateMacroInfo &>(*create_stmt.info);
	macro_info.schema = "main";
	macro_info.internal = true;
	loader.RegisterFunction(macro_info);
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
