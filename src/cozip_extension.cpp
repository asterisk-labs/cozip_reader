#define DUCKDB_EXTENSION_MAIN

#include "cozip_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension_helper.hpp"

// From the parquet extension (statically linked via CMakeLists.txt).
#include "parquet_reader.hpp"

#include <cstring>
#include <utility>
#include <vector>

namespace duckdb {

// -----------------------------------------------------------------------------
// cozip 1.0 on-disk constants
// -----------------------------------------------------------------------------
static constexpr uint32_t ZIP_LFH_SIGNATURE       = 0x04034B50U;
static constexpr idx_t    COZIP_LFH_SIZE          = 51;
static constexpr idx_t    COZIP_INDEX_HEADER_SIZE = 11;
static constexpr idx_t    COZIP_HASH_WINDOW_SIZE  = 32768;
static constexpr idx_t    COZIP_MIN_SIZE          = COZIP_LFH_SIZE + COZIP_HASH_WINDOW_SIZE;
static constexpr idx_t    COZIP_BOOTSTRAP_SIZE    = 65536;
static constexpr uint16_t COZIP_EXTRA_HEADER_ID   = 0xCA0C;
static constexpr uint8_t  COZIP_PROFILE_FLAT      = 1;
static constexpr uint16_t COZIP_FORMAT_VERSION    = 1;

static const std::string COZIP_INDEX_NAME    = "__cozip__";
static const std::string COZIP_METADATA_NAME = "__metadata__";
static const std::string COZIP_VSI_COLUMN    = "cozip:gdal_vsi";

// -----------------------------------------------------------------------------
// Little-endian readers
// -----------------------------------------------------------------------------
static inline uint16_t ReadU16LE(const uint8_t *p) {
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t ReadU32LE(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ReadU64LE(const uint8_t *p) {
	return (uint64_t)ReadU32LE(p) | ((uint64_t)ReadU32LE(p + 4) << 32);
}

// -----------------------------------------------------------------------------
// Cozip index parsing (read minimal ranges only)
// -----------------------------------------------------------------------------
struct CozipMetadataEntry {
	uint64_t offset;
	uint64_t size;
};

static CozipMetadataEntry ParseCozipMetadataLocation(FileHandle &handle, const std::string &source) {
	auto file_size = (idx_t)handle.GetFileSize();
	if (file_size < COZIP_MIN_SIZE) {
		throw InvalidInputException("cozip archive too small (minimum is %llu bytes): %s",
		                            (unsigned long long)COZIP_MIN_SIZE, source);
	}

	auto bootstrap = std::min<idx_t>(COZIP_BOOTSTRAP_SIZE, file_size);
	std::vector<uint8_t> head(bootstrap);
	handle.Read(head.data(), bootstrap, 0);

	if (ReadU32LE(head.data()) != ZIP_LFH_SIGNATURE) {
		throw InvalidInputException("byte 0 is not a ZIP Local File Header: %s", source);
	}
	auto name_len  = ReadU16LE(head.data() + 26);
	auto extra_len = ReadU16LE(head.data() + 28);
	if (name_len != 9 || extra_len != 12) {
		throw InvalidInputException("LFH does not match cozip layout: %s", source);
	}
	if (memcmp(head.data() + 30, COZIP_INDEX_NAME.data(), 9) != 0) {
		throw InvalidInputException("first ZIP entry is not __cozip__: %s", source);
	}
	if (ReadU16LE(head.data() + 39) != COZIP_EXTRA_HEADER_ID) {
		throw InvalidInputException("cozip integrity extra field (0xCA0C) missing: %s", source);
	}

	auto index_payload_size = (idx_t)ReadU32LE(head.data() + 18);
	if (index_payload_size == 0) {
		throw InvalidInputException("cozip index payload size is zero: %s", source);
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
		throw InvalidInputException("index payload magic is not 'CZIP': %s", source);
	}
	auto version = ReadU16LE(pp + 4);
	if (version > COZIP_FORMAT_VERSION) {
		throw InvalidInputException("unsupported cozip format version %d: %s", (int)version, source);
	}
	auto profile = pp[6];
	if (profile != COZIP_PROFILE_FLAT) {
		throw InvalidInputException(
		    "read_cozip only supports the Flat profile (profile=1). Got profile=%d in: %s", (int)profile, source);
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
			return CozipMetadataEntry{offsets[i], sizes[i]};
		}
	}
	throw InvalidInputException("Flat-profile cozip is missing __metadata__ entry: %s", source);
}

// -----------------------------------------------------------------------------
// VSI base mapping: prefix → /vsi*/ handler
// -----------------------------------------------------------------------------
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
			rest      = rest.substr(9);
		} else if (StringUtil::StartsWith(rest, "spaces/")) {
			ns_prefix = "spaces/";
			rest      = rest.substr(7);
		}
		auto sl1 = rest.find('/');
		auto sl2 = (sl1 == std::string::npos) ? std::string::npos : rest.find('/', sl1 + 1);
		if (sl1 == std::string::npos || sl2 == std::string::npos) {
			throw InvalidInputException("cannot parse hf:// URL for VSI mapping: %s", path);
		}
		auto owner_repo = rest.substr(0, sl2);
		auto inner      = rest.substr(sl2 + 1);
		return "/vsicurl/https://huggingface.co/" + ns_prefix + owner_repo + "/resolve/main/" + inner;
	}
	return path;
}

