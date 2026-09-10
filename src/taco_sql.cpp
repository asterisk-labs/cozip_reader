#include "taco_sql.hpp"

#include "cozip_index.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "yyjson.hpp"

#include <algorithm>

namespace duckdb {

static const char *METADATA_PREFIX = "METADATA/";
static const char *PARQUET_SUFFIX = ".parquet";
static const char *COLLECTION_NAME = "COLLECTION.json";
static const char *DATA_DIR = "DATA";
static const char *LEVEL_SAMPLE = "sample";
static const char *LEVEL_CHILDREN = "children";
static const char *TACO_VERSION = "3.0.0";

// TACO spec 7.2. Users may not define columns with this prefix, so the
// generator can strip them from the projected metadata without collisions.
static const char *ID_CURRENT = "internal:current_id";
static const char *ID_PARENT = "internal:parent_id";
static const char *ID_PATH = "internal:relative_path";
static const char *ID_OFFSET = "internal:offset";
static const char *ID_SIZE = "internal:size";
static const char *ID_SOURCE = "internal:source_file";

static string Quote(const string &identifier) {
	return "\"" + StringUtil::Replace(identifier, "\"", "\"\"") + "\"";
}

static string Literal(const string &value) {
	return "'" + StringUtil::Replace(value, "'", "''") + "'";
}

static string RegexEscape(const string &value) {
	string out;
	for (auto c : value) {
		if (strchr(".^$|()[]{}*+?\\/", c)) {
			out += '\\';
		}
		out += c;
	}
	return out;
}

static string FileToLevel(const string &file_name) {
	auto stem = file_name.substr(0, file_name.size() - strlen(PARQUET_SUFFIX));
	return StringUtil::Replace(stem, "__", "/");
}

static idx_t LevelDepth(const string &level) {
	if (level == LEVEL_SAMPLE) {
		return 0;
	}
	return 1 + (idx_t)std::count(level.begin(), level.end(), '/');
}

static string ParentLevel(const string &level) {
	if (level == LEVEL_CHILDREN) {
		return LEVEL_SAMPLE;
	}
	auto slash = level.rfind('/');
	if (slash == string::npos) {
		return LEVEL_SAMPLE;
	}
	return level.substr(0, slash);
}

static string LastSegment(const string &level) {
	auto slash = level.rfind('/');
	return slash == string::npos ? level : level.substr(slash + 1);
}

idx_t TacoLayout::LevelIndex(const string &name) const {
	for (idx_t i = 0; i < level_names.size(); i++) {
		if (level_names[i] == name) {
			return i;
		}
	}
	throw InvalidInputException("TACO dataset has no level '%s': %s", name, source);
}

static void SortAndValidateLevels(vector<string> &names, vector<string> &uris, const string &source) {
	if (names.empty()) {
		throw InvalidInputException("no METADATA Parquet files found: %s", source);
	}
	vector<idx_t> order(names.size());
	for (idx_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	// Parents must be emitted before their children so the join chain resolves.
	std::sort(order.begin(), order.end(), [&](idx_t a, idx_t b) {
		auto da = LevelDepth(names[a]);
		auto db = LevelDepth(names[b]);
		return da != db ? da < db : names[a] < names[b];
	});
	vector<string> sorted_names, sorted_uris;
	for (auto i : order) {
		sorted_names.push_back(names[i]);
		sorted_uris.push_back(uris[i]);
	}
	names = std::move(sorted_names);
	uris = std::move(sorted_uris);

	if (names[0] != LEVEL_SAMPLE) {
		throw InvalidInputException("TACO dataset has no sample.parquet: %s", source);
	}
	for (idx_t i = 1; i < names.size(); i++) {
		if (names[i] != LEVEL_CHILDREN && !StringUtil::StartsWith(names[i], "children/")) {
			throw InvalidInputException("METADATA level '%s' is not 'children' or below it: %s", names[i], source);
		}
		auto parent = ParentLevel(names[i]);
		if (std::find(names.begin(), names.begin() + i, parent) == names.begin() + i) {
			throw InvalidInputException("METADATA level '%s' has no parent level '%s': %s", names[i], parent, source);
		}
	}
	if (names.size() > 1 && names[1] != LEVEL_CHILDREN) {
		throw InvalidInputException("TACO dataset has child levels but no children.parquet: %s", source);
	}
}

static TacoLayout ResolveZip(ClientContext &context, const string &path) {
	TacoLayout layout;
	layout.container = TacoContainer::ZIP;
	layout.source = path;
	layout.vsi_base = BuildVsiBase(path);

	auto index = ReadCozipIndex(context, path);
	if (index.profile != COZIP_PROFILE_TACO) {
		throw InvalidInputException("read_taco needs a TACO-profile archive (profile=%d). Got profile=%s in: %s",
		                            (int)COZIP_PROFILE_TACO, ProfileName(index.profile), path);
	}
	auto collection = index.Find(COLLECTION_NAME);
	if (!collection) {
		throw InvalidInputException("TACO archive has no %s in its cozip index: %s", COLLECTION_NAME, path);
	}
	layout.collection_uri = SubFilePath(path, collection->offset, collection->size);

	vector<string> names, uris;
	for (auto &entry : index.entries) {
		if (!StringUtil::StartsWith(entry.name, METADATA_PREFIX) || !StringUtil::EndsWith(entry.name, PARQUET_SUFFIX)) {
			continue;
		}
		names.push_back(FileToLevel(entry.name.substr(strlen(METADATA_PREFIX))));
		uris.push_back(SubFilePath(path, entry.offset, entry.size));
	}
	SortAndValidateLevels(names, uris, path);
	layout.level_names = std::move(names);
	layout.level_uris = std::move(uris);
	return layout;
}

static vector<string> GlobParquet(ClientContext &context, const string &directory) {
	auto &fs = FileSystem::GetFileSystem(context);
	vector<string> found;
	fs.ListFiles(directory, [&](OpenFileInfo &info) {
		auto name = info.path;
		auto slash = name.find_last_of("/\\");
		if (slash != string::npos) {
			name = name.substr(slash + 1);
		}
		if (StringUtil::EndsWith(name, PARQUET_SUFFIX)) {
			found.push_back(name);
		}
	});
	std::sort(found.begin(), found.end());
	return found;
}

static TacoLayout ResolveDirectory(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto directory = StringUtil::EndsWith(path, "/") ? path.substr(0, path.size() - 1) : path;

	TacoLayout layout;
	layout.source = directory;
	layout.vsi_base = BuildVsiBase(directory);
	layout.collection_uri = directory + "/" + COLLECTION_NAME;
	if (!fs.FileExists(layout.collection_uri)) {
		throw InvalidInputException("directory has no %s: %s", COLLECTION_NAME, directory);
	}

	auto metadata_dir = directory + "/METADATA";
	string parquet_dir;
	if (fs.DirectoryExists(metadata_dir)) {
		layout.container = TacoContainer::FOLDER;
		parquet_dir = metadata_dir;
	} else {
		// TACO spec 7.5: a consolidated catalog sits beside the archives it
		// indexes, so internal:source_file resolves against the parent.
		layout.container = TacoContainer::TACOCAT;
		parquet_dir = directory;
		auto slash = directory.find_last_of("/\\");
		layout.vsi_base = BuildVsiBase(slash == string::npos ? string(".") : directory.substr(0, slash));
	}

	vector<string> names, uris;
	for (auto &file_name : GlobParquet(context, parquet_dir)) {
		names.push_back(FileToLevel(file_name));
		uris.push_back(parquet_dir + "/" + file_name);
	}
	SortAndValidateLevels(names, uris, directory);
	layout.level_names = std::move(names);
	layout.level_uris = std::move(uris);
	return layout;
}

TacoLayout ResolveTacoLayout(ClientContext &context, const string &path) {
	if (path.empty()) {
		throw InvalidInputException("read_taco: path must not be empty");
	}
	auto &fs = FileSystem::GetFileSystem(context);
	if (fs.DirectoryExists(path)) {
		return ResolveDirectory(context, path);
	}
	return ResolveZip(context, path);
}

// Reads the whole file. COLLECTION.json is a descriptor, not a payload.
static string ReadWholeFile(ClientContext &context, const string &uri) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(uri, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("could not open %s", uri);
	}
	auto size = (idx_t)handle->GetFileSize();
	static constexpr idx_t MAX_COLLECTION_SIZE = 64ULL * 1024 * 1024;
	if (size > MAX_COLLECTION_SIZE) {
		throw InvalidInputException("%s is larger than 64 MiB, refusing to read it: %s", COLLECTION_NAME, uri);
	}
	string content(size, '\0');
	if (size > 0) {
		handle->Read((void *)content.data(), size, 0);
	}
	return content;
}

namespace {

//! Frees a yyjson document however the enclosing scope exits.
struct JsonDocument {
	explicit JsonDocument(duckdb_yyjson::yyjson_doc *doc_p) : doc(doc_p) {
	}
	~JsonDocument() {
		if (doc) {
			duckdb_yyjson::yyjson_doc_free(doc);
		}
	}
	JsonDocument(const JsonDocument &) = delete;
	JsonDocument &operator=(const JsonDocument &) = delete;
	duckdb_yyjson::yyjson_doc *doc;
};

} // namespace

const vector<string> *TacoContract::FieldsOf(const string &level) const {
	for (auto &entry : fields) {
		if (entry.first == level) {
			return &entry.second;
		}
	}
	return nullptr;
}

TacoContract ReadTacoContract(ClientContext &context, const TacoLayout &layout) {
	using namespace duckdb_yyjson;

	auto text = ReadWholeFile(context, layout.collection_uri);
	JsonDocument document(yyjson_read(text.c_str(), text.size(), 0));
	if (!document.doc) {
		throw InvalidInputException("%s is not valid JSON: %s", COLLECTION_NAME, layout.source);
	}
	auto root = yyjson_doc_get_root(document.doc);
	if (!yyjson_is_obj(root)) {
		throw InvalidInputException("%s must be a JSON object: %s", COLLECTION_NAME, layout.source);
	}

	TacoContract contract;
	auto version = yyjson_obj_get(root, "taco:version");
	if (!version || !yyjson_is_str(version)) {
		throw InvalidInputException("%s has no valid taco:version: %s", COLLECTION_NAME, layout.source);
	}
	string version_text(yyjson_get_str(version), yyjson_get_len(version));
	if (version_text != TACO_VERSION) {
		throw InvalidInputException("unsupported TACO version '%s' in %s; expected %s", version_text, layout.source,
		                            TACO_VERSION);
	}
	auto structure = yyjson_obj_get(root, "taco:structure");
	if (!structure) {
		throw InvalidInputException("%s has no taco:structure key: %s", COLLECTION_NAME, layout.source);
	}
	// TACO spec 5.2: null means every sample is a single file.
	contract.null_structure = yyjson_is_null(structure);
	if (!contract.null_structure) {
		if (!yyjson_is_arr(structure)) {
			throw InvalidInputException("%s: taco:structure must be an array or null: %s", COLLECTION_NAME,
			                            layout.source);
		}
		size_t index, count;
		yyjson_val *item;
		yyjson_arr_foreach(structure, index, count, item) {
			if (!yyjson_is_str(item)) {
				throw InvalidInputException("%s: taco:structure must contain strings: %s", COLLECTION_NAME,
				                            layout.source);
			}
			contract.structure.push_back(string(yyjson_get_str(item), yyjson_get_len(item)));
		}
	}

	// taco:metadata names the user columns of every level. The reader needs
	// them to resolve a field declared at two levels of the same branch.
	auto metadata = yyjson_obj_get(root, "taco:metadata");
	if (!metadata || !yyjson_is_obj(metadata)) {
		throw InvalidInputException("%s has no valid taco:metadata object: %s", COLLECTION_NAME, layout.source);
	}
	size_t index, count;
	yyjson_val *key, *value;
	yyjson_obj_foreach(metadata, index, count, key, value) {
		if (!yyjson_is_obj(value)) {
			throw InvalidInputException("%s: metadata level must be an object: %s", COLLECTION_NAME, layout.source);
		}
		vector<string> names;
		size_t field_index, field_count;
		yyjson_val *field_key, *field_value;
		yyjson_obj_foreach(value, field_index, field_count, field_key, field_value) {
			names.push_back(string(yyjson_get_str(field_key), yyjson_get_len(field_key)));
		}
		contract.fields.emplace_back(string(yyjson_get_str(key), yyjson_get_len(key)), std::move(names));
	}
	for (auto &level : layout.level_names) {
		if (!contract.FieldsOf(level)) {
			throw InvalidInputException("%s has no metadata declaration for level '%s': %s", COLLECTION_NAME, level,
			                            layout.source);
		}
	}
	if (contract.fields.size() != layout.level_names.size()) {
		throw InvalidInputException("%s metadata levels do not match its Parquet files: %s", COLLECTION_NAME,
		                            layout.source);
	}
	if (contract.null_structure != layout.NullStructure()) {
		throw InvalidInputException("%s structure does not match its metadata levels: %s", COLLECTION_NAME,
		                            layout.source);
	}

	auto derived = yyjson_obj_get(root, "taco:derived");
	if (derived) {
		if (!yyjson_is_obj(derived)) {
			throw InvalidInputException("%s: taco:derived must be an object: %s", COLLECTION_NAME, layout.source);
		}
		size_t length = 0;
		auto serialized = yyjson_val_write(derived, YYJSON_WRITE_NOFLAG, &length);
		if (!serialized) {
			throw InvalidInputException("%s: could not read taco:derived: %s", COLLECTION_NAME, layout.source);
		}
		contract.derived.emplace_back(serialized, length);
		free(serialized);
	}
	return contract;
}

vector<string> ReadTacoStructure(ClientContext &context, const TacoLayout &layout) {
	return ReadTacoContract(context, layout).structure;
}

// A structure leaf is either a literal name or prefix*[min,max]suffix.
struct TacoLeaf {
	string declaration;
	bool variable = false;
	string prefix;
	string suffix;
};

static TacoLeaf ParseLeaf(const string &declaration) {
	TacoLeaf leaf;
	leaf.declaration = declaration;
	auto star = declaration.find('*');
	if (star == string::npos) {
		return leaf;
	}
	auto open = declaration.find('[', star);
	auto close = declaration.find(']', open == string::npos ? star : open);
	if (open != star + 1 || close == string::npos) {
		throw InvalidInputException("malformed variable leaf in taco:structure: %s", declaration);
	}
	leaf.variable = true;
	leaf.prefix = declaration.substr(0, star);
	leaf.suffix = declaration.substr(close + 1);
	return leaf;
}

namespace {

// Assembles the query. Every method appends to one buffer; nothing here
// touches the filesystem, so the SQL is fully determined by the layout.
struct TacoQueryBuilder {
	TacoQueryBuilder(const TacoLayout &layout_p, const TacoOptions &options_p, const TacoContract &contract_p)
	    : layout(layout_p), options(options_p), contract(contract_p),
	      tacocat(layout_p.container == TacoContainer::TACOCAT),
	      has_offsets(layout_p.container != TacoContainer::FOLDER) {
	}

