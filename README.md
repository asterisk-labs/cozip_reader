# cozip DuckDB Extension

`cozip` is a small DuckDB extension for inspecting ZIP-compatible `cozip`
archives. A `cozip` file is still a valid ZIP file, but its first entry is a
special `__cozip__` member containing a compact binary index with priority file
names, absolute payload offsets, and payload sizes.

This extension currently focuses on local-file inspection. It validates the
core cozip 1.0 layout and exposes the priority index through DuckDB SQL. It
does not create archives or decompress payloads.

## SQL API

```sql
LOAD cozip;

SELECT cozip_priority_count('test/data/flat_store_6files.cozip');
SELECT cozip_profile('test/data/flat_store_6files.cozip');
SELECT cozip_integrity_hash_ok('test/data/flat_store_6files.cozip');
SELECT cozip_metadata_offset('test/data/flat_store_6files.cozip');
SELECT cozip_index_json('test/data/flat_store_6files.cozip');

SELECT *
FROM cozip_index('test/data/flat_store_6files.cozip')
ORDER BY ordinal;
```

`cozip_index(path)` returns:

| column | type | meaning |
| --- | --- | --- |
| `ordinal` | `UBIGINT` | priority-entry order in the `__cozip__` index |
| `name` | `VARCHAR` | priority-entry name |
| `offset_bytes` | `UBIGINT` | absolute payload offset from byte 0 |
| `size_bytes` | `UBIGINT` | payload size in bytes |

Scalar functions:

| function | returns |
| --- | --- |
| `cozip_priority_count(path)` | number of entries listed in the `__cozip__` priority index |
| `cozip_profile(path)` | profile byte from the cozip index header |
| `cozip_metadata_offset(path)` | absolute payload offset for `__metadata__` |
| `cozip_integrity_hash_ok(path)` | whether the stored FNV-1a integrity hash matches |
| `cozip_tail_hash_ok(path)` | compatibility alias for `cozip_integrity_hash_ok` |
| `cozip_index_json(path)` | compact JSON representation of the parsed index |

## Format Notes

The reader expects the current cozip core format:

1. The first ZIP Local File Header starts at byte `0`.
2. The first entry name is exactly `__cozip__`.
3. The `__cozip__` Local File Header has a single 12-byte extra field with
   header id `0xCA0C`.
4. The cozip index payload starts at byte `51`.
5. The index payload begins with `CZIP`, followed by `version`, `profile`, and
   `n_entries`.
6. The integrity hash is FNV-1a 64 over the index payload and the final
   `32768` bytes of the archive, counting overlapping bytes once.
7. The stored hash lives in archive bytes `43..50`.

## Build On Windows

Configure with CMake and the DuckDB submodule:

```powershell
cmake -S .\duckdb -B .\build\release -G "Visual Studio 18 2026" -A x64 -T v143 `
  -DDUCKDB_EXTENSION_CONFIGS="${PWD}\extension_config.cmake" `
  -DEXTENSION_STATIC_BUILD=1 `
  -DUNITTEST_ROOT_DIRECTORY="${PWD}" `
  -DBENCHMARK_ROOT_DIRECTORY="${PWD}" `
  -DENABLE_UNITTEST_CPP_TESTS=FALSE `
  -DENABLE_EXTENSION_AUTOLOADING=0 `
  -DENABLE_EXTENSION_AUTOINSTALL=0
```

If `cmake` is not on your `PATH`, use the Visual Studio bundled executable,
for example:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --version
```

Build the extension and test runner:

```powershell
cmake --build .\build\release --config Release --target cozip_loadable_extension
cmake --build .\build\release --config Release --target unittest
```

Run the extension tests:

```powershell
.\build\release\test\Release\unittest.exe --test-dir . "test/sql/cozip.test"
```

If Windows refuses to overwrite `cozip.duckdb_extension`, close any Python, R,
or DuckDB process that previously loaded it.

## Local Manual Test

```powershell
.\build\release\Release\duckdb.exe -unsigned -c "LOAD './build/release/extension/cozip/cozip.duckdb_extension'; SELECT * FROM cozip_index('test/data/flat_store_6files.cozip');"
```

## GitHub Actions

`.github/workflows/MainDistributionPipeline.yml` calls DuckDB's official
`extension-ci-tools` reusable pipeline. That pipeline builds and tests the
extension on Linux, Windows, and macOS using the same SQLLogicTest file under
`test/sql/` that we run locally.

## Notebooks

- `notebooks/cozip_architecture.ipynb` explains the binary layout, formulas,
  integrity hash, and generation steps.
- `notebooks/cozip_python_testing.ipynb` loads the compiled extension from
  Python and cross-checks the fixture with the pure-Python parser in
  `notebooks/cozip.py`.

## Generating A `.cozip`

A `.cozip` is not produced by renaming an arbitrary `.zip`. It is generated as a
ZIP file with extra rules:

1. Write `__cozip__` as the first local file entry.
2. Use ZIP method STORE for `__cozip__`.
3. Put a 12-byte extra field in the `__cozip__` local header.
4. Store a `CZIP` payload listing priority names, offsets, and sizes.
5. Write regular ZIP entries, Central Directory, and EOCD.
6. Compute FNV-1a over the index payload and the final 32768 bytes.
7. Store that hash in the `__cozip__` extra field.
