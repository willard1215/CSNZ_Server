#!/usr/bin/env python3
"""Extract latest CSNZ metadata ZIP payloads from a decrypted packet stream.

Input must be the decrypted application stream that still contains CSNZ `U`
packets. A Wireshark pcap without TLS/application secrets is not enough.

Supported packet shape:
  U + uint16_le payload_size + uint16_le seq + packet_id + payload

For packet id 91 (Metadata), latest hw.dll expects:
  metadata_id + chunk_flag + uint16_le chunk_size + zip_payload

Single-chunk metadata uses chunk_flag 0x05. The ZIP payload is written as-is
and also extracted when it is a valid ZIP archive.
"""

from __future__ import annotations

import argparse
import io
import struct
import zipfile
from pathlib import Path


METADATA_NAMES = {
    0: "resource/MapList.csv",
    1: "resource/itemBox.csv",
    2: "resource/MapModeV2/ModeList.csv",
    12: "Matching.csv",
    20: "weaponparts.csv",
    21: "MileageShop.csv",
    27: "resource/GameModeList.csv",
    28: "badwordadd.csv",
    29: "badworddel.csv",
    30: "resource/zombiez/progress_unlock.csv",
    31: "ReinforceMaxLv.csv",
    32: "ReinforceMaxEXP.csv",
    35: "resource/item.csv",
    36: "voxel/voxel_list.csv",
    37: "voxel/voxel_item.csv",
    38: "resource/codis/codisdata.cso",
    39: "HonorShop.csv",
    40: "resource/ItemExpireTime.csv",
    41: "resource/scenariotx/scenariotx_common.json",
    42: "resource/scenariotx/scenariotx_dedi.json",
    43: "resource/scenariotx/shopitemlist_dedi.json",
    44: "EpicPieceShop.csv",
    45: "models/SkinWeaponInfo_server.json",
    47: "SeasonBadgeShop.csv",
    48: "ppsystem/config.json",
    49: "resource/buff/classmastery.json",
    51: "resource/zombiecompetitive/ZBCompetitive.json",
    53: "resource/ModeEvent/ModeEvent.csv",
    54: "resource/CPShop/EventShop.csv",
    55: "resource/ClanWar/FamilyTotalWarMap.csv",
    56: "resource/ClanWar/FamilyTotalWar.json",
    59: "resource/WeaponAscend.csv",
    61: "resource/zombi/PerkParam.csv",
    62: "AnniversaryShop.csv",
    63: "resource/Anniversary/GlobalAnniversary.json",
    64: "resource/Anniversary/AnniversaryLottery.json",
    65: "resource/Anniversary/AnniversaryTicket.json",
    66: "resource/Synthesis/SynthesisItem.csv",
    69: "ZCoinShop.csv",
}


def safe_name(name: str) -> str:
    return name.replace("\\", "/").strip("/").replace("..", "__")


def iter_u_packets(data: bytes):
    pos = 0
    while True:
        u = data.find(b"U", pos)
        if u < 0 or u + 6 > len(data):
            break
        size = struct.unpack_from("<H", data, u + 1)[0]
        end = u + 5 + size
        if size < 1 or end > len(data):
            pos = u + 1
            continue
        seq = struct.unpack_from("<H", data, u + 3)[0]
        packet_id = data[u + 5]
        payload = data[u + 6:end]
        yield u, seq, packet_id, payload
        pos = end


def write_metadata(out_dir: Path, metadata_id: int, chunk_flag: int, payload: bytes) -> None:
    name = METADATA_NAMES.get(metadata_id, f"metadata_{metadata_id:03d}.bin")
    zip_path = out_dir / "zip" / f"{metadata_id:03d}_{safe_name(name).replace('/', '__')}.zip"
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    zip_path.write_bytes(payload)

    extract_root = out_dir / "extracted"
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as zf:
            for member in zf.infolist():
                target = extract_root / safe_name(member.filename)
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(zf.read(member))
    except zipfile.BadZipFile:
        raw_path = out_dir / "raw" / f"{metadata_id:03d}_{safe_name(name).replace('/', '__')}.bin"
        raw_path.parent.mkdir(parents=True, exist_ok=True)
        raw_path.write_bytes(payload)

    print(f"id={metadata_id:3} flag=0x{chunk_flag:02x} size={len(payload):6} name={name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("stream", type=Path, help="decrypted CSNZ application stream")
    parser.add_argument("-o", "--out-dir", type=Path, default=Path("metadata_recovered"))
    args = parser.parse_args()

    data = args.stream.read_bytes()
    found = 0
    for offset, seq, packet_id, payload in iter_u_packets(data):
        if packet_id != 91 or len(payload) < 4:
            continue
        metadata_id = payload[0]
        chunk_flag = payload[1]
        if metadata_id not in METADATA_NAMES or chunk_flag not in (0x01, 0x02, 0x04, 0x05):
            continue
        chunk_size = struct.unpack_from("<H", payload, 2)[0]
        chunk = payload[4:4 + chunk_size]
        if len(chunk) != chunk_size:
            print(f"skip offset={offset}: truncated metadata id={metadata_id}")
            continue
        if chunk_flag == 0x05 and not chunk.startswith(b"PK\x03\x04"):
            continue
        write_metadata(args.out_dir, metadata_id, chunk_flag, chunk)
        found += 1

    print(f"metadata_packets={found}")
    if found == 0:
        print("No metadata packets found. The input is probably still encrypted or not a CSNZ stream.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