	const TacoLayout &layout;
	const TacoOptions &options;
	const TacoContract &contract;
	bool tacocat;
	bool has_offsets;

	static string Alias(idx_t level) {
		return "l" + to_string(level);
	}

	//! Columns the reader owns and therefore hides from the projected metadata.
	//! `shadowed` additionally hides fields a deeper level of the same branch
	//! redeclares, so the most specific value wins instead of colliding.
	string ExcludeList(idx_t level, const vector<string> &shadowed = vector<string>()) const {
		vector<string> columns {ID_CURRENT, ID_PATH};
		if (level > 0) {
			columns.push_back(ID_PARENT);
		}
		if (has_offsets && (level > 0 || layout.NullStructure())) {
			columns.push_back(ID_OFFSET);
			columns.push_back(ID_SIZE);
		}
		if (tacocat) {
			columns.push_back(ID_SOURCE);
		}
		for (auto &name : shadowed) {
			columns.push_back(name);
		}
		string out = " EXCLUDE (";
		for (idx_t i = 0; i < columns.size(); i++) {
			out += (i ? ", " : "") + Quote(columns[i]);
		}
		return out + ")";
	}

	//! Where one data row can be read from, or NULL when the caller opted out.
	string VsiExpression(idx_t level) const {
		if (!options.location) {
			return "NULL::VARCHAR";
		}
		auto alias = Alias(level);
		if (layout.container == TacoContainer::FOLDER) {
			return Literal(layout.vsi_base + "/" + DATA_DIR + "/") + " || " + alias + "." + Quote(ID_PATH);
		}
		string archive = tacocat ? Literal(layout.vsi_base + "/") + " || " + alias + "." + Quote(ID_SOURCE)
		                         : Literal(layout.vsi_base);
		return "'/vsisubfile/' || " + alias + "." + Quote(ID_OFFSET) + " || '_' || " + alias + "." + Quote(ID_SIZE) +
		       " || ',' || " + archive;
	}

