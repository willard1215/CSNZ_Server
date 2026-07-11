#!/usr/bin/env python3
"""Summarize CSNZ official pcap traffic around the metadata burst.

This script does not decrypt TLS/application crypto. It reports the flow,
payload timing, and TLS record sizes so we can compare the official server's
post-login transfer volume with the local metadata stream.
"""

from __future__ import annotations

import argparse
import struct
from collections import defaultdict
from pathlib import Path

from pcapng import FileScanner


def ipstr(raw: bytes) -> str:
    return ".".join(str(x) for x in raw)


def parse_tcp_payload(frame: bytes):
    if len(frame) < 14:
        return None

    eth_type = struct.unpack_from("!H", frame, 12)[0]
    offset = 14
    if eth_type == 0x8100:
        if len(frame) < 18:
            return None
        eth_type = struct.unpack_from("!H", frame, 16)[0]
        offset = 18

    if eth_type != 0x0800 or len(frame) < offset + 20:
        return None

    version_ihl = frame[offset]
    ihl = (version_ihl & 0x0F) * 4
    proto = frame[offset + 9]
    if proto != 6:
        return None

    total_len = struct.unpack_from("!H", frame, offset + 2)[0]
    src = ipstr(frame[offset + 12:offset + 16])
    dst = ipstr(frame[offset + 16:offset + 20])
    tcp_offset = offset + ihl
    if len(frame) < tcp_offset + 20:
        return None

    src_port, dst_port = struct.unpack_from("!HH", frame, tcp_offset)
    seq, ack = struct.unpack_from("!II", frame, tcp_offset + 4)
    data_offset = (frame[tcp_offset + 12] >> 4) * 4
    payload = frame[tcp_offset + data_offset:offset + total_len]
    if not payload:
        return None
    return src, src_port, dst, dst_port, seq, ack, payload


def tls_records(payload: bytes):
    pos = 0
    while pos + 5 <= len(payload):
        content_type = payload[pos]
        version = payload[pos + 1:pos + 3]
        length = struct.unpack_from("!H", payload, pos + 3)[0]
        if version[0] != 3 or pos + 5 + length > len(payload):
            break
        yield content_type, version.hex(), length
        pos += 5 + length


def tls_client_random(payload: bytes):
    if len(payload) < 43:
        return None
    if payload[0] != 0x16 or payload[5] != 0x01:
        return None
    return payload[11:43].hex()


def load_keylog_randoms(path: Path) -> set[str]:
    randoms = set()
    with path.open("rb") as fp:
        for raw in fp:
            if not raw.startswith(b"CLIENT_RANDOM "):
                continue
            parts = raw.split()
            if len(parts) >= 3:
                randoms.add(parts[1].decode("ascii", "ignore").lower())
    return randoms


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pcap", type=Path)
    parser.add_argument("--server-ip", default="222.122.48.21")
    parser.add_argument("--server-port", type=int, default=8001)
    parser.add_argument("--keylog", type=Path)
    parser.add_argument("--context", type=int, default=12)
    parser.add_argument("--rows", type=int, default=120)
    args = parser.parse_args()

    rows = []
    client_hellos = {}
    flow_bytes = defaultdict(int)
    tls_counts = defaultdict(int)
    tls_bytes = defaultdict(int)

    with args.pcap.open("rb") as fp:
        for block in FileScanner(fp):
            if not hasattr(block, "packet_data"):
                continue
            parsed = parse_tcp_payload(block.packet_data)
            if not parsed:
                continue
            src, sport, dst, dport, seq, ack, payload = parsed

            client_random = tls_client_random(payload)
            if client_random:
                client_hellos.setdefault(client_random, (src, sport, dst, dport))

            if not ((src == args.server_ip and sport == args.server_port) or
                    (dst == args.server_ip and dport == args.server_port)):
                continue

            ts = float(getattr(block, "timestamp", 0.0) or 0.0)
            direction = "S>C" if src == args.server_ip else "C>S"
            rows.append((ts, direction, len(payload), payload[:8].hex(" ")))
            flow_bytes[direction] += len(payload)
            for content_type, version, length in tls_records(payload):
                key = (direction, content_type, version)
                tls_counts[key] += 1
                tls_bytes[key] += length

    print(f"pcap={args.pcap}")
    print(f"flow={args.server_ip}:{args.server_port}")
    print(f"payload_packets={len(rows)}")
    for direction in ("S>C", "C>S"):
        print(f"{direction}_payload_bytes={flow_bytes[direction]}")

    target_randoms = [
        (random, flow) for random, flow in client_hellos.items()
        if flow[2] == args.server_ip and flow[3] == args.server_port
    ]
    print(f"pcap_client_hellos={len(client_hellos)}")
    for random, flow in target_randoms:
        print(f"target_client_random={random} flow={flow[0]}:{flow[1]}->{flow[2]}:{flow[3]}")

    if args.keylog:
        keylog_randoms = load_keylog_randoms(args.keylog)
        all_hits = [(random, flow) for random, flow in client_hellos.items() if random in keylog_randoms]
        target_hits = [(random, flow) for random, flow in target_randoms if random in keylog_randoms]
        print(f"keylog={args.keylog}")
        print(f"keylog_client_randoms={len(keylog_randoms)}")
        print(f"keylog_pcap_hits={len(all_hits)}")
        print(f"keylog_target_hits={len(target_hits)}")
        for random, flow in target_hits:
            print(f"keylog_target_hit={random} flow={flow[0]}:{flow[1]}->{flow[2]}:{flow[3]}")
        for random, flow in all_hits[:10]:
            print(f"keylog_any_hit={random} flow={flow[0]}:{flow[1]}->{flow[2]}:{flow[3]}")

    print("\ntls_records")
    for key in sorted(tls_counts):
        direction, content_type, version = key
        print(f"{direction} type=0x{content_type:02x} ver={version} "
              f"records={tls_counts[key]} bytes={tls_bytes[key]}")

    large_start = next((i for i, row in enumerate(rows)
                        if row[1] == "S>C" and row[2] > 10000), 0)
    start = max(0, large_start - args.context)
    end = min(len(rows), large_start + args.rows)
    print("\npost_login_window")
    for ts, direction, length, head in rows[start:end]:
        print(f"{ts:.6f} {direction:3} len={length:5} head={head}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
