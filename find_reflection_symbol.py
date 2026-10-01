import argparse
import struct
from pathlib import Path

import pefile


DEFAULT_EXE = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")


def main() -> int:
    parser = argparse.ArgumentParser(description="Locate a reflected UE4 name and nearby pointer pairs in Ace7Game.exe")
    parser.add_argument("symbol", help="Exact ASCII reflected function/name to locate")
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    args = parser.parse_args()

    pe = pefile.PE(str(args.exe), fast_load=False)
    data = args.exe.read_bytes()
    image_base = pe.OPTIONAL_HEADER.ImageBase

    sections = []
    for section in pe.sections:
        sections.append(
            (
                section.Name.rstrip(b"\0").decode(errors="ignore"),
                section.PointerToRawData,
                section.SizeOfRawData,
                section.VirtualAddress,
                max(section.SizeOfRawData, section.Misc_VirtualSize),
            )
        )

    def file_offset_to_rva(offset: int) -> int | None:
        for _name, raw, raw_size, rva, _span in sections:
            if raw <= offset < raw + raw_size:
                return rva + (offset - raw)
        return None

    def describe_pointer(value: int) -> str:
        if not (image_base <= value < image_base + pe.OPTIONAL_HEADER.SizeOfImage):
            return ""
        rva = value - image_base
        for name, _raw, _raw_size, section_rva, span in sections:
            if section_rva <= rva < section_rva + span:
                description = f"{name} rva={rva:#x}"
                if name == ".rdata":
                    for _n, raw, raw_size, srva, _span in sections:
                        if srva <= rva < srva + raw_size:
                            offset = raw + (rva - srva)
                            end = data.find(b"\0", offset, min(offset + 160, len(data)))
                            if end > offset:
                                candidate = data[offset:end]
                                if all(0x20 <= byte < 0x7F for byte in candidate):
                                    description += f" string={candidate.decode('ascii')!r}"
                            break
                return description
        return "image"

    needle = args.symbol.encode("ascii") + b"\0"
    search_at = 0
    found = False
    while True:
        string_offset = data.find(needle, search_at)
        if string_offset < 0:
            break
        found = True
        string_rva = file_offset_to_rva(string_offset)
        if string_rva is None:
            search_at = string_offset + 1
            continue

        string_va = image_base + string_rva
        print(f"string file={string_offset:#x} rva={string_rva:#x} va={string_va:#x}")

        pointer_bytes = struct.pack("<Q", string_va)
        pointer_at = 0
        while True:
            pointer_offset = data.find(pointer_bytes, pointer_at)
            if pointer_offset < 0:
                break
            pointer_rva = file_offset_to_rva(pointer_offset)
            if pointer_rva is not None:
                print(f"  pointer file={pointer_offset:#x} rva={pointer_rva:#x}")
                for delta in range(-0x20, 0x38, 8):
                    qword_offset = pointer_offset + delta
                    if qword_offset < 0 or qword_offset + 8 > len(data):
                        continue
                    value = struct.unpack_from("<Q", data, qword_offset)[0]
                    nearby_rva = pointer_rva + delta
                    print(f"    {nearby_rva:#010x} {value:#018x} {describe_pointer(value)}")
            pointer_at = pointer_offset + 1

        search_at = string_offset + 1

    if not found:
        print(f"symbol not found: {args.symbol}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
