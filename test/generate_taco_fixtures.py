"""Generate TACO test fixtures for the DuckDB extension.

Produces, under test/data/:

  taco_flat.zip        2 fixed leaves, metadata at collection and sample level
  taco_nested.zip      before/ and after/ folders, 4 contract levels
  taco_variable.zip    a variable leaf img*[0,3].bin
  taco_null.zip        taco:structure = null, one file per sample
  taco_folder/         the same dataset as taco_flat in FOLDER mode
  taco_cat/            two partitions plus the consolidated .tacocat directory
  taco_badjson/        a FOLDER dataset whose COLLECTION.json is not JSON
  taco_shadow.zip      the same field name declared at two contract levels

Every archive is a cozip profile-2 container. Per cozip spec 14.5 the file
extension is .zip; the profile byte in the byte-0 index is authoritative.

Run from the repo root with the taco writer installed:

    python test/generate_taco_fixtures.py
"""

from __future__ import annotations

import shutil
import struct
import tempfile
from datetime import datetime, timezone
from pathlib import Path

import taco

THIS_DIR = Path(__file__).resolve().parent
DATA_DIR = THIS_DIR / "data"

CITIES = [
    ("lima", -77.04, -12.05),
    ("paris", 2.35, 48.86),
    ("tokyo", 139.69, 35.69),
    ("denver", -104.99, 39.74),
]
PAYLOAD = 2500  # bytes per inner file, keeps archives just over the 32 KiB floor


def wkb_point(x: float, y: float) -> bytes:
    return struct.pack("<BIdd", 1, 1, float(x), float(y))


def provider() -> dict:
    return {"name": "Asterisk Labs", "roles": ["producer"]}


def payload(tag: str, index: int) -> bytes:
    return f"{tag}-{index}:".encode() * PAYLOAD


def collection_of(contract: taco.Contract, identifier: str, description: str) -> taco.Collection:
    return taco.Collection(
        contract=contract,
        id=identifier,
        dataset_version="1.0.0",
        description=description,
        licenses=["CC-BY-4.0"],
        providers=[provider()],
        tasks=["segmentation"],
        title=description,
    )


def stac_collection_fields() -> dict:
    return {
        "stac:centroid": ["binary", "Center point in EPSG:4326 (WKB)"],
        "stac:time_start": ["timestamp[us]", "Acquisition start (UTC)"],
        "split": ["string", "Dataset split"],
        "cloud_cover": ["double", "Cloud cover percentage"],
    }


def stac_values(index: int) -> dict:
    _, lon, lat = CITIES[index % len(CITIES)]
    return {
        "stac:centroid": wkb_point(lon, lat),
        "stac:time_start": datetime(2024, 1, 1 + index, tzinfo=timezone.utc),
        "split": "train" if index % 2 == 0 else "val",
        "cloud_cover": 12.5 * index,
    }


def build_flat(out: Path) -> None:
    contract = taco.Contract(
        structure=["image.bin", "label.bin"],
        metadata={
            "collection": stac_collection_fields(),
            "sample": {"role": ["string", "Asset role"], "bands": ["int32", "Band count"]},
        },
    )
    collection = collection_of(contract, "taco-flat", "Flat TACO fixture")
    with taco.open_writer(collection, out, overwrite=True) as writer:
        for index in range(4):
            writer.add(
                taco.Sample(
                    assets={"image.bin": payload("image", index), "label.bin": payload("label", index)},
                    metadata={
                        "collection": stac_values(index),
                        "sample": {
                            "image.bin": {"role": "image", "bands": 13},
                            "label.bin": {"role": "label", "bands": 1},
                        },
                    },
                )
            )
        writer.run()


def build_nested(out: Path) -> None:
    contract = taco.Contract(
        structure=["before/B02.bin", "before/B03.bin", "after/B02.bin", "change.bin"],
        metadata={
            "collection": stac_collection_fields(),
            "sample": {"kind": ["string", "Child role"]},
            "sample/before": {"resolution": ["int32", "Metres per pixel"]},
            "sample/after": {"resolution": ["int32", "Metres per pixel"]},
        },
    )
    collection = collection_of(contract, "taco-nested", "Hierarchical TACO fixture")
    with taco.open_writer(collection, out, overwrite=True) as writer:
        for index in range(3):
            writer.add(
                taco.Sample(
                    assets={
                        "before/B02.bin": payload("b02", index),
                        "before/B03.bin": payload("b03", index),
                        "after/B02.bin": payload("a02", index),
                        "change.bin": payload("change", index),
                    },
                    metadata={
                        "collection": stac_values(index),
                        "sample": {
                            "before": {"kind": "imagery"},
                            "after": {"kind": "imagery"},
                            "change.bin": {"kind": "label"},
                        },
                        "sample/before": {"B02.bin": {"resolution": 10}, "B03.bin": {"resolution": 10}},
                        "sample/after": {"B02.bin": {"resolution": 20}},
                    },
                )
            )
        writer.run()


