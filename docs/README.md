# cozip DuckDB Extension

This repository contains a DuckDB extension that reads the cozip core index
from ZIP-compatible archives produced by the TACO/cozip writer.

The extension currently exposes inspection functions rather than a full TACO
table reader. It validates the byte-zero `__cozip__` entry, parses the `CZIP`
priority index, and checks the FNV-1a 64 integrity hash defined by the current
cozip 1.0 draft.

## Functions

```sql
LOAD cozip;

SELECT cozip_priority_count('dataset.zip');
SELECT cozip_profile('dataset.zip');
SELECT cozip_integrity_hash_ok('dataset.zip');
SELECT cozip_metadata_offset('dataset.zip');
SELECT cozip_index_json('dataset.zip');

SELECT *
FROM cozip_index('dataset.zip')
ORDER BY ordinal;
```

`cozip_index(path)` returns `ordinal`, `name`, `offset_bytes`, and
`size_bytes` for each priority entry in the archive.

## Development Test

```powershell
cmake --build .\build\release --config MinSizeRel --target cozip_loadable_extension unittest
.\build\release\test\MinSizeRel\unittest.exe --test-dir . "test/sql/cozip.test"
```

If `cmake` is not on the active conda environment's `PATH`, use the Visual
Studio bundled executable directly:

```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build .\build\release --config MinSizeRel --target cozip_loadable_extension unittest
```
