#!/usr/bin/env python3
"""Assemble the complete latest metadata path tree into one output folder."""

from __future__ import annotations

import argparse
import json
import shutil
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Artifact:
    target: str
    source: str | None
    extracted: str | None = None
    generated: bytes | None = None
    parser: str | None = None
    unsupported: bool = False


def csv_header(columns: int) -> bytes:
    return (",".join(f"field_{index}" for index in range(columns)) + "\r\n").encode()


ARTIFACTS = (
    Artifact("resource/MapList.csv", "MapList.csv", "kr_00000/lstrike/locale_kr/resource/MapList.csv"),
    Artifact("resource/itemBox.csv", "ClientTable.csv"),
    Artifact("resource/MapModeV2/ModeList.csv", "ModeList.csv"),
    Artifact("Matching.csv", "MatchOption.csv"),
    Artifact("weaponparts.csv", "weaponparts.csv"),
    Artifact("MileageShop.csv", "MileageShop.csv"),
    Artifact("resource/GameModeList.csv", "GameModeList.csv"),
    Artifact("badwordadd.csv", None, generated=b"", parser="0x02671150", unsupported=True),
    Artifact("badworddel.csv", None, generated=b"", parser="0x02671160", unsupported=True),
    Artifact("resource/zombiez/progress_unlock.csv", "progress_unlock.csv"),
    Artifact("ReinforceMaxLv.csv", "ReinforceMaxLv.csv"),
    Artifact("ReinforceMaxEXP.csv", "ReinforceMaxEXP.csv"),
    Artifact("resource/item.csv", "Item.csv", "kr_00001/lstrike/locale_kr/resource/Item.csv"),
    Artifact("voxel/voxel_list.csv", "voxel_list.csv", "kr_00000/lstrike/locale_kr/voxel/voxel_list.csv"),
    Artifact("voxel/voxel_item.csv", "voxel_item.csv"),
    Artifact("resource/codis/codisdata.cso", "CodisData.csv", "kr_00000/lstrike/locale_kr/resource/codis/codisdata.cso"),
    Artifact("HonorShop.csv", "HonorMoneyShop.csv"),
    Artifact("resource/ItemExpireTime.csv", "ItemExpireTime.csv"),
    Artifact("resource/scenariotx/scenariotx_common.json", "scenariotx_common.json"),
    Artifact("resource/scenariotx/scenariotx_dedi.json", "scenariotx_dedi.json"),
    Artifact("resource/scenariotx/shopitemlist_dedi.json", "shopitemlist_dedi.json"),
    Artifact("EpicPieceShop.csv", "EpicPieceShop.csv"),
    Artifact("models/SkinWeaponInfo_server.json", None, generated=b"{}\n", parser="0x0203e630"),
    Artifact("SeasonBadgeShop.csv", None, generated=csv_header(1), parser="0x026718d0"),
    Artifact("ppsystem/config.json", "ppsystem.json"),
    Artifact("resource/buff/classmastery.json", None, generated=b"{}\n", parser="0x0203f880"),
    Artifact("resource/zombiecompetitive/ZBCompetitive.json", "ZBCompetitive.json"),
    Artifact("resource/ModeEvent/ModeEvent.csv", "ModeEvent.csv"),
    Artifact("resource/CPShop/EventShop.csv", "EventShop.csv"),
    Artifact("resource/ClanWar/FamilyTotalWarMap.csv", "FamilyTotalWarMap.csv"),
    Artifact("resource/ClanWar/FamilyTotalWar.json", "FamilyTotalWar.json"),
    Artifact("resource/WeaponAscend.csv", None, generated=b"{}\n", parser="0x01ff1080"),
    Artifact("resource/zombi/PerkParam.csv", None, generated=csv_header(7), parser="0x025d0a80"),
    Artifact("AnniversaryShop.csv", None, generated=csv_header(8), parser="0x025d0090"),
    Artifact("resource/Anniversary/GlobalAnniversary.json", None, generated=b"", parser="0x02671cb0", unsupported=True),
    Artifact("resource/Anniversary/AnniversaryLottery.json", None, generated=b"", parser="0x02671cc0", unsupported=True),
    Artifact("resource/Anniversary/AnniversaryTicket.json", None, generated=b"", parser="0x02671cd0", unsupported=True),
    Artifact("resource/Synthesis/SynthesisItem.csv", None, generated=b"", parser="0x02671ce0", unsupported=True),
    Artifact("ZCoinShop.csv", None, generated=csv_header(4), parser="0x025d0280"),
)


def assemble(data_dir: Path, extracted_dir: Path, output_dir: Path, clean: bool) -> int:
    if clean and output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for artifact in ARTIFACTS:
        target = output_dir / Path(artifact.target)
        target.parent.mkdir(parents=True, exist_ok=True)
        extracted = extracted_dir / artifact.extracted if artifact.extracted else None
        source = data_dir / artifact.source if artifact.source else None
        if extracted and extracted.is_file():
            source = extracted
            status = "extracted"
        else:
            status = "source"
        if source and source.is_file():
            shutil.copyfile(source, target)
            size = target.stat().st_size
        elif artifact.generated is not None:
            target.write_bytes(artifact.generated)
            status = "unsupported_by_hw" if artifact.unsupported else "parser_minimal"
            size = target.stat().st_size
        else:
            raise FileNotFoundError(f"no source or parser-derived form for {artifact.target}")
        manifest.append(
            {
                "path": artifact.target,
                "status": status,
                "source": artifact.source,
                "parser": artifact.parser,
                "size": size,
            }
        )
        print(f"{status:11} {artifact.target}")

    summary = {
        "total": len(manifest),
        "source": sum(row["status"] == "source" for row in manifest),
        "extracted": sum(row["status"] == "extracted" for row in manifest),
        "parser_minimal": sum(row["status"] == "parser_minimal" for row in manifest),
        "unsupported_by_hw": sum(row["status"] == "unsupported_by_hw" for row in manifest),
        "files": manifest,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({key: summary[key] for key in ("total", "source", "extracted", "parser_minimal", "unsupported_by_hw")}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", type=Path, default=Path("bin/Data"))
    parser.add_argument(
        "--extracted-dir", type=Path, default=Path("bin/ExtractedPak")
    )
    parser.add_argument(
        "--output-dir", type=Path, default=Path("bin/MetadataArtifacts")
    )
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()
    return assemble(args.data_dir, args.extracted_dir, args.output_dir, args.clean)


if __name__ == "__main__":
    raise SystemExit(main())
