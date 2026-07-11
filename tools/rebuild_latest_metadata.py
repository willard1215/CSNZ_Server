#!/usr/bin/env python3
"""Rebuild every metadata payload registered by the latest CSNZ hw.dll.

The output is one deterministic ZIP per metadata id.  ZIP entry names follow
the latest client handler table, not the legacy server filenames.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import re
import shutil
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

from validate_metadata_csv import SCHEMAS, decode_file, validate


@dataclass(frozen=True)
class Metadata:
    metadata_id: int
    source: str
    entry: str
    kind: str


METADATA = (
    Metadata(0, "MapList.csv", "resource/MapList.csv", "csv"),
    Metadata(1, "ClientTable.csv", "resource/itemBox.csv", "csv"),
    Metadata(2, "ModeList.csv", "resource/MapModeV2/ModeList.csv", "csv"),
    Metadata(12, "MatchOption.csv", "Matching.csv", "csv"),
    Metadata(20, "weaponparts.csv", "weaponparts.csv", "csv"),
    Metadata(21, "MileageShop.csv", "MileageShop.csv", "csv"),
    Metadata(27, "GameModeList.csv", "resource/GameModeList.csv", "csv"),
    Metadata(30, "progress_unlock.csv", "resource/zombiez/progress_unlock.csv", "csv"),
    Metadata(31, "ReinforceMaxLv.csv", "ReinforceMaxLv.csv", "csv"),
    Metadata(32, "ReinforceMaxEXP.csv", "ReinforceMaxEXP.csv", "csv"),
    Metadata(35, "Item.csv", "resource/item.csv", "csv"),
    Metadata(39, "HonorMoneyShop.csv", "HonorShop.csv", "csv"),
    Metadata(40, "ItemExpireTime.csv", "resource/ItemExpireTime.csv", "csv-no-header"),
    Metadata(41, "scenariotx_common.json", "resource/scenariotx/scenariotx_common.json", "jsonc"),
    Metadata(42, "scenariotx_dedi.json", "resource/scenariotx/scenariotx_dedi.json", "jsonc"),
    Metadata(43, "shopitemlist_dedi.json", "resource/scenariotx/shopitemlist_dedi.json", "jsonc"),
    Metadata(48, "ppsystem.json", "ppsystem/config.json", "jsonc"),
    Metadata(51, "ZBCompetitive.json", "resource/zombiecompetitive/ZBCompetitive.json", "jsonc"),
    Metadata(53, "ModeEvent.csv", "resource/ModeEvent/ModeEvent.csv", "csv"),
    Metadata(54, "EventShop.csv", "resource/CPShop/EventShop.csv", "csv"),
    Metadata(55, "FamilyTotalWarMap.csv", "resource/ClanWar/FamilyTotalWarMap.csv", "csv"),
    Metadata(56, "FamilyTotalWar.json", "resource/ClanWar/FamilyTotalWar.json", "jsonc"),
)


def strip_json_comments(text: str) -> str:
    """Remove // and /* */ comments without touching quoted strings."""
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


def parse_jsonc(path: Path) -> None:
    text = decode_file(path)
    without_comments = strip_json_comments(text)
    without_trailing_commas = re.sub(r",\s*([}\]])", r"\1", without_comments)
    json.loads(without_trailing_commas)


def validate_csv_shape(
    path: Path, no_header: bool = False, header_rows: int = 1
) -> tuple[int, set[int]]:
    text = decode_file(path)
    rows = list(csv.reader(text.splitlines()))
    data_rows = rows if no_header else rows[header_rows:]
    widths = {
        len(row)
        for row in data_rows
        if row and any(cell.strip() for cell in row)
    }
    if not data_rows:
        raise ValueError("CSV has no data rows")
    if no_header:
        bad = [index + 1 for index, row in enumerate(rows) if row and len(row) < 2]
        if bad:
            raise ValueError(f"rows with fewer than two columns: {bad[:10]}")
    elif len(widths) != 1:
        raise ValueError(f"inconsistent data row widths: {sorted(widths)}")
    return len(data_rows), widths


def deterministic_zip(entry: str, payload: bytes) -> bytes:
    output = io.BytesIO()
    info = zipfile.ZipInfo(entry, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o100644 << 16
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(info, payload)
    return output.getvalue()


def rebuild(data_dir: Path, output_dir: Path) -> int:
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest: list[dict[str, object]] = []
    errors = 0
    for metadata in METADATA:
        source = data_dir / metadata.source
        try:
            if not source.is_file():
                raise FileNotFoundError(source)
            if metadata.kind == "jsonc":
                parse_jsonc(source)
                rows = None
                widths = None
            else:
                schema = SCHEMAS.get(metadata.source)
                rows, widths = validate_csv_shape(
                    source,
                    no_header=metadata.kind == "csv-no-header",
                    header_rows=schema.get("header_rows", 1) if schema else 1,
                )
                if schema and validate(source, schema):
                    raise ValueError("parser schema validation failed")

            payload = source.read_bytes()
            rebuilt = deterministic_zip(metadata.entry, payload)
            if len(rebuilt) > 0xFFFF:
                raise ValueError(
                    f"compressed payload is {len(rebuilt)} bytes; one-shot chunk limit is 65535"
                )
            output_name = f"{metadata.metadata_id:02d}_{Path(metadata.entry).name}.zip"
            (output_dir / output_name).write_bytes(rebuilt)
            manifest.append(
                {
                    "id": metadata.metadata_id,
                    "source": metadata.source,
                    "entry": metadata.entry,
                    "kind": metadata.kind,
                    "source_size": len(payload),
                    "zip_size": len(rebuilt),
                    "rows": rows,
                    "widths": sorted(widths) if widths is not None else None,
                    "output": output_name,
                }
            )
            print(
                f"id={metadata.metadata_id:02d} source={metadata.source} "
                f"entry={metadata.entry} zip={len(rebuilt)} OK"
            )
        except Exception as error:
            errors += 1
            print(f"id={metadata.metadata_id:02d} source={metadata.source}: ERROR: {error}")

    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(f"rebuilt={len(manifest)} errors={errors} output={output_dir}")
    return 1 if errors else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, default=Path("bin/Data"))
    parser.add_argument(
        "--output-dir", type=Path, default=Path("bin/RebuiltMetadata")
    )
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    if args.clean and args.output_dir.exists():
        shutil.rmtree(args.output_dir)
    return rebuild(args.data_dir, args.output_dir)


if __name__ == "__main__":
    sys.exit(main())
