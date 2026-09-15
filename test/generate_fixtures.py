"""Generate cozip test fixtures for the DuckDB extension.

Produces four archives under test/data/.

  flat_simple.zip   plain metadata (name, offset, size, category)
  flat_geo.zip      GeoParquet metadata (adds a Point geometry column)
  flat_bad_hash.zip flat_simple.zip with a damaged integrity field
  unsupported_profile.zip
                     flat_simple.zip with valid integrity and profile 255

The 4 inner files together fit comfortably above the cozip 1.0 minimum
size of 32 KiB + 51 bytes, so the produced archives are valid without
padding.

Run from the repo root after installing the cozip writer and
geopandas:

    python test/generate_fixtures.py
"""

from __future__ import annotations
import tempfile
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
DATA_DIR = THIS_DIR / "data"

COZIP_LFH_SIZE = 51
COZIP_PROFILE_OFFSET = COZIP_LFH_SIZE + 6
COZIP_HASH_OFFSET = 43
COZIP_HASH_WINDOW_SIZE = 32768
COZIP_FNV_OFFSET_BASIS = 0xCBF29CE484222325
COZIP_FNV_PRIME = 0x100000001B3

# 4 cities, each file roughly 10 KB so the archive clears the cozip
# minimum without padding tricks.
CITIES = [
    ("lima.txt", "Lima, Peru. 12 S, 77 W.\n", -77.04, -12.05),
    ("paris.txt", "Paris, France. 48 N, 2 E.\n", 2.35, 48.86),
    ("tokyo.txt", "Tokyo, Japan. 35 N, 139 E.\n", 139.69, 35.69),
    ("denver.txt", "Denver, Colorado. 39 N, 104 W.\n", -104.99, 39.74),
]

REPEAT = 400  # ~10 KB per file


def write_inputs(tmp_root: Path) -> list[tuple[str, str]]:
    """Materialize the 4 inner files. Returns [(name, abs_path), ...]."""
    out = []
    for name, line, _, _ in CITIES:
        path = tmp_root / name
        path.write_bytes(line.encode("utf-8") * REPEAT)
        out.append((name, str(path)))
    return out


def build_simple(out_path: Path, name_path_pairs: list[tuple[str, str]]) -> None:
    """Plain flat cozip. cozip.create handles staging and packing in one call."""
    import cozip
    import pyarrow as pa

    names = [n for n, _ in name_path_pairs]
    paths = [p for _, p in name_path_pairs]
    table = pa.table(
        {
            "name": names,
            "path": paths,
            "category": ["text"] * len(names),
        }
    )
    cozip.create(out_path, table)


def build_geo(
    out_path: Path,
    name_path_pairs: list[tuple[str, str]],
    tmp_root: Path,
) -> None:
    """GeoParquet metadata, otherwise identical to flat_simple."""
    import cozip
    import geopandas as gpd
    import pyarrow as pa
    import pyarrow.parquet as pq
    from shapely.geometry import Point

    names = [n for n, _ in name_path_pairs]
    paths = [p for _, p in name_path_pairs]
    points = [Point(lon, lat) for _, _, lon, lat in CITIES]

    src_table = pa.table(
        {
            "name": names,
            "path": paths,
            "category": ["text"] * len(names),
        }
    )

    meta_arrow, staged_paths = cozip.stage_metadata(src_table)
    gdf = gpd.GeoDataFrame(
        meta_arrow.to_pandas(),
        geometry=points,
        crs="EPSG:4326",
    )

    meta_pq = tmp_root / "meta_geo.parquet"
    gdf.to_parquet(meta_pq)

    cozip.stage_create(out_path, staged_paths, meta_pq)


def build_bad_hash(source: Path, out_path: Path) -> None:
    data = bytearray(source.read_bytes())
    data[43] ^= 1
    out_path.write_bytes(data)


def fnv1a64(data: bytes | bytearray, seed: int = COZIP_FNV_OFFSET_BASIS) -> int:
    value = seed
    for byte in data:
        value ^= byte
        value = (value * COZIP_FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def build_unsupported_profile(source: Path, out_path: Path) -> None:
    """Set profile 255 and repair the integrity hash around that change."""
    data = bytearray(source.read_bytes())
    payload_size = int.from_bytes(data[18:22], "little")
    index_end = COZIP_LFH_SIZE + payload_size
    suffix_start = len(data) - COZIP_HASH_WINDOW_SIZE

    data[COZIP_PROFILE_OFFSET] = 255
    if index_end <= suffix_start:
        checksum = fnv1a64(data[COZIP_LFH_SIZE:index_end])
        checksum = fnv1a64(data[suffix_start:], checksum)
    else:
        checksum = fnv1a64(data[COZIP_LFH_SIZE:])
    data[COZIP_HASH_OFFSET : COZIP_HASH_OFFSET + 8] = checksum.to_bytes(8, "little")
    out_path.write_bytes(data)


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tmp_root = Path(tmp)
        inputs = write_inputs(tmp_root)

        simple_out = DATA_DIR / "flat_simple.zip"
        build_simple(simple_out, inputs)
        print(f"wrote {simple_out.relative_to(THIS_DIR.parent)} " f"({simple_out.stat().st_size} bytes)")

        geo_out = DATA_DIR / "flat_geo.zip"
        build_geo(geo_out, inputs, tmp_root)
        print(f"wrote {geo_out.relative_to(THIS_DIR.parent)} " f"({geo_out.stat().st_size} bytes)")

        bad_hash_out = DATA_DIR / "flat_bad_hash.zip"
        build_bad_hash(simple_out, bad_hash_out)
        print(f"wrote {bad_hash_out.relative_to(THIS_DIR.parent)} " f"({bad_hash_out.stat().st_size} bytes)")

        unsupported_out = DATA_DIR / "unsupported_profile.zip"
        build_unsupported_profile(simple_out, unsupported_out)
        print(f"wrote {unsupported_out.relative_to(THIS_DIR.parent)} " f"({unsupported_out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
