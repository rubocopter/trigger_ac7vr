from pathlib import Path

import struct

from capstone import CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, Cs
from capstone.x86_const import X86_REG_RIP


TEXT_BASE = 0x1000
RANGES = (
    (0x00A08A80, 0x00A08DF0),
    (0x00A06D80, 0x00A07020),
    (0x0183BE80, 0x0183C430),
    (0x0183C430, 0x0183C650),
    (0x0183C648, 0x0183C9D0),
    (0x0183C980, 0x0183CB10),
)

text = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.text.runtime.bin").read_bytes()
rdata = Path(r"E:\trigger_ac7vr\evidence\Ace7Game.rdata.runtime.bin").read_bytes()
RDATA_BASE = 0x02510000
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

for start, end in RANGES:
    print(f"RANGE {start:08X}-{end:08X}")
    for insn in md.disasm(text[start - TEXT_BASE : end - TEXT_BASE], start):
        suffix = ""
        for op in insn.operands:
            if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
                target = insn.address + insn.size + op.mem.disp
                offset = target - RDATA_BASE
                if 0 <= offset <= len(rdata) - 4:
                    bits = struct.unpack_from("<I", rdata, offset)[0]
                    value = struct.unpack_from("<f", rdata, offset)[0]
                    suffix = f" ; [{target:08X}] f32={value:.9g} bits={bits:08X}"
                break
        print(f"{insn.address:08X}: {insn.mnemonic:8} {insn.op_str}{suffix}")