	//! Contract-relative path: internal:relative_path minus the sample index.
	static string PathExpression(idx_t level) {
		return "regexp_replace(" + Alias(level) + "." + Quote(ID_PATH) + ", '^[0-9]+/', '')";
	}

	//! Folder rows carry no bytes. A child level proves a name is a folder.
	string FileFilter(idx_t level) const {
		vector<string> folders;
		for (idx_t i = 1; i < layout.level_names.size(); i++) {
			if (ParentLevel(layout.level_names[i]) == layout.level_names[level]) {
				folders.push_back(LastSegment(layout.level_names[i]));
			}
		}
		if (folders.empty()) {
			return "";
		}
		string out = "regexp_replace(" + Alias(level) + "." + Quote(ID_PATH) + ", '^.*/', '') NOT IN (";
		for (idx_t i = 0; i < folders.size(); i++) {
			out += (i ? ", " : "") + Literal(folders[i]);
		}
		return out + ")";
	}

	static string VariablePattern(const TacoLeaf &leaf) {
		return "^" + RegexEscape(leaf.prefix) + "(0|[1-9][0-9]*)" + RegexEscape(leaf.suffix) + "$";
	}

	string SelectedFilesFilter(idx_t level, const vector<TacoLeaf> &leaves) const {
		auto path = PathExpression(level);
		string out = "(";
		for (idx_t i = 0; i < leaves.size(); i++) {
			if (i) {
				out += " OR ";
			}
			if (leaves[i].variable) {
				out += "regexp_matches(" + path + ", " + Literal(VariablePattern(leaves[i])) + ")";
			} else {
				out += path + " = " + Literal(leaves[i].declaration);
			}
		}
		return out + ")";
	}

