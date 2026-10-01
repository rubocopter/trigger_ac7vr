"""Inspect the loaded code/vtables saved by the diagnostic DLL (never launch AC7)."""

import argparse
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


def load_image(directory):
    base = None
    sections = {}
    for line in (directory / "runtime_image.tsv").read_text().splitlines():
        fields = line.split("\t")
        if fields[0] == "module_base":
            base = int(fields[1], 0)
        elif fields[0] == "section":
            _, name, rva, size, written, complete, filename = fields
            size = int(size, 0)
            if complete != "1" or int(written) != size:
                raise ValueError(f"Incomplete capture of {name}: {written}/{size} bytes")
            data = (directory / filename).read_bytes()
            if len(data) != size:
                raise ValueError(f"File size does not match manifest for {name}")
            sections[name] = (int(rva, 0), data)
    if base is None or not sections:
        raise ValueError("Manifest has no module base or captured sections")
    return base, sections


def read_at(sections, rva, size):
    for start, data in sections.values():
        offset = rva - start
        if 0 <= offset and offset + size <= len(data):
            return data[offset:offset + size]
    raise ValueError(f"RVA {rva:#x} + {size:#x} is outside captured sections")


def enclosing_function(sections, rva):
    if ".pdata" not in sections:
        return None
    _, data = sections[".pdata"]
    for offset in range(0, len(data) - 11, 12):
        start, end, _ = struct.unpack_from("<III", data, offset)
        if start <= rva < end:
            return start, end
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rva", type=lambda value: int(value, 0))
    parser.add_argument("--directory", type=Path, default=Path(__file__).with_name("evidence"))
    parser.add_argument("--before", type=int, default=64)
    parser.add_argument("--after", type=int, default=128)
    parser.add_argument("--table", type=int, metavar="POINTER_COUNT")
    args = parser.parse_args()
    if args.before < 0 or args.after < 1 or (args.table is not None and args.table < 1):
        parser.error("Window sizes and pointer count must be positive (before may be zero)")
    try:
        base, sections = load_image(args.directory)
        print(f"Runtime module base: {base:#x}")
        if args.table is not None:
            for index in range(args.table):
                pointer, = struct.unpack("<Q", read_at(sections, args.rva + index * 8, 8))
                label = f"module RVA {pointer - base:#x}" if base <= pointer < base + 0x0441A000 else "outside module"
                print(f"[{index:3d}] +{index * 8:#05x}: {pointer:#018x} ({label})")
            return

        bounds = enclosing_function(sections, args.rva)
        if bounds:
            start, end = bounds
            print(f"Unwind function range: {start:#x}..{end:#x}")
            # Decode from the function boundary so the displayed window starts
            # at a real instruction, even when the requested RVA is a return PC.
            stop = min(end, args.rva + args.after)
        else:
            start, stop = max(0, args.rva - args.before), args.rva + args.after
            print("No unwind entry: initial instruction alignment is unverified")
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        for ins in md.disasm(read_at(sections, start, stop - start), start):
            if ins.address < args.rva - args.before:
                continue
            marker = "=>" if ins.address == args.rva else "  "
            print(f"{marker} {ins.address:#010x}: {ins.mnemonic:8s} {ins.op_str}")
    except (OSError, ValueError) as error:
        parser.exit(1, f"Cannot inspect runtime image: {error}\n")


if __name__ == "__main__":
    main()
