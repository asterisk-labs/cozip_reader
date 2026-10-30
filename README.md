# cozip_reader

DuckDB extension for reading Flat-profile cozip archives.

## Install

```sql
INSTALL cozip FROM community;
LOAD cozip;
```

Linux, macOS and Windows. WebAssembly is not supported.

## Read an archive

```sql
SELECT *
FROM read_flat('dataset.zip');
```

The result contains `name`, `offset`, `size`, any user metadata, and a
`cozip:location` path for each file.

Remote archives work through DuckDB's `httpfs` extension. These public
examples contain the same Flat archive:

```sql
-- Source Cooperative
SELECT * FROM read_flat(
  'https://data.source.coop/asterisk-labs/cozip-api-fixtures/data/cities.zip'
);

-- Hugging Face, pinned to a revision
SELECT * FROM read_flat(
  'hf://datasets/asterisk-labs/cozip-api-fixtures@v0.1.0/data/cities.zip'
);
```

Pass `location := false` when only the manifest is needed:

```sql
SELECT * FROM read_flat('dataset.zip', location := false);
```

`cozip_profile()` reads only the archive bootstrap and returns the raw profile
byte. `read_flat()` accepts profile `1` and rejects every other profile.

```sql
SELECT cozip_profile('dataset.zip'); -- 1 for Flat
```

## Read a file inside an archive

Pass the `cozip:location` returned by `read_flat()` to `read_parquet`,
`read_text`, or another DuckDB reader. TACO ZIP datasets use the same path
format for their `::location` values. Readers can fetch the needed byte ranges
without unpacking the archive.

```sql
SELECT count(*) FROM read_parquet('/vsisubfile/1032_52411,/vsicurl/https://example.org/dataset.zip');
```

The syntax is `/vsisubfile/<offset>[_<size>],<path>`. An omitted or zero size
reads through EOF. `<path>` can be local, another subfile, or a GDAL storage
path: `/vsicurl/`, `/vsis3/`, `/vsigs/`, `/vsiaz/`, `/vsiadls/`, `/vsihf/`, or
`/vsisource/`. These map to DuckDB's HTTP, S3, GCS, Azure, and Hugging Face
paths. `/vsisource/` is limited to public files at `https://data.source.coop/`;
it does not use `SOURCE_ENDPOINT` or signed Source access.

For a DuckDB path, use `cozip-subfile://<offset>_<size>!<path>`. Unlike the
GDAL form, size 0 means an empty file.

```sql
SELECT 'cozip-subfile://' || "offset" || '_' || "size" || '!s3://bucket/dataset.zip' AS path
FROM read_flat('s3://bucket/dataset.zip', location := false);
```

Both forms are read-only and reject ranges outside the source file.

## Compatibility

The original API remains available. `read_cozip()` emits the legacy
`cozip:gdal_vsi` column, and `gdal_vsi` remains accepted by `read_flat()` as an
alias for `location`.

```sql
SELECT * FROM read_cozip('dataset.zip', gdal_vsi := true);
SELECT * FROM read_flat('dataset.zip', gdal_vsi := false);
```

## Build

```bash
git clone --recurse-submodules https://github.com/asterisk-labs/cozip_reader
cd cozip_reader
make
make test
```

## License

MIT

<div align="center">
  <br>
  Made with ♥ by
  <br><br>
  <a href="https://asterisk.coop">
    <img src="https://raw.githubusercontent.com/asterisk-labs/cozip/refs/heads/main/images/asterisk_logo.svg" alt="Asterisk Labs" width="320"/>
  </a>
</div>