	//! Projects the user columns of `level` and of every ancestor, deepest
	//! first. A field redeclared higher up is hidden, so the row carries the
	//! value of the level it actually belongs to.
	string AncestorProjection(idx_t level) const {
		vector<idx_t> chain;
		auto node = level;
		while (node > 0) {
			chain.push_back(node);
			node = layout.LevelIndex(ParentLevel(layout.level_names[node]));
		}
		chain.push_back(0);

		string out;
		vector<string> seen;
		for (auto index : chain) {
			vector<string> shadowed;
			auto declared = contract.FieldsOf(layout.level_names[index]);
			if (declared) {
				for (auto &name : *declared) {
					if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
						shadowed.push_back(name);
					} else {
						seen.push_back(name);
					}
				}
			}
			out += ", " + Alias(index) + ".*" + ExcludeList(index, shadowed);
		}
		return out;
	}

	//! Chain of joins from `level` up to the sample level.
	string JoinChain(idx_t level) const {
		string out;
		auto child = level;
		while (child > 0) {
			auto parent = layout.LevelIndex(ParentLevel(layout.level_names[child]));
			out += " JOIN " + Alias(parent) + " ON " + Alias(child) + "." + Quote(ID_PARENT) + " = " + Alias(parent) +
			       "." + Quote(ID_CURRENT);
			if (tacocat) {
				// Row ids restart in every partition, so identity is (source, id).
				out += " AND " + Alias(child) + "." + Quote(ID_SOURCE) + " = " + Alias(parent) + "." + Quote(ID_SOURCE);
			}
			child = parent;
		}
		return out;
	}

