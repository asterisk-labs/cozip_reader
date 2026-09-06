<div align="center">
  <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/banner.svg" alt="cozip — DuckDB extension" width="700"/>

  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-EAB308?style=flat-square" alt="License MIT"/></a>
    <a href="https://duckdb.org/community_extensions/extensions/cozip.html"><img src="https://img.shields.io/badge/duckdb-community--extension-FFF000?logo=duckdb&logoColor=black&style=flat-square" alt="DuckDB community extension"/></a>
    <a href="https://github.com/asterisk-labs/cozip/blob/main/SPEC.md"><img src="https://img.shields.io/badge/cozip-format--%26--spec-A8B9CC?style=flat-square" alt="cozip format"/></a>
    <a href="https://asterisk.coop/taco/spec"><img src="https://img.shields.io/badge/taco-spec-F97316?style=flat-square" alt="TACO spec"/></a>
  </p>
</div>

---

Query a [cozip](https://github.com/asterisk-labs/cozip) archive as a SQL table — locally, over HTTPS, S3, GCS, Azure, or HuggingFace. The index at byte 0 locates the metadata without scanning the ZIP Central Directory or downloading the archive.

The archive is still a valid ZIP. `unzip`, `zipfile.ZipFile`, your OS file preview — all unchanged.

## Install

```sql
INSTALL cozip FROM community;
LOAD cozip;
```

Linux, macOS and Windows. For the browser, use the pure-JavaScript reader in the [cozip](https://github.com/asterisk-labs/cozip) repository instead.

## One reader per profile

A cozip declares a profile in its byte-0 index. The file extension is always `.zip` and carries no meaning, so the profile is what tells you which reader to use.

```sql
SELECT cozip_profile('dataset.zip');   -- 'flat', 'taco', 'none'
```

| Profile | Reader | Shape |
| --- | --- | --- |
| Flat | `read_flat(path)` | one row per file in the archive |
| TACO | `read_taco(path)` | one row per sample, one column per file |

### read_flat

```sql
SELECT *
FROM read_flat('https://huggingface.co/datasets/Major-TOM/Core-VIIRS-Nighttime-Light/resolve/main/2024/MAJORTOM-VIIRS-NTL_2024_median_000.zip')
LIMIT 10;
```

One row per entry: `name`, `offset`, `size`, plus whatever columns the writer put in `__metadata__`. Filter, join, sample, then fetch only the payloads you actually need.

```sql
-- Sample 32 training tiles from a remote archive without downloading it.
SELECT name, "cozip:gdal_vsi"
FROM read_flat('s3://my-bucket/dataset.zip')
WHERE split = 'train'
USING SAMPLE 32 ROWS;
```

### read_taco

[TACO](https://asterisk.coop/taco/spec) datasets declare which files each sample contains and which metadata belongs to each level. `read_taco` returns one row per sample with one column per file.

```sql
SELECT * FROM read_taco('cloudsen12.zip');
-- sample_id │ ml:split │ quality:cloud_cover │ s2_l1c.tif      │ target.tif
-- 0         │ train    │ 23.5                │ /vsisubfile/... │ /vsisubfile/...
```

That shape feeds a dataloader directly. Every file column holds a GDAL path that opens the file in place.

The pivoted shape includes sample metadata. Use `pivoted := false` to include
asset and folder metadata too, with one row per file. Collection metadata stays
in `COLLECTION.json` and is available through `taco_collection()`.

```python
import duckdb, rasterio

con = duckdb.connect()
con.execute("INSTALL cozip FROM community; LOAD cozip")
rows = con.execute("""
    SELECT "s2_l1c.tif", "target.tif"
    FROM read_taco('hf://datasets/tacofoundation/cloudsen12/cloudsen12.zip')
    WHERE "ml:split" = 'train' USING SAMPLE 64 ROWS
""").fetchall()

with rasterio.open(rows[0][0]) as src:
    image = src.read()
```

#### Parameters

| Parameter | Default | Meaning |
| --- | --- | --- |
| `idx` | all samples | `5` for one sample, `[0, 100]` for a half-open range |
| `level` | joined view | one contract level read raw; the other options do not apply |
| `pivoted` | `true` | `false` gives one row per file instead of one per sample |
| `files` | every leaf | restricts which files become columns |
| `gdal_vsi` | `true` | `false` leaves the path columns NULL |

```sql
-- One row per file, with the metadata of every level it belongs to.
SELECT sample_id, path, "raster:resolution", "cozip:gdal_vsi"
FROM read_taco('change_detection.zip', pivoted := false);
-- 0 │ after/B02.tif  │ 20 │ /vsisubfile/...
-- 0 │ before/B02.tif │ 10 │ /vsisubfile/...
-- 0 │ change_map.tif │ NULL │ /vsisubfile/...

-- The first hundred samples, two bands only.
SELECT * FROM read_taco('ds.zip', idx := [0, 100], files := ['before/B02.tif', 'after/B02.tif']);

-- One level raw, for inspection.
SELECT * FROM read_taco('ds.zip', level := 'children/before');
```

Rows have no implicit order. Add `ORDER BY sample_id` when order matters.

A contract may declare a variable leaf, written `img*[4,16].tif`, for samples that hold a varying number of files. Those collapse into one `LIST(VARCHAR)` column named after the prefix, ordered by the cardinal index rather than lexicographically.

```sql
SELECT sample_id, len("img") AS n, "img"[1] AS first
FROM read_taco('multitemporal.zip');
```

#### Three containers, one reader

`read_taco` takes a path to any of them and works out which it is:

```sql
SELECT * FROM read_taco('dataset.zip');       -- ZIP, byte ranges into the archive
SELECT * FROM read_taco('dataset/');          -- FOLDER, plain files under DATA/
SELECT * FROM read_taco('.tacocat/');         -- TACOCAT directory
```

A TACOCAT directory contains the consolidated metadata for several ZIP partitions. Its result carries an extra `source_file` column because sample ids restart in every ZIP.

#### Contract inspection

```sql
SELECT * FROM taco_contract('ds.zip');   -- structure, level and derived rows
SELECT taco_structure('ds.zip');         -- LIST of the contract's leaves
SELECT taco_levels('ds.zip');            -- LIST of the metadata levels
SELECT taco_derived('ds.zip');           -- LIST containing taco:derived as JSON
SELECT taco_collection('ds.zip');        -- the raw COLLECTION.json
```

Field types and descriptions live in `COLLECTION.json`. Load the `json` extension to query them.

## How it works

The extension registers a virtual filesystem, `cozip-subfile://<offset>_<size>!<path>`, that exposes any byte range of any file as if it were a standalone file. Everything else composes on top of it in SQL: `read_parquet` reads the metadata Parquets straight out of the archive, and the range requests flow lazily through DuckDB's own HTTP, S3 and Azure layers.

C++ only ever reads bytes at fixed offsets: the byte-0 index, and `COLLECTION.json`. No Parquet is parsed in the extension. `read_taco` is a macro over a scalar that resolves the container, discovers the metadata levels and returns the query to run.

```sql
-- The generated query, for debugging.
SELECT taco_sql('ds.zip', NULL, NULL, true, NULL, true);
```

## Function reference

| Function | Returns |
| --- | --- |
| `read_flat(path, gdal_vsi := true)` | table, Flat manifest |
| `read_taco(path, idx, level, pivoted, files, gdal_vsi)` | table, TACO dataset |
| `taco_contract(path)` | table of `kind`, `value` |
| `taco_structure(path)` | `LIST(VARCHAR)` |
| `taco_levels(path)` | `LIST(VARCHAR)` |
| `taco_derived(path)` | `LIST(VARCHAR)`, serialized `taco:derived` object |
| `taco_collection(path)` | `VARCHAR`, raw JSON |
| `taco_sql(path, idx, level, pivoted, files, gdal_vsi)` | `VARCHAR`, generated SQL |
| `cozip_profile(path)` | `VARCHAR` |
| `cozip_offset_size(path)` | `VARCHAR`, `"<offset>_<size>"` of `__metadata__` |
| `cozip_vsi_base(path)` | `VARCHAR`, GDAL prefix for a URL |
| `read_cozip(path, gdal_vsi := true)` | deprecated alias of `read_flat` |

## Building

```bash
git clone --recurse-submodules https://github.com/asterisk-labs/cozip_reader
cd cozip_reader
make            # builds duckdb and the extension into build/release
make test       # runs test/sql/*.test
```

Regenerate the fixtures after a format change:

```bash
python test/generate_fixtures.py        # Flat archives, needs the cozip writer
python test/generate_taco_fixtures.py   # TACO datasets, needs the taco writer
```

## License

MIT. See [LICENSE](LICENSE).
