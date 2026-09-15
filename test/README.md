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

`test/data` is generated, not hand-written. Rebuild it with the writer from
the sibling `cozip` project:

```bash
python test/generate_fixtures.py
```

| Fixture | What it covers |
| --- | --- |
| `flat_simple.zip` | Flat profile, plain metadata |
| `flat_geo.zip` | Flat profile, GeoParquet metadata |
| `flat_bad_hash.zip` | Flat profile with a damaged integrity field |
| `unsupported_profile.zip` | Valid cozip archive with unknown profile `255` |

Every archive clears the cozip minimum size of 32 KiB + 51 bytes, so the
writer never has to pad them.

Note that `require cozip` skips a test file silently when the extension is
missing, so a build misconfiguration looks like a pass. Check that the
assertion count moves when you add tests.