	string IdxFilter(const string &alias) const {
		if (options.idx.empty()) {
			return "";
		}
		auto text = options.idx;
		StringUtil::Trim(text);
		bool range = !text.empty() && text.front() == '[';
		if (range) {
			if (text.back() != ']') {
				throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s",
				                            options.idx);
			}
			text = text.substr(1, text.size() - 2);
		} else if (text.find_first_of("[],") != string::npos) {
			throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s", options.idx);
		}
		auto parts = StringUtil::Split(text, ',');
		if ((!range && parts.size() != 1) || (range && parts.size() != 2)) {
			throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s", options.idx);
		}
		vector<uint64_t> bounds;
		for (auto &part : parts) {
			auto trimmed = part;
			StringUtil::Trim(trimmed);
			if (trimmed.empty()) {
				throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s",
				                            options.idx);
			}
			for (auto c : trimmed) {
				if (c < '0' || c > '9') {
					throw InvalidInputException("read_taco: idx must contain non-negative integers, got %s",
					                            options.idx);
				}
			}
			try {
				size_t consumed = 0;
				auto value = std::stoull(trimmed, &consumed, 10);
				if (consumed != trimmed.size()) {
					throw InvalidInputException("read_taco: idx must contain non-negative integers, got %s",
					                            options.idx);
				}
				bounds.push_back(value);
			} catch (std::exception &) {
				throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s",
				                            options.idx);
			}
		}
		auto column = alias + "." + Quote(ID_CURRENT);
		if (bounds.size() == 1) {
			return column + " = " + to_string(bounds[0]);
		}
		if (bounds.size() == 2) {
			if (bounds[0] > bounds[1]) {
				throw InvalidInputException("read_taco: idx range start must not exceed its end, got %s", options.idx);
			}
			// Half-open, so idx := [0, 100] is the first hundred samples.
			return column + " >= " + to_string(bounds[0]) + " AND " + column + " < " + to_string(bounds[1]);
		}
		throw InvalidInputException("read_taco: idx must be an integer or a two-element list, got %s", options.idx);
	}

	string CommonTableExpressions() const {
		string out = "WITH ";
		for (idx_t i = 0; i < layout.level_uris.size(); i++) {
			out +=
			    (i ? ", " : "") + Alias(i) + " AS (SELECT * FROM read_parquet(" + Literal(layout.level_uris[i]) + "))";
		}
		return out;
	}

	//! One row per data file, columns aligned across levels by name.
	string FlatBranches(bool identity_only, const vector<TacoLeaf> *selected_files = nullptr) const {
		string out;
		for (idx_t level = 1; level < layout.level_names.size(); level++) {
			if (!out.empty()) {
				out += "\nUNION ALL BY NAME\n";
			}
			auto alias = Alias(level);
			out += "SELECT " + Alias(0) + "." + Quote(ID_CURRENT) + " AS sample_id";
			if (tacocat) {
				out += ", " + Alias(0) + "." + Quote(ID_SOURCE) + " AS source_file";
			}
			out += ", " + PathExpression(level) + " AS path";
			out += ", " + VsiExpression(level) + " AS " + Quote(TACO_LOCATION_COLUMN);
			if (!identity_only) {
				out += AncestorProjection(level);
			}
			out += " FROM " + alias + JoinChain(level);
			vector<string> filters;
			auto files = FileFilter(level);
			if (!files.empty()) {
				filters.push_back(files);
			}
			auto idx = IdxFilter(Alias(0));
			if (!idx.empty()) {
				filters.push_back(idx);
			}
			if (selected_files) {
				filters.push_back(SelectedFilesFilter(level, *selected_files));
			}
			for (idx_t i = 0; i < filters.size(); i++) {
				out += (i ? " AND " : " WHERE ") + filters[i];
			}
		}
		return out;
	}

	//! A dataset whose taco:structure is null: the sample is the file.
	string NullStructureQuery() const {
		string out = "SELECT " + Quote(ID_CURRENT) + " AS sample_id";
		if (tacocat) {
			out += ", " + Quote(ID_SOURCE) + " AS source_file";
		}
		out += ", " + VsiExpression(0) + " AS " + Quote(TACO_LOCATION_COLUMN);
		out += ", *" + ExcludeList(0);
		out += " FROM read_parquet(" + Literal(layout.level_uris[0]) + ") AS " + Alias(0);
		auto idx = IdxFilter(Alias(0));
		if (!idx.empty()) {
			out += " WHERE " + idx;
		}
		return out;
	}

	string FlatQuery() const {
		if (!options.has_files) {
			return CommonTableExpressions() + "\n" + FlatBranches(false);
		}
		auto leaves = SelectedLeaves();
		return CommonTableExpressions() + "\n" + FlatBranches(false, &leaves);
	}

	vector<TacoLeaf> SelectedLeaves() const {
		auto &structure = contract.structure;
		string unknown;
		for (auto &name : options.files) {
			if (std::find(structure.begin(), structure.end(), name) == structure.end()) {
				unknown += (unknown.empty() ? "" : ", ") + name;
			}
		}
		if (!unknown.empty()) {
			throw InvalidInputException("read_taco: files contains unknown structure leaf: %s", unknown);
		}

		vector<TacoLeaf> leaves;
		for (auto &declaration : structure) {
			if (options.has_files &&
			    std::find(options.files.begin(), options.files.end(), declaration) == options.files.end()) {
				continue;
			}
			leaves.push_back(ParseLeaf(declaration));
		}
		if (leaves.empty()) {
			throw InvalidInputException("read_taco: no structure leaf matches the requested files");
		}
		return leaves;
	}

	string PivotQuery() const {
		auto leaves = SelectedLeaves();
		if (!options.location) {
			string out = "SELECT " + Alias(0) + "." + Quote(ID_CURRENT) + " AS sample_id";
			if (tacocat) {
				out += ", " + Alias(0) + "." + Quote(ID_SOURCE) + " AS source_file";
			}
			out += ", " + Alias(0) + ".*" + ExcludeList(0);
			for (auto &leaf : leaves) {
				auto name = leaf.variable ? leaf.prefix : leaf.declaration;
				out += (leaf.variable ? ", NULL::VARCHAR[] AS " : ", NULL::VARCHAR AS ");
				out += Quote(name);
			}
			out += " FROM read_parquet(" + Literal(layout.level_uris[0]) + ") AS " + Alias(0);
			auto idx = IdxFilter(Alias(0));
			if (!idx.empty()) {
				out += " WHERE " + idx;
			}
			return out;
		}

		string out = CommonTableExpressions();
		out += ", flat AS (\n" + FlatBranches(true, options.has_files ? &leaves : nullptr) + "\n)";
		out += ", pivoted AS (SELECT sample_id";
		if (tacocat) {
			out += ", source_file";
		}
		for (auto &leaf : leaves) {
			if (leaf.variable) {
				// TACO spec 5.2: the cardinal index has no leading zeros, so
				// img01.tif is not an instance of img*[a,b].tif.
				auto pattern = VariablePattern(leaf);
				out += ", list(" + Quote(TACO_LOCATION_COLUMN) + " ORDER BY TRY_CAST(regexp_extract(path, " +
				       Literal(pattern) + ", 1) AS BIGINT)) FILTER (WHERE regexp_matches(path, " + Literal(pattern) +
				       ")) AS " + Quote(leaf.prefix);
			} else {
				out += ", MAX(CASE WHEN path = " + Literal(leaf.declaration) + " THEN " + Quote(TACO_LOCATION_COLUMN) +
				       " END) AS " + Quote(leaf.declaration);
			}
		}
		out += " FROM flat GROUP BY ALL)";

		out += "\nSELECT " + Alias(0) + "." + Quote(ID_CURRENT) + " AS sample_id";
		if (tacocat) {
			out += ", " + Alias(0) + "." + Quote(ID_SOURCE) + " AS source_file";
		}
		out += ", " + Alias(0) + ".*" + ExcludeList(0);
		for (auto &leaf : leaves) {
			auto name = leaf.variable ? leaf.prefix : leaf.declaration;
			if (leaf.variable) {
				out += ", COALESCE(p." + Quote(name) + ", []::VARCHAR[]) AS " + Quote(name);
			} else {
				out += ", p." + Quote(name);
			}
		}
		out += " FROM " + Alias(0) + " LEFT JOIN pivoted p ON p.sample_id = " + Alias(0) + "." + Quote(ID_CURRENT);
		if (tacocat) {
			out += " AND p.source_file = " + Alias(0) + "." + Quote(ID_SOURCE);
		}
		auto idx = IdxFilter(Alias(0));
		if (!idx.empty()) {
			out += " WHERE " + idx;
		}
		return out;
	}

	string LevelQuery() const {
		auto level = layout.LevelIndex(options.level);
		return "SELECT * FROM read_parquet(" + Literal(layout.level_uris[level]) + ")";
	}
};

} // namespace

string BuildTacoSQL(ClientContext &context, const string &path, const TacoOptions &options) {
	auto layout = ResolveTacoLayout(context, path);
	auto contract = ReadTacoContract(context, layout);
	if (!options.level.empty()) {
		if (options.has_files) {
			throw InvalidInputException("read_taco: files does not apply when level is set");
		}
		return TacoQueryBuilder(layout, options, contract).LevelQuery();
	}
	if (contract.null_structure) {
		if (options.has_files) {
			throw InvalidInputException("read_taco: files requires taco:structure");
		}
		return TacoQueryBuilder(layout, options, contract).NullStructureQuery();
	}

	TacoQueryBuilder builder(layout, options, contract);
	if (!options.pivot) {
		return builder.FlatQuery();
	}
	if (contract.structure.empty()) {
		throw InvalidInputException("taco:structure is null but the dataset has %llu metadata levels: %s",
		                            (uint64_t)layout.level_names.size(), layout.source);
	}
	return builder.PivotQuery();
}

} // namespace duckdb
