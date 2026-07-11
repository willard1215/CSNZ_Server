#!/usr/bin/env python3
"""Validate local metadata CSV shape against reversed latest hw.dll schemas."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


SCHEMAS = {
    "HonorMoneyShop.csv": {
        "min_columns": 25,
        "numeric": list(range(0, 3)) + list(range(5, 25)),
        "date": [3, 4, 9, 10],
        "extra_count_column": 24,
        "extra_start_column": 25,
    },
    "ReinforceMaxLv.csv": {
        "min_columns": 8,
        "numeric": list(range(0, 8)),
        "date": [],
    },
    "ReinforceMaxEXP.csv": {
        "min_columns": 10,
        "numeric": [0],
        "float": list(range(1, 10)),
        "date": [],
    },
    "progress_unlock.csv": {
        "min_columns": 4,
        "numeric": [0, 2, 3],
        "float": [1],
        "date": [],
    },
}


def decode_file(path: Path) -> str:
    data = path.read_bytes()
    for encoding in ("utf-8-sig", "cp949", "utf-8"):
        try:
            return data.decode(encoding)
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", errors="replace")


def is_intish(value: str) -> bool:
    value = value.strip()
    if value == "":
        return False
    try:
        int(value, 0)
        return True
    except ValueError:
        return False


def is_floatish(value: str) -> bool:
    value = value.strip()
    if value == "":
        return False
    try:
        float(value)
        return True
    except ValueError:
        return False


def is_zero_or_date(value: str) -> bool:
    value = value.strip()
    if value in ("", "0"):
        return True
    parts = value.replace("-", " ").replace(":", " ").split()
    if len(parts) != 5:
        return False
    return all(part.isdigit() for part in parts)


def validate(path: Path, schema: dict) -> int:
    text = decode_file(path)
    rows = list(csv.reader(text.splitlines()))
    errors = 0
    for row_index, row in enumerate(rows[1:], start=2):
        if not row or all(not cell.strip() for cell in row):
            continue
        if len(row) < schema["min_columns"]:
            print(f"{path.name}:{row_index}: only {len(row)} columns, expected >= {schema['min_columns']}")
            errors += 1
            continue
        extra_count_column = schema.get("extra_count_column")
        if extra_count_column is not None and extra_count_column < len(row):
            if is_intish(row[extra_count_column]):
                extra_count = int(row[extra_count_column], 0)
                extra_start = schema["extra_start_column"]
                needed = extra_start + max(extra_count, 0)
                if len(row) < needed:
                    print(
                        f"{path.name}:{row_index}: only {len(row)} columns, "
                        f"column {extra_count_column} requests {extra_count} extra values, expected >= {needed}"
                    )
                    errors += 1
                for column in range(extra_start, min(needed, len(row))):
                    if row[column].strip() == "":
                        print(f"{path.name}:{row_index}: empty extra numeric column {column}")
                        errors += 1
                    elif not is_intish(row[column]):
                        print(f"{path.name}:{row_index}: non-integer extra column {column}: {row[column]!r}")
                        errors += 1
        for column in schema["numeric"]:
            if column >= len(row):
                continue
            if row[column].strip() == "":
                print(f"{path.name}:{row_index}: empty numeric column {column}")
                errors += 1
            elif not is_intish(row[column]):
                print(f"{path.name}:{row_index}: non-integer column {column}: {row[column]!r}")
                errors += 1
        for column in schema.get("float", []):
            if column >= len(row):
                continue
            if row[column].strip() == "":
                print(f"{path.name}:{row_index}: empty float column {column}")
                errors += 1
            elif not is_floatish(row[column]):
                print(f"{path.name}:{row_index}: non-float column {column}: {row[column]!r}")
                errors += 1
        for column in schema["date"]:
            if column < len(row) and not is_zero_or_date(row[column]):
                print(f"{path.name}:{row_index}: invalid date column {column}: {row[column]!r}")
                errors += 1
    print(f"{path.name}: rows={max(len(rows) - 1, 0)} errors={errors}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, default=Path("bin/Data"))
    args = parser.parse_args()

    total = 0
    for filename, schema in SCHEMAS.items():
        path = args.data_dir / filename
        if not path.exists():
            print(f"{filename}: missing")
            total += 1
            continue
        total += validate(path, schema)
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main())
