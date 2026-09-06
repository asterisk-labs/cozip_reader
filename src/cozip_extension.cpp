// The cozip DuckDB extension.
//
// Two readers, one per cozip profile:
//   read_flat(path)   profile 1, the __metadata__ manifest, one row per entry
//   read_taco(path)   profile 2, a TACO dataset, one row per sample
//
// Both are SQL macros over read_parquet. C++ only reads bytes at fixed
// offsets: the byte-0 index, and COLLECTION.json. No Parquet is ever parsed
// here; DuckDB handles the columnar work.

#define DUCKDB_EXTENSION_MAIN

#include "cozip_extension.hpp"
#include "cozip_index.hpp"
#include "cozip_subfile_fs.hpp"
#include "taco_sql.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/statement/create_statement.hpp"

#include <string>
#include <vector>

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
	auto target = FlatVector::GetData<string_t>(result);
	auto &validity = FlatVector::Validity(args.data[0]);

	for (idx_t i = 0; i < (constant ? 1 : count); i++) {
		if (!validity.RowIsValid(i)) {
			throw InvalidInputException("%s: path argument is NULL", function_name);
		}
		target[i] = StringVector::AddString(result, body(source[i].GetString()));
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
			throw InvalidInputException("read_flat needs a Flat-profile archive (profile=1). Got profile=%s in: %s. "
			                            "Use read_taco() for a TACO dataset.",
			                            ProfileName(index.profile), path);
		}
		return index.OffsetSize(FLAT_METADATA_NAME, path);
	});
}

static void CozipProfileFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringScalarLoop(args, result, "cozip_profile", [&](const string &path) {
		auto handle = OpenSource(context, path, "cozip_profile");
		return ProfileName(ReadCozipProfile(*handle, path));
	});
}

static void CozipVsiBaseFunction(DataChunk &args, ExpressionState &, Vector &result) {
	StringScalarLoop(args, result, "cozip_vsi_base", BuildVsiBase);
}

static void TacoCollectionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringScalarLoop(args, result, "taco_collection", [&](const string &path) {
		auto layout = ResolveTacoLayout(context, path);
		ReadTacoContract(context, layout);
		auto handle = OpenSource(context, layout.collection_uri, "taco_collection");
		auto size = (idx_t)handle->GetFileSize();
		string content(size, '\0');
		if (size > 0) {
			handle->Read((void *)content.data(), size, 0);
		}
		return content;
	});
}

//! Fills one LIST(VARCHAR) result row from a vector of strings.
static void SetStringList(Vector &result, idx_t row, const vector<string> &values) {
	auto entries = FlatVector::GetData<list_entry_t>(result);
	entries[row].offset = ListVector::GetListSize(result);
	entries[row].length = values.size();
	ListVector::Reserve(result, entries[row].offset + values.size());
	auto &child = ListVector::GetEntry(result);
	auto child_data = FlatVector::GetData<string_t>(child);
	for (idx_t i = 0; i < values.size(); i++) {
		child_data[entries[row].offset + i] = StringVector::AddString(child, values[i]);
	}
	ListVector::SetListSize(result, entries[row].offset + values.size());
}

template <typename Body>
static void StringListScalarLoop(DataChunk &args, Vector &result, const char *function_name, Body body) {
	auto count = args.size();
	auto constant = args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR;
	args.data[0].Flatten(count);
	result.SetVectorType(VectorType::FLAT_VECTOR);
	ListVector::SetListSize(result, 0);

	auto source = FlatVector::GetData<string_t>(args.data[0]);
	auto &validity = FlatVector::Validity(args.data[0]);
	for (idx_t i = 0; i < (constant ? 1 : count); i++) {
		if (!validity.RowIsValid(i)) {
			throw InvalidInputException("%s: path argument is NULL", function_name);
		}
		SetStringList(result, i, body(source[i].GetString()));
	}
	if (constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

static void TacoStructureFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringListScalarLoop(args, result, "taco_structure", [&](const string &path) {
		auto layout = ResolveTacoLayout(context, path);
		return ReadTacoStructure(context, layout);
	});
}

static void TacoLevelsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringListScalarLoop(args, result, "taco_levels", [&](const string &path) {
		auto layout = ResolveTacoLayout(context, path);
		ReadTacoContract(context, layout);
		return layout.level_names;
	});
}

static void TacoDerivedFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	StringListScalarLoop(args, result, "taco_derived", [&](const string &path) {
		auto layout = ResolveTacoLayout(context, path);
		return ReadTacoContract(context, layout).derived;
	});
}

//! Reads one optional VARCHAR argument, returning "" when it is NULL.
static string OptionalString(DataChunk &args, idx_t column, idx_t row) {
	auto &vector = args.data[column];
	UnifiedVectorFormat format;
	vector.ToUnifiedFormat(args.size(), format);
	auto index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(index)) {
		return string();
	}
	return UnifiedVectorFormat::GetData<string_t>(format)[index].GetString();
}

static bool OptionalBool(DataChunk &args, idx_t column, idx_t row, bool fallback) {
	UnifiedVectorFormat format;
	args.data[column].ToUnifiedFormat(args.size(), format);
	auto index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(index)) {
		return fallback;
	}
	return UnifiedVectorFormat::GetData<bool>(format)[index];
}

