from pathlib import Path
import struct

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


TEXT_BASE = 0x1000
RDATA_BASE = 0x02510000
START = 0x0165F4B0
END = 0x0165F620
TARGET = 0x0165F470
MODULE_BASE = 0x7FF68CE50000


text = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.text.runtime.bin").read_bytes()
md = Cs(CS_ARCH_X86, CS_MODE_64)

for ins in md.disasm(text[START - TEXT_BASE : END - TEXT_BASE], START):
    print(f"{ins.address:08X}: {ins.mnemonic:8} {ins.op_str}")

print("\nDirect rel32 callers of 0x0165F470:")
for offset in range(len(text) - 5):
    if text[offset] != 0xE8:
        continue
    displacement = struct.unpack_from("<i", text, offset + 1)[0]
    call_rva = TEXT_BASE + offset
    if call_rva + 5 + displacement == TARGET:
        print(f"{call_rva:08X}")

pdata = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.pdata.runtime.bin").read_bytes()
print("\n.pdata entry containing 0x0165F470:")
for begin, end, unwind in struct.iter_unpack("<III", pdata):
    if begin <= TARGET < end:
        print(f"begin={begin:08X} end={end:08X} unwind={unwind:08X}")

rdata = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.rdata.runtime.bin").read_bytes()
needle = struct.pack("<Q", MODULE_BASE + TARGET)
print("\n.rdata absolute pointers to 0x0165F470:")
search_from = 0
while True:
    offset = rdata.find(needle, search_from)
    if offset < 0:
        break
    pointer_rva = RDATA_BASE + offset
    run_start = offset
    while run_start >= 8:
        previous = struct.unpack_from("<Q", rdata, run_start - 8)[0]
        if not (MODULE_BASE + TEXT_BASE <= previous < MODULE_BASE + TEXT_BASE + len(text)):
            break
        run_start -= 8
    slot_index = (offset - run_start) // 8
    type_name = "?"
    col_debug = "none"
    if run_start >= 8:
        col_va = struct.unpack_from("<Q", rdata, run_start - 8)[0]
        col_rva = col_va - MODULE_BASE
        col_offset = col_rva - RDATA_BASE
        col_debug = f"col_va={col_va:016X} col_rva={col_rva:08X}"
        if 0 <= col_offset <= len(rdata) - 24:
            signature, object_offset, cd_offset, type_rva, class_rva, self_rva = struct.unpack_from(
                "<IIIIII", rdata, col_offset
            )
            col_debug += (
                f" sig={signature} off={object_offset} cd={cd_offset} "
                f"type_rva={type_rva:08X} class_rva={class_rva:08X} self_rva={self_rva:08X}"
            )
            type_offset = type_rva - RDATA_BASE
            if signature == 1 and 0 <= type_offset + 16 < len(rdata):
                name_start = type_offset + 16
                name_end = rdata.find(b"\0", name_start)
                if name_end > name_start:
                    type_name = rdata[name_start:name_end].decode("ascii", errors="replace")
    print(
        f"{pointer_rva:08X} contiguous_text_run={RDATA_BASE + run_start:08X} "
        f"slot_index={slot_index} type={type_name} {col_debug}"
    )
    first = max(0, offset - 8 * 12)
    last = min(len(rdata), offset + 8 * 13)
    for qoff in range(first, last, 8):
        value = struct.unpack_from("<Q", rdata, qoff)[0]
        tag = ""
        if MODULE_BASE + TEXT_BASE <= value < MODULE_BASE + TEXT_BASE + len(text):
            tag = f" text_rva={value - MODULE_BASE:08X}"
        elif MODULE_BASE + RDATA_BASE <= value < MODULE_BASE + RDATA_BASE + len(rdata):
            tag = f" rdata_rva={value - MODULE_BASE:08X}"
        marker = " <== target" if qoff == offset else ""
        print(f"  {RDATA_BASE + qoff:08X}: {value:016X}{tag}{marker}")
    search_from = offset + 1