// -----------------------------------------------------------------------------
// read_cozip table function: bind, init, scan
// -----------------------------------------------------------------------------
struct ReadCozipBindData : public TableFunctionData {
	std::string path;
	std::string vsi_base;
	std::string temp_parquet_path;
	bool with_gdal_vsi = true;
	bool validate      = false;

	// Parquet schema mirrored out of bind_data->reader->columns.
	vector<LogicalType> parquet_types;
	vector<string>      parquet_names;
	idx_t offset_col_idx = DConstants::INVALID_INDEX;
	idx_t size_col_idx   = DConstants::INVALID_INDEX;

	// The reader lives in bind because it owns the file handle to the
	// temp parquet and the parsed Parquet metadata. The scan state is
	// per-init and lives in the global state.
	unique_ptr<ParquetReader> reader;
};

struct ReadCozipGlobalState : public GlobalTableFunctionState {
	ParquetReaderScanState scan_state;
	idx_t parquet_column_count = 0;
};

static unique_ptr<FunctionData> ReadCozipBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("read_cozip path cannot be NULL");
	}
	auto path = StringValue::Get(input.inputs[0]);

	auto bind_data  = make_uniq<ReadCozipBindData>();
	bind_data->path = path;

	for (auto &kv : input.named_parameters) {
		auto key = StringUtil::Lower(kv.first);
		if (key == "gdal_vsi") {
			bind_data->with_gdal_vsi = BooleanValue::Get(kv.second);
		} else if (key == "validate") {
			bind_data->validate = BooleanValue::Get(kv.second);
		}
	}

	// Open the cozip via DuckDB's FileSystem. Transparent dispatch to
	// LocalFileSystem, HttpFs (autoloaded for http/https/s3/gs/r2/hf),
	// or AzureFs (autoloaded if installed).
	auto &fs    = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	if (!handle) {
		throw IOException("could not open cozip archive: %s", path);
	}

	auto md = ParseCozipMetadataLocation(*handle, path);

	// TODO(v1.1): if validate is true, read the final 32 KiB suffix region
	// and recompute the FNV-1a 64 hash, then compare with the stored hash
	// at LFH bytes 43..50.

	// Pull the __metadata__ Parquet payload into RAM and persist it as a
	// temp file so ParquetReader can open it by path. v1.1 will switch to
	// a registered in-memory FileSystem to avoid the temp file.
	std::vector<uint8_t> parquet_buf(md.size);
	handle->Read(parquet_buf.data(), md.size, md.offset);

	auto tmp_dir = fs.GetHomeDirectory();
	if (tmp_dir.empty()) {
		tmp_dir = ".";
	}
	auto tmp_name = "cozip_md_" + std::to_string((uint64_t)reinterpret_cast<uintptr_t>(bind_data.get())) + ".parquet";
	bind_data->temp_parquet_path = fs.JoinPath(tmp_dir, tmp_name);

	{
		auto tmp_handle =
		    fs.OpenFile(bind_data->temp_parquet_path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE);
		tmp_handle->Write(parquet_buf.data(), parquet_buf.size());
	}

	ParquetOptions parquet_options(context);
	bind_data->reader = make_uniq<ParquetReader>(context, OpenFileInfo(bind_data->temp_parquet_path), parquet_options);

	// Mirror the parquet schema out of BaseFileReader::columns.
	for (auto &col : bind_data->reader->columns) {
		bind_data->parquet_names.push_back(col.name);
		bind_data->parquet_types.push_back(col.type);
	}

	return_types = bind_data->parquet_types;
	names        = bind_data->parquet_names;

	for (idx_t i = 0; i < bind_data->parquet_names.size(); i++) {
		if (bind_data->parquet_names[i] == "offset") {
			bind_data->offset_col_idx = i;
		} else if (bind_data->parquet_names[i] == "size") {
			bind_data->size_col_idx = i;
		}
	}

	if (bind_data->with_gdal_vsi) {
		if (bind_data->offset_col_idx == DConstants::INVALID_INDEX ||
		    bind_data->size_col_idx == DConstants::INVALID_INDEX) {
			throw InvalidInputException(
			    "__metadata__ Parquet missing required 'offset' and/or 'size' columns "
			    "(needed to build cozip:gdal_vsi). Set gdal_vsi := false to skip.");
		}
		bind_data->vsi_base = BuildVsiBase(path);
		return_types.push_back(LogicalType::VARCHAR);
		names.push_back(COZIP_VSI_COLUMN);
	}

	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> ReadCozipInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ReadCozipBindData>();
	auto state      = make_uniq<ReadCozipGlobalState>();
	state->parquet_column_count = bind_data.parquet_types.size();

	// Build the row-group list (read all groups) and initialize the scan.
	vector<idx_t> groups_to_read;
	auto n_groups = bind_data.reader->NumRowGroups();
	groups_to_read.reserve(n_groups);
	for (idx_t i = 0; i < n_groups; i++) {
		groups_to_read.push_back(i);
	}
	bind_data.reader->InitializeScan(context, state->scan_state, std::move(groups_to_read));

	return std::move(state);
}

