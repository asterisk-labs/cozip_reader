# Testing cozip

The `sql` directory contains SQLLogicTests for the DuckDB extension.

On Windows, after configuring CMake, run:

```powershell
.\build\release\test\MinSizeRel\unittest.exe --test-dir . "test/sql/cozip.test"
```

The current fixture in `test/data/flat_store_6files.cozip` follows the
cozip 1.0 FLAT profile produced by the writer in the sibling `taco` project.
