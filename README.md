# cozip_reader

DuckDB extension for reading cozip archives.

## Install

```sql
INSTALL cozip FROM community;
LOAD cozip;
```

Linux, macOS and Windows. WebAssembly is not supported.

## Flat archives

```sql
SELECT *
FROM read_flat('dataset.zip');
```

The result contains `name`, `offset`, `size`, any user metadata, and a
`cozip:gdal_vsi` path for each file.

`read_cozip()` is a deprecated alias of `read_flat()`.

## TACO archives

```sql
SELECT *
FROM read_taco('dataset.zip');
```

By default, `read_taco()` returns one row per sample and one column per file in
the contract. File columns contain GDAL VSI paths.

It accepts the three TACO containers:

```sql
SELECT * FROM read_taco('dataset.zip');  -- ZIP
SELECT * FROM read_taco('dataset/');     -- FOLDER
SELECT * FROM read_taco('.tacocat/');    -- TACOCAT
```

Common options:

```sql
SELECT *
FROM read_taco(
  'dataset.zip',
  idx := [0, 100],
  files := ['before/B02.tif', 'after/B02.tif']
);

SELECT * FROM read_taco('dataset.zip', pivoted := false);
SELECT * FROM read_taco('dataset.zip', level := 'children/before');
SELECT * FROM read_taco('dataset.zip', gdal_vsi := false);
```

Contract helpers:

```sql
SELECT cozip_profile('dataset.zip');
SELECT taco_structure('dataset.zip');
SELECT taco_levels('dataset.zip');
SELECT taco_derived('dataset.zip');
SELECT taco_collection('dataset.zip');
SELECT * FROM taco_contract('dataset.zip');
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
