# Testing cozip

`test/sql` holds SQLLogicTests for the DuckDB extension.

```bash
make test
```

On Windows, after configuring CMake:

```powershell
.\build\release\test\MinSizeRel\unittest.exe --test-dir . "test/sql/*"
```

## Fixtures

`test/data` is generated, not hand-written. Rebuild it with the writers from
the sibling `cozip` and `taco` projects:

```bash
python test/generate_fixtures.py        # flat_simple.zip, flat_geo.zip
python test/generate_taco_fixtures.py   # taco_*.zip, taco_folder/, taco_cat/
```

| Fixture | What it covers |
| --- | --- |
| `flat_simple.zip` | Flat profile, plain metadata |
| `flat_geo.zip` | Flat profile, GeoParquet metadata |
| `taco_flat.zip` | two fixed leaves, two contract levels |
| `taco_nested.zip` | `before/` and `after/` folders, four levels |
| `taco_variable.zip` | a variable leaf, `img*[0,3].bin` |
| `taco_null.zip` | `taco:structure` is null, one file per sample |
| `taco_folder/` | FOLDER container, no byte offsets |
| `taco_cat/` | two ZIP partitions plus their `.tacocat/` directory |

Every archive clears the cozip minimum size of 32 KiB + 51 bytes, so the
writer never has to pad them.

Note that `require cozip` skips a test file silently when the extension is
missing, so a build misconfiguration looks like a pass. Check that the
assertion count moves when you add tests.
