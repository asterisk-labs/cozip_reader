// The cozip DuckDB extension.
//
// The Flat reader is a SQL macro over read_parquet. C++ only reads the cozip
// index at byte 0; DuckDB handles the columnar work.

#define DUCKDB_EXTENSION_MAIN

#include "cozip_extension.hpp"
#include "cozip_index.hpp"
#include "cozip_subfile_fs.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/vector.hpp"
#if __has_include("duckdb/common/vector/list_vector.hpp")
#define COZIP_DUCKDB_SPLIT_VECTOR_HEADERS 1
#endif
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

#include <string>

namespace duckdb {

static const char *FLAT_METADATA_NAME = "__metadata__";

static unique_ptr<FileHandle> OpenSource(ClientContext &context, const string &path, const char *function_name) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("%s: could not open %s", function_name, path);
	}
	return handle;
}

//! Applies `body` to every row of a single VARCHAR argument.
template <typename Body>
static void StringScalarLoop(DataChunk &args, Vector &result, const char *function_name, Body body) {
	auto count = args.size();
	auto constant = args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR;
	args.data[0].Flatten(count);
	result.SetVectorType(VectorType::FLAT_VECTOR);

	auto source = FlatVector::GetData<string_t>(args.data[0]);
	auto &validity = FlatVector::Validity(args.data[0]);

	for (idx_t i = 0; i < (constant ? 1 : count); i++) {
		if (!validity.RowIsValid(i)) {
			throw InvalidInputException("%s: path argument is NULL", function_name);
		}
		result.SetValue(i, Value(body(source[i].GetString())));
	}
	if (constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// Locates the Flat manifest. Kept as its own scalar because the read_flat
// macro splices the result straight into a cozip-subfile:// path.
static void CozipOffsetSizeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringScalarLoop(args, result, "cozip_offset_size", [&](const string &path) {
		auto handle = OpenSource(context, path, "cozip_offset_size");
		auto index = ReadCozipIndex(*handle, path);
		if (index.profile != COZIP_PROFILE_FLAT) {
			throw InvalidInputException("read_flat needs a Flat-profile archive (profile=1). Got profile=%d in: %s",
			                            (int)index.profile, path);
		}
		return index.OffsetSize(FLAT_METADATA_NAME, path);
	});
}

static void CozipProfileFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringScalarLoop(args, result, "cozip_profile", [&](const string &path) {
		auto handle = OpenSource(context, path, "cozip_profile");
		return ReadCozipProfile(*handle, path);
	});
}

static void CozipVsiBaseFunction(DataChunk &args, ExpressionState &, Vector &result) {
	StringScalarLoop(args, result, "cozip_vsi_base", BuildVsiBase);
}

// One row per archive entry, plus where its payload can be read from.
static const char *FLAT_MACRO_BODY = R"sql(
SELECT COLUMNS(lambda c: c != 'cozip:location' AND c != 'cozip:gdal_vsi'),
  CASE WHEN coalesce(location, gdal_vsi, true)
       THEN '/vsisubfile/' || "offset" || '_' || "size" || ',' || cozip_vsi_base(p)
       ELSE NULL
  END AS "cozip:location"
FROM read_parquet('cozip-subfile://' || cozip_offset_size(p) || '!' || p)
)sql";

static const char *LEGACY_FLAT_MACRO_BODY = R"sql(
SELECT COLUMNS(lambda c: c != 'cozip:location' AND c != 'cozip:gdal_vsi'),
  CASE WHEN gdal_vsi
       THEN '/vsisubfile/' || "offset" || '_' || "size" || ',' || cozip_vsi_base(p)
       ELSE NULL
  END AS "cozip:gdal_vsi"
FROM read_parquet('cozip-subfile://' || cozip_offset_size(p) || '!' || p)
)sql";

//! Registers one table macro. ExtensionLoader installs into the system
//! catalog, which only accepts internal entries in the main schema; the
//! Parser leaves both fields default, so they are set here.
static void RegisterTableMacro(ExtensionLoader &loader, const string &signature, const string &body) {
	Parser parser;
	parser.ParseQuery("CREATE OR REPLACE MACRO " + signature + " AS TABLE " + body + ";");
	if (parser.statements.empty()) {
		throw IOException("cozip: macro SQL produced no statements: %s", signature);
	}
	auto &create_statement = static_cast<CreateStatement &>(*parser.statements[0]);
	auto &macro_info = static_cast<CreateMacroInfo &>(*create_statement.info);
#ifdef COZIP_DUCKDB_SPLIT_VECTOR_HEADERS
	macro_info.SetSchema("main");
#else
	macro_info.schema = "main";
#endif
	macro_info.internal = true;
	loader.RegisterFunction(macro_info);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	db.GetFileSystem().RegisterSubSystem(make_uniq<CozipSubFileSystem>());

	ScalarFunction cozip_offset_size("cozip_offset_size", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                                 CozipOffsetSizeFunction);
	cozip_offset_size.SetFallible();
	loader.RegisterFunction(cozip_offset_size);
	ScalarFunction cozip_profile("cozip_profile", {LogicalType::VARCHAR}, LogicalType::UTINYINT, CozipProfileFunction);
	cozip_profile.SetFallible();
	loader.RegisterFunction(cozip_profile);
	ScalarFunction cozip_vsi_base("cozip_vsi_base", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipVsiBaseFunction);
	cozip_vsi_base.SetFallible();
	loader.RegisterFunction(cozip_vsi_base);
	RegisterTableMacro(loader, "read_flat(p, location := NULL, gdal_vsi := NULL)", FLAT_MACRO_BODY);
	RegisterTableMacro(loader, "read_cozip(p, gdal_vsi := true)", LEGACY_FLAT_MACRO_BODY);
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
