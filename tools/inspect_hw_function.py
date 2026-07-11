#!/usr/bin/env python3
"""Disassemble selected virtual addresses from the latest 32-bit hw.dll."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("addresses", nargs="+", type=lambda value: int(value, 0))
    parser.add_argument("--dll", type=Path, default=Path("../IDA_2026_07_08/hw_2026.dll"))
    parser.add_argument("--size", type=lambda value: int(value, 0), default=0x500)
    args = parser.parse_args()

    pe = pefile.PE(str(args.dll), fast_load=True)
    image_base = pe.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    data = args.dll.read_bytes()
    for address in args.addresses:
        rva = address - image_base
        offset = pe.get_offset_from_rva(rva)
        print(f"\n=== 0x{address:08x} (file+0x{offset:x}) ===")
        for insn in md.disasm(data[offset : offset + args.size], address):
            suffix = ""
            for operand in insn.operands:
                if operand.type == 2:
                    value = operand.imm & 0xFFFFFFFF
                    if image_base <= value < image_base + pe.OPTIONAL_HEADER.SizeOfImage:
                        try:
                            string_offset = pe.get_offset_from_rva(value - image_base)
                            raw = data[string_offset : string_offset + 100].split(b"\0", 1)[0]
                            if raw and all(byte in range(0x20, 0x7f) for byte in raw):
                                suffix = f" ; {raw.decode('ascii')}"
                        except pefile.PEFormatError:
                            pass
            print(f"{insn.address:08x}  {insn.mnemonic:7} {insn.op_str}{suffix}")
            if insn.mnemonic == "ret":
                break
    return 0


if __name__ == "__main__":
    sys.exit(main())
