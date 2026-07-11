#!/usr/bin/env python3
"""Validate MetadataArtifacts against the latest hw.dll metadata table."""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

from validate_metadata_csv import SCHEMAS, decode_file, validate


ZIP_LIMIT = 0xFFFF


@dataclass(frozen=True)
class Artifact:
    metadata_id: int | None
    path: str
    kind: str
    schema_name: str | None = None
    unsupported: bool = False
    sendable: bool = False
    allow_header_only: bool = False


ARTIFACTS = (
    Artifact(0, "resource/MapList.csv", "csv", sendable=True),
    Artifact(1, "resource/itemBox.csv", "csv", sendable=True),
    Artifact(2, "resource/MapModeV2/ModeList.csv", "csv"),
    Artifact(12, "Matching.csv", "csv", sendable=True),
    Artifact(20, "weaponparts.csv", "csv", sendable=True, allow_header_only=True),
    Artifact(21, "MileageShop.csv", "csv", "MileageShop.csv", sendable=True),
    Artifact(27, "resource/GameModeList.csv", "csv", sendable=True),
    Artifact(None, "badwordadd.csv", "raw", unsupported=True),
    Artifact(None, "badworddel.csv", "raw", unsupported=True),
    Artifact(30, "resource/zombiez/progress_unlock.csv", "csv", "progress_unlock.csv", sendable=True),
    Artifact(31, "ReinforceMaxLv.csv", "csv", "ReinforceMaxLv.csv", sendable=True),
    Artifact(32, "ReinforceMaxEXP.csv", "csv", "ReinforceMaxEXP.csv", sendable=True),
    Artifact(35, "resource/item.csv", "csv", sendable=True),
    Artifact(36, "voxel/voxel_list.csv", "csv"),
    Artifact(37, "voxel/voxel_item.csv", "csv"),
    Artifact(38, "resource/codis/codisdata.cso", "raw"),
    Artifact(39, "HonorShop.csv", "csv", "HonorMoneyShop.csv", sendable=True),
    Artifact(40, "resource/ItemExpireTime.csv", "csv-no-header", sendable=True),
    Artifact(41, "resource/scenariotx/scenariotx_common.json", "json", sendable=True),
    Artifact(42, "resource/scenariotx/scenariotx_dedi.json", "json", sendable=True),
    Artifact(43, "resource/scenariotx/shopitemlist_dedi.json", "json", sendable=True),
    Artifact(44, "EpicPieceShop.csv", "csv"),
    Artifact(45, "models/SkinWeaponInfo_server.json", "json"),
    Artifact(47, "SeasonBadgeShop.csv", "csv", allow_header_only=True),
    Artifact(48, "ppsystem/config.json", "json", sendable=True),
    Artifact(49, "resource/buff/classmastery.json", "json"),
    Artifact(51, "resource/zombiecompetitive/ZBCompetitive.json", "json", sendable=True),
    Artifact(53, "resource/ModeEvent/ModeEvent.csv", "csv", sendable=True),
    Artifact(54, "resource/CPShop/EventShop.csv", "csv", "EventShop.csv", sendable=True),
    Artifact(55, "resource/ClanWar/FamilyTotalWarMap.csv", "csv", sendable=True),
    Artifact(56, "resource/ClanWar/FamilyTotalWar.json", "json", sendable=True),
    Artifact(59, "resource/WeaponAscend.csv", "json"),
    Artifact(61, "resource/zombi/PerkParam.csv", "csv", allow_header_only=True),
    Artifact(62, "AnniversaryShop.csv", "csv", allow_header_only=True),
    Artifact(None, "resource/Anniversary/GlobalAnniversary.json", "raw", unsupported=True),
    Artifact(None, "resource/Anniversary/AnniversaryLottery.json", "raw", unsupported=True),
    Artifact(None, "resource/Anniversary/AnniversaryTicket.json", "raw", unsupported=True),
    Artifact(None, "resource/Synthesis/SynthesisItem.csv", "raw", unsupported=True),
    Artifact(69, "ZCoinShop.csv", "csv", allow_header_only=True),
)


def strip_json_comments(text: str) -> str:
    out: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(text):
        char = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""
        if in_string:
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(char)
            index += 1
        elif char == "/" and next_char == "/":
            index += 2
            while index < len(text) and text[index] not in "\r\n":
                index += 1
        elif char == "/" and next_char == "*":
            index += 2
            while index + 1 < len(text) and text[index:index + 2] != "*/":
                index += 1
            index += 2
        else:
            out.append(char)
            index += 1
    return "".join(out)


def deterministic_zip(entry: str, payload: bytes) -> bytes:
    output = io.BytesIO()
    info = zipfile.ZipInfo(entry, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(info, payload)
    return output.getvalue()


def validate_shape(path: Path, artifact: Artifact) -> tuple[int | None, list[int] | None]:
    if artifact.kind.startswith("csv"):
        text = decode_file(path)
        rows = list(csv.reader(text.splitlines()))
        data_rows = rows if artifact.kind == "csv-no-header" else rows[1:]
        nonempty = [row for row in data_rows if row and any(cell.strip() for cell in row)]
        widths = sorted({len(row) for row in nonempty})
        if not nonempty and artifact.allow_header_only and rows:
            return 0, []
        if not nonempty:
            raise ValueError("CSV has no data rows")
        schema = SCHEMAS.get(artifact.schema_name or "")
        if schema and validate(path, schema):
            raise ValueError("parser schema validation failed")
        if artifact.kind == "csv-no-header" and any(len(row) < 2 for row in nonempty):
            raise ValueError("csv-no-header rows must have at least two columns")
        return len(nonempty), widths

    if artifact.kind == "json":
        text = decode_file(path)
        cleaned = strip_json_comments(text)
        cleaned = re.sub(r",\s*([}\]])", r"\1", cleaned)
        json.loads(cleaned)
    return None, None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--artifact-dir", type=Path, default=Path("bin/MetadataArtifacts")
    )
    args = parser.parse_args()

    errors = 0
    chunked = 0
    for artifact in ARTIFACTS:
        path = args.artifact_dir / artifact.path
        if not path.is_file():
            print(f"ERROR missing path={artifact.path}")
            errors += 1
            continue

        payload = path.read_bytes()
        if artifact.unsupported:
            if payload:
                print(f"ERROR unsupported path={artifact.path} expected zero bytes")
                errors += 1
            else:
                print(f"OK unsupported path={artifact.path} raw=0")
            continue

        try:
            rows, widths = validate_shape(path, artifact)
        except Exception as error:
            print(f"ERROR path={artifact.path}: {error}")
            errors += 1
            continue

        zip_size = len(deterministic_zip(artifact.path, payload))
        status = "OK"
        if artifact.sendable and zip_size > ZIP_LIMIT:
            status = "CHUNKED"
            chunked += 1
        detail = f"raw={len(payload)} zip={zip_size}"
        if rows is not None:
            detail += f" rows={rows} widths={widths}"
        print(f"{status} id={artifact.metadata_id} path={artifact.path} {detail}")

    print(f"validated={len(ARTIFACTS)} chunked={chunked} errors={errors}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
