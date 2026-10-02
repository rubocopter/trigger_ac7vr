from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

root = Path(r'E:\trigger_ac7vr\evidence')
code = (root / 'Ace7Game.text.runtime.bin').read_bytes()
pdata = (root / 'Ace7Game.pdata.runtime.bin').read_bytes()
md = Cs(CS_ARCH_X86, CS_MODE_64)
for target in (0x0165F56B, 0x0118FA41, 0x021F2B20):
    entries = [struct.unpack_from('<III', pdata, i) for i in range(0, len(pdata) - 11, 12)]
    match = next((entry for entry in entries if entry[0] <= target < entry[1]), None)
    if not match:
        print(f'No function for {target:08X}')
        continue
    start, end, _ = match
    print(f'POSE CONSUMER {target:08X} FUNCTION {start:08X}-{end:08X}')
    for ins in md.disasm(code[start - 0x1000:end - 0x1000], start):
        if target - 0x80 <= ins.address <= target + 0x110:
            print(f'{ins.address:08X}: {ins.mnemonic:8} {ins.op_str}')