def build_variable(out: Path) -> None:
    contract = taco.Contract(
        structure=["img*[0,3].bin", "mask.bin"],
        metadata={
            "collection": {"split": ["string", "Dataset split"], "n_images": ["int32", "Images in this sample"]},
            "sample": {"kind": ["string", "Child role"]},
        },
    )
    collection = collection_of(contract, "taco-variable", "Variable-leaf TACO fixture")
    with taco.open_writer(collection, out, overwrite=True) as writer:
        for index in range(3):
            count = index
            assets = {f"img{k}.bin": payload(f"img{k}", index) for k in range(count)}
            assets["mask.bin"] = payload("mask", index)
            writer.add(
                taco.Sample(
                    assets=assets,
                    metadata={
                        "collection": {"split": "train", "n_images": count},
                        "sample": {
                            **{f"img{k}.bin": {"kind": "image"} for k in range(count)},
                            "mask.bin": {"kind": "label"},
                        },
                    },
                )
            )
        writer.run()


def build_shadow(out: Path) -> None:
    """The same field name at two levels. The deeper one owns its own rows."""
    contract = taco.Contract(
        structure=["before/B02.bin", "change.bin"],
        metadata={
            "collection": {"split": ["string", "Dataset split"]},
            "sample": {"resolution": ["int32", "Metres per pixel"]},
            "sample/before": {"resolution": ["int32", "Metres per pixel"]},
        },
    )
    collection = collection_of(contract, "taco-shadow", "Shadowed-field TACO fixture")
    with taco.open_writer(collection, out, overwrite=True) as writer:
        for index in range(2):
            writer.add(
                taco.Sample(
                    assets={"before/B02.bin": payload("b02", index), "change.bin": payload("change", index)},
                    metadata={
                        "collection": {"split": "train"},
                        "sample": {"before": {"resolution": 1}, "change.bin": {"resolution": 2}},
                        "sample/before": {"B02.bin": {"resolution": 3}},
                    },
                )
            )
        writer.run()


def build_null(out: Path) -> None:
    contract = taco.Contract(
        structure=None,
        metadata={"collection": {"label": ["int32", "Class id"], "split": ["string", "Dataset split"]}},
    )
    collection = collection_of(contract, "taco-null", "Single-file TACO fixture")
    with taco.open_writer(collection, out, overwrite=True) as writer:
        for index in range(6):
            writer.add(
                taco.Sample(
                    assets=payload("sample", index),
                    metadata={"collection": {"label": index % 3, "split": "train"}},
                )
            )
        writer.run()


def build_folder(archive: Path, out: Path) -> None:
    if out.exists():
        shutil.rmtree(out)
    taco.unpack(archive, out)


def build_bad_json(source: Path, out: Path) -> None:
    """A dataset the reader must reject with a clear message, not a crash."""
    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(source, out)
    (out / "COLLECTION.json").write_text("{ this is not json\n", encoding="utf-8")


def build_tacocat(out: Path) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    contract = taco.Contract(
        structure=["image.bin"],
        metadata={
            "collection": stac_collection_fields(),
            "sample": {"role": ["string", "Asset role"]},
        },
    )
    collection = collection_of(contract, "taco-cat", "Partitioned TACO fixture")
    with taco.open_writer(collection, out / "part.zip", partition_by="split", overwrite=True) as writer:
        for index in range(6):
            writer.add(
                taco.Sample(
                    assets={"image.bin": payload("image", index)},
                    metadata={"collection": stac_values(index), "sample": {"image.bin": {"role": "image"}}},
                )
            )
        writer.run()


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory():
        built = []
        for name, builder in (
            ("taco_flat.zip", build_flat),
            ("taco_nested.zip", build_nested),
            ("taco_variable.zip", build_variable),
            ("taco_null.zip", build_null),
            ("taco_shadow.zip", build_shadow),
        ):
            target = DATA_DIR / name
            builder(target)
            built.append(target)

        build_folder(DATA_DIR / "taco_flat.zip", DATA_DIR / "taco_folder")
        build_bad_json(DATA_DIR / "taco_folder", DATA_DIR / "taco_badjson")
        build_tacocat(DATA_DIR / "taco_cat")

    for target in built:
        print(f"wrote {target.relative_to(THIS_DIR.parent)} ({target.stat().st_size} bytes)")
    for directory in (DATA_DIR / "taco_folder", DATA_DIR / "taco_badjson", DATA_DIR / "taco_cat"):
        count = sum(1 for _ in directory.rglob("*") if _.is_file())
        print(f"wrote {directory.relative_to(THIS_DIR.parent)}/ ({count} files)")


if __name__ == "__main__":
    main()
