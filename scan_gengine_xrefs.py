from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM
from capstone.x86_const import X86_REG_RIP
text = Path(r'E:\trigger_ac7vr\evidence\Ace7Game.text.runtime.bin').read_bytes()
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
target = 0x03CBBC28
hits = []
for ins in md.disasm(text, 0x1000):
    if ins.id == 0:
        continue
    for op in ins.operands:
        if op.type == CS_OP_MEM and op.mem.base == X86_REG_RIP:
            if ins.address + ins.size + op.mem.disp == target:
                hits.append(ins.address)
                break
print('GEngine xrefs:', len(hits))
for addr in hits:
    print(f'{addr:#010x}')