static void ReadCozipFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<ReadCozipBindData>();
	auto &gstate    = data_p.global_state->Cast<ReadCozipGlobalState>();

	// Read into a staging chunk shaped like the parquet schema.
	DataChunk staging;
	staging.Initialize(context, bind_data.parquet_types);

	(void)bind_data.reader->Scan(context, gstate.scan_state, staging);

	auto row_count = staging.size();
	output.SetCardinality(row_count);
	if (row_count == 0) {
		return;
	}

	// Pass parquet columns through verbatim.
	for (idx_t c = 0; c < gstate.parquet_column_count; c++) {
		output.data[c].Reference(staging.data[c]);
	}

	// Inject cozip:gdal_vsi as the last column when requested.
	if (bind_data.with_gdal_vsi) {
		auto &offset_vec = staging.data[bind_data.offset_col_idx];
		auto &size_vec   = staging.data[bind_data.size_col_idx];
		offset_vec.Flatten(row_count);
		size_vec.Flatten(row_count);
		auto offsets = FlatVector::GetData<uint64_t>(offset_vec);
		auto sizes   = FlatVector::GetData<uint64_t>(size_vec);

		auto &out_vec = output.data[gstate.parquet_column_count];
		auto out_data = FlatVector::GetData<string_t>(out_vec);
		for (idx_t r = 0; r < row_count; r++) {
			auto s = "/vsisubfile/" + std::to_string(offsets[r]) + "_" + std::to_string(sizes[r]) + "," +
			         bind_data.vsi_base;
			out_data[r] = StringVector::AddString(out_vec, s);
		}
	}
}

// -----------------------------------------------------------------------------
// Extension entry points
// -----------------------------------------------------------------------------
static void LoadInternal(ExtensionLoader &loader) {
	TableFunction read_cozip("read_cozip", {LogicalType::VARCHAR}, ReadCozipFunction, ReadCozipBind, ReadCozipInit);
	read_cozip.named_parameters["gdal_vsi"] = LogicalType::BOOLEAN;
	read_cozip.named_parameters["validate"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(read_cozip);
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