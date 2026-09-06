from __future__ import annotations

import json
import shutil
import struct
from datetime import datetime, timezone
from pathlib import Path
from typing import Annotated

import pyarrow as pa
from pydantic import BaseModel

import taco

DATA = Path(__file__).resolve().parent / "data"
PAYLOAD = 2500
CITIES = [(-77.04, -12.05), (2.35, 48.86), (139.69, 35.69), (-104.99, 39.74)]


class Geo(BaseModel):
    centroid: bytes
    time_start: Annotated[datetime, pa.timestamp("us", tz="UTC")]


class ML(BaseModel):
    split: str
    cloud_cover: float


class VariableML(BaseModel):
    split: str
    n_images: Annotated[int, pa.int32()]


class File(BaseModel):
    role: str
    bands: Annotated[int, pa.int32()]


class Kind(BaseModel):
    kind: str


class Raster(BaseModel):
    resolution: Annotated[int, pa.int32()]


def point(x: float, y: float) -> bytes:
    return struct.pack("<BIdd", 1, 1, x, y)


def payload(tag: str, index: int) -> bytes:
    return f"{tag}-{index}:".encode() * PAYLOAD


def sample_metadata(index: int) -> taco.Metadata:
    lon, lat = CITIES[index % len(CITIES)]
    return taco.Metadata(
        stac=Geo(centroid=point(lon, lat), time_start=datetime(2024, 1, index + 1, tzinfo=timezone.utc)),
        ml=ML(split="train" if index % 2 == 0 else "val", cloud_cover=12.5 * index),
    )


def collection(contract: taco.Contract, name: str) -> taco.Collection:
    return taco.Collection(
        contract=contract,
        id=name,
        dataset_version="1.0.0",
        description=name,
        licenses=["CC-BY-4.0"],
        providers=[{"name": "Asterisk Labs", "roles": ["producer"]}],
        tasks=["segmentation"],
        title=name,
    )


def write_flat(output: Path, *, folder: bool = False) -> None:
    contract = taco.Contract(
        structure=["image.bin", "label.bin"],
        metadata=taco.MetadataSchema(
            taco.Level("sample", stac=Geo, ml=ML),
            taco.Level("children", file=File),
        ),
    )
    target = output.with_suffix("") if folder else output
    with taco.open_writer(collection(contract, "taco-flat"), target, overwrite=True) as writer:
        for index in range(4):
            writer.add(
                taco.Sample(
                    metadata=sample_metadata(index),
                    assets=[
                        taco.Asset(
                            payload("image", index),
                            path="image.bin",
                            metadata=taco.Metadata(file=File(role="image", bands=13)),
                        ),
                        taco.Asset(
                            payload("label", index),
                            path="label.bin",
                            metadata=taco.Metadata(file=File(role="label", bands=1)),
                        ),
                    ],
                )
            )
        writer.run()


def write_nested(output: Path) -> None:
    contract = taco.Contract(
        structure=["before/B02.bin", "before/B03.bin", "after/B02.bin", "change.bin"],
        metadata=taco.MetadataSchema(
            taco.Level("sample", stac=Geo, ml=ML, majortom=taco.metadata.sample.MajorTOM()),
            taco.Level("children", node=Kind),
            taco.Level("children/before", raster=Raster),
            taco.Level("children/after", raster=Raster),
        ),
    )
    with taco.open_writer(collection(contract, "taco-nested"), output, overwrite=True) as writer:
        for index in range(3):
            writer.add(
                taco.Sample(
                    metadata=sample_metadata(index),
                    folders=[
                        taco.Folder("before", metadata=taco.Metadata(node=Kind(kind="imagery"))),
                        taco.Folder("after", metadata=taco.Metadata(node=Kind(kind="imagery"))),
                    ],
                    assets=[
                        taco.Asset(
                            payload("b02", index),
                            path="before/B02.bin",
                            metadata=taco.Metadata(raster=Raster(resolution=10)),
                        ),
                        taco.Asset(
                            payload("b03", index),
                            path="before/B03.bin",
                            metadata=taco.Metadata(raster=Raster(resolution=10)),
                        ),
                        taco.Asset(
                            payload("a02", index),
                            path="after/B02.bin",
                            metadata=taco.Metadata(raster=Raster(resolution=20)),
                        ),
                        taco.Asset(
                            payload("change", index),
                            path="change.bin",
                            metadata=taco.Metadata(node=Kind(kind="label")),
                        ),
                    ],
                )
            )
        writer.run()


