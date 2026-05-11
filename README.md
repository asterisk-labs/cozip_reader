# cozip

A DuckDB extension to read Cloud-Optimized ZIP (cozip) archives as SQL tables, from local files or remote URLs.

cozip looks like a regular ZIP from the outside, but it carries a tiny index at byte 0 that points to an embedded Parquet metadata file. The extension opens that Parquet directly as a DuckDB table, so a multi-gigabyte archive becomes queryable with just a couple of HTTP range requests. Random access stays cheap because there is no central-directory scan at the end of the file and no full download.

## Install

```sql
INSTALL cozip FROM community;
LOAD cozip;
```

Works in DuckDB on native (Linux, macOS, Windows) and WebAssembly.

## Example

Here's a single tile from the [Major TOM VIIRS Nighttime Light](https://huggingface.co/datasets/Major-TOM/Core-VIIRS-Nighttime-Light) dataset on HuggingFace.

```sql
SELECT *
FROM read_cozip('https://huggingface.co/datasets/Major-TOM/Core-VIIRS-Nighttime-Light/resolve/main/2024/MAJORTOM-VIIRS-NTL_2024_median_000.zip')
LIMIT 10;
```

`read_cozip` opens the archive's `__metadata__` Parquet and hands you the rows directly. From there you can filter, join, or use the per-row file references to fetch only the rasters you actually need.

You can pass a local path or any remote URL DuckDB can open. `https://`, `s3://` and S3-compatible storage work directly through `httpfs`. `azure://` and `hf://datasets/...` work when the matching extension is installed (DuckDB autoloads them on demand).

`read_cozip` also adds a `cozip:gdal_vsi` column with a ready-made `/vsisubfile/...` path you can feed straight into GDAL to open the inner file without re-downloading the archive.

## License

MIT, Asterisk Labs.