static bool OptionalStringList(DataChunk &args, idx_t column, idx_t row, vector<string> &out) {
	auto &vector = args.data[column];
	UnifiedVectorFormat format;
	vector.ToUnifiedFormat(args.size(), format);
	auto index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(index)) {
		return false;
	}
	auto entry = UnifiedVectorFormat::GetData<list_entry_t>(format)[index];
	auto &child = ListVector::GetEntry(vector);
	UnifiedVectorFormat child_format;
	child.ToUnifiedFormat(ListVector::GetListSize(vector), child_format);
	auto child_data = UnifiedVectorFormat::GetData<string_t>(child_format);
	for (idx_t i = 0; i < entry.length; i++) {
		auto child_index = child_format.sel->get_index(entry.offset + i);
		if (!child_format.validity.RowIsValid(child_index)) {
			throw InvalidInputException("read_taco: files must not contain NULL");
		}
		out.push_back(child_data[child_index].GetString());
	}
	return true;
}

// Resolves a TACO container and returns the query used by read_taco.
static void TacoSqlFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto count = args.size();
	bool constant = true;
	for (idx_t i = 0; i < args.ColumnCount(); i++) {
		constant = constant && args.data[i].GetVectorType() == VectorType::CONSTANT_VECTOR;
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	args.data[0].Flatten(count);
	auto paths = FlatVector::GetData<string_t>(args.data[0]);
	auto &path_validity = FlatVector::Validity(args.data[0]);
	auto target = FlatVector::GetData<string_t>(result);

	for (idx_t row = 0; row < (constant ? 1 : count); row++) {
		if (!path_validity.RowIsValid(row)) {
			throw InvalidInputException("read_taco: path argument is NULL");
		}
		TacoOptions options;
		options.idx = OptionalString(args, 1, row);
		options.level = OptionalString(args, 2, row);
		options.pivot = OptionalBool(args, 3, row, true);
		options.has_files = OptionalStringList(args, 4, row, options.files);
		options.gdal_vsi = OptionalBool(args, 5, row, true);
		target[row] = StringVector::AddString(result, BuildTacoSQL(context, paths[row].GetString(), options));
	}
	if (constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// read_flat keeps the shape read_cozip has always had: one row per archive
// entry, plus the GDAL path. read_cozip stays as a deprecated alias.
static const char *FLAT_MACRO_BODY = R"sql(
SELECT *,
  CASE WHEN gdal_vsi
       THEN '/vsisubfile/' || "offset" || '_' || "size" || ',' || cozip_vsi_base(p)
       ELSE NULL
  END AS "cozip:gdal_vsi"
FROM read_parquet('cozip-subfile://' || cozip_offset_size(p) || '!' || p)
)sql";

static const char *TACO_MACRO_BODY = R"sql(
SELECT * FROM query(taco_sql(p, CAST(idx AS VARCHAR), level, pivoted, files, gdal_vsi))
)sql";

static const char *CONTRACT_MACRO_BODY = R"sql(
SELECT 'structure' AS kind, unnest(taco_structure(p)) AS value
UNION ALL
SELECT 'level' AS kind, unnest(taco_levels(p)) AS value
UNION ALL
SELECT 'derived' AS kind, unnest(taco_derived(p)) AS value
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
	macro_info.schema = "main";
	macro_info.internal = true;
	loader.RegisterFunction(macro_info);
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	db.GetFileSystem().RegisterSubSystem(make_uniq<CozipSubFileSystem>());

	loader.RegisterFunction(
	    ScalarFunction("cozip_offset_size", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipOffsetSizeFunction));
	loader.RegisterFunction(
	    ScalarFunction("cozip_profile", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipProfileFunction));
	loader.RegisterFunction(
	    ScalarFunction("cozip_vsi_base", {LogicalType::VARCHAR}, LogicalType::VARCHAR, CozipVsiBaseFunction));
	ScalarFunction taco_collection("taco_collection", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
	                               TacoCollectionFunction);
	taco_collection.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(taco_collection);
	ScalarFunction taco_structure("taco_structure", {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR),
	                              TacoStructureFunction);
	taco_structure.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(taco_structure);
	ScalarFunction taco_levels("taco_levels", {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR),
	                           TacoLevelsFunction);
	taco_levels.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(taco_levels);
	ScalarFunction taco_derived("taco_derived", {LogicalType::VARCHAR}, LogicalType::LIST(LogicalType::VARCHAR),
	                            TacoDerivedFunction);
	taco_derived.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(taco_derived);

	ScalarFunction taco_sql("taco_sql",
	                        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                         LogicalType::LIST(LogicalType::VARCHAR), LogicalType::BOOLEAN},
	                        LogicalType::VARCHAR, TacoSqlFunction);
	taco_sql.null_handling = FunctionNullHandling::SPECIAL_HANDLING;
	taco_sql.stability = FunctionStability::CONSISTENT_WITHIN_QUERY;
	loader.RegisterFunction(taco_sql);

	RegisterTableMacro(loader, "read_flat(p, gdal_vsi := true)", FLAT_MACRO_BODY);
	RegisterTableMacro(loader, "read_cozip(p, gdal_vsi := true)", FLAT_MACRO_BODY);
	// TACO spec 8.2 calls this parameter "pivot"; DuckDB reserves that word
	// for the PIVOT statement, so the reader spells it "pivoted".
	RegisterTableMacro(loader,
	                   "read_taco(p, idx := NULL, level := NULL, pivoted := true, files := NULL, gdal_vsi := true)",
	                   TACO_MACRO_BODY);
	RegisterTableMacro(loader, "taco_contract(p)", CONTRACT_MACRO_BODY);
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