def write_variable(output: Path) -> None:
    contract = taco.Contract(
        structure=["img*[0,3].bin", "mask.bin"],
        metadata=taco.MetadataSchema(
            taco.Level("sample", ml=VariableML),
            taco.Level("children", node=Kind),
        ),
    )
    with taco.open_writer(collection(contract, "taco-variable"), output, overwrite=True) as writer:
        for index in range(3):
            assets = [
                taco.Asset(
                    payload(f"img{number}", index),
                    path=f"img{number}.bin",
                    metadata=taco.Metadata(node=Kind(kind="image")),
                )
                for number in range(index)
            ]
            assets.append(
                taco.Asset(payload("mask", index), path="mask.bin", metadata=taco.Metadata(node=Kind(kind="label")))
            )
            writer.add(taco.Sample(metadata=taco.Metadata(ml=VariableML(split="train", n_images=index)), assets=assets))
        writer.run()


def write_shadow(output: Path) -> None:
    contract = taco.Contract(
        structure=["before/B02.bin", "change.bin"],
        metadata=taco.MetadataSchema(
            taco.Level("sample", ml=VariableML),
            taco.Level("children", raster=Raster),
            taco.Level("children/before", raster=Raster),
        ),
    )
    with taco.open_writer(collection(contract, "taco-shadow"), output, overwrite=True) as writer:
        for index in range(2):
            writer.add(
                taco.Sample(
                    metadata=taco.Metadata(ml=VariableML(split="train", n_images=1)),
                    folders=[taco.Folder("before", metadata=taco.Metadata(raster=Raster(resolution=1)))],
                    assets=[
                        taco.Asset(
                            payload("b02", index),
                            path="before/B02.bin",
                            metadata=taco.Metadata(raster=Raster(resolution=3)),
                        ),
                        taco.Asset(
                            payload("change", index),
                            path="change.bin",
                            metadata=taco.Metadata(raster=Raster(resolution=2)),
                        ),
                    ],
                )
            )
        writer.run()


def write_null(output: Path) -> None:
    contract = taco.Contract(
        structure=None,
        metadata=taco.MetadataSchema(taco.Level("sample", ml=VariableML)),
    )
    with taco.open_writer(collection(contract, "taco-null"), output, overwrite=True) as writer:
        for index in range(6):
            writer.add(
                taco.Sample(
                    assets=payload("sample", index),
                    metadata=taco.Metadata(ml=VariableML(split="train", n_images=index % 3)),
                )
            )
        writer.run()


def write_tacocat(output: Path) -> None:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    contract = taco.Contract(
        structure=["image.bin"],
        metadata=taco.MetadataSchema(taco.Level("sample", stac=Geo, ml=ML)),
    )
    with taco.open_writer(
        collection(contract, "taco-cat"), output / "part.zip", partition_by="ml:split", overwrite=True
    ) as writer:
        for index in range(6):
            writer.add(
                taco.Sample(
                    metadata=sample_metadata(index),
                    assets=[taco.Asset(payload("image", index), path="image.bin")],
                )
            )
        writer.run()


def main() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    for path in (DATA / "taco_folder", DATA / "taco_badjson", DATA / "taco_badversion", DATA / "taco_cat"):
        if path.exists():
            shutil.rmtree(path)
    write_flat(DATA / "taco_flat.zip")
    write_flat(DATA / "taco_folder.zip", folder=True)
    shutil.copytree(DATA / "taco_folder", DATA / "taco_badjson")
    (DATA / "taco_badjson/COLLECTION.json").write_text("{ not json")
    shutil.copytree(DATA / "taco_folder", DATA / "taco_badversion")
    collection_path = DATA / "taco_badversion/COLLECTION.json"
    data = json.loads(collection_path.read_text())
    data["taco:version"] = "2.0.0"
    collection_path.write_text(json.dumps(data))
    write_nested(DATA / "taco_nested.zip")
    write_variable(DATA / "taco_variable.zip")
    write_null(DATA / "taco_null.zip")
    write_shadow(DATA / "taco_shadow.zip")
    write_tacocat(DATA / "taco_cat")


if __name__ == "__main__":
    main()
