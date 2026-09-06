#pragma once

// Generates the SQL that reads a TACO dataset.
//
// Nothing here opens a Parquet file. The generator resolves the container,
// discovers the metadata levels and emits one self-contained SELECT that
// read_parquet executes. That keeps every columnar code path inside DuckDB.

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context.hpp"

#include <utility>

namespace duckdb {

enum class TacoContainer : uint8_t { ZIP, FOLDER, TACOCAT };

struct TacoOptions {
	//! "" for every sample, "5" for one, "[0, 100]" for a half-open range.
	string idx;
	//! "" for the joined view, otherwise one contract level read raw.
	string level;
	//! One row per sample with a column per structure leaf.
	bool pivot = true;
	//! Restricts which structure leaves become pivot columns.
	vector<string> files;
	bool has_files = false;
	//! Emit GDAL VSI paths. When false the column is present but NULL.
	bool gdal_vsi = true;
};

//! Container, level layout and structure of one TACO dataset.
struct TacoLayout {
	TacoContainer container;
	string source;
	//! GDAL prefix for the archive (ZIP) or the directory (FOLDER, TACOCAT).
	string vsi_base;
	//! Where read_parquet finds COLLECTION.json.
	string collection_uri;
	vector<string> level_names;
	vector<string> level_uris;

	bool NullStructure() const {
		return level_names.size() == 1;
	}
	idx_t LevelIndex(const string &name) const;
};

//! Resolve the container and enumerate its metadata levels.
TacoLayout ResolveTacoLayout(ClientContext &context, const string &path);

//! The full query for read_taco().
string BuildTacoSQL(ClientContext &context, const string &path, const TacoOptions &options);

//! taco:structure plus the user field names of each level, from COLLECTION.json.
struct TacoContract {
	//! Empty when taco:structure is null.
	vector<string> structure;
	bool null_structure = false;
	//! Level name to the user fields it declares.
	vector<std::pair<string, vector<string>>> fields;
	//! Serialized taco:derived object, empty when no derived groups are declared.
	vector<string> derived;

	const vector<string> *FieldsOf(const string &level) const;
};

TacoContract ReadTacoContract(ClientContext &context, const TacoLayout &layout);

//! The contract structure alone. Empty for a null structure.
vector<string> ReadTacoStructure(ClientContext &context, const TacoLayout &layout);

} // namespace duckdb
