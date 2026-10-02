from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM
text = Path(r'E:\trigger_ac7vr\evidence\Ace7Game.text.runtime.bin').read_bytes()
base = 0x1000
target = 0x03CBBC28
xrefs = [base+i for i in range(len(text)-7)
         if text[i] == 0x48 and text[i+1] == 0x8B and (text[i+2] & 0xC7) == 0x05
         and base+i+7+int.from_bytes(text[i+3:i+7], 'little', signed=True) == target]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
rows = []
for xref in xrefs:
    off = xref - base
    insns = list(md.disasm(text[off:off+160], xref))
    for j, ins in enumerate(insns):
        if ins.address > xref + 48:
            break
        has_ac8 = any(op.type == CS_OP_MEM and op.mem.disp == 0xAC8 for op in ins.operands)
        if not has_ac8:
            continue
        calls = []
        for nxt in insns[j+1:j+14]:
            if nxt.mnemonic == 'call' and '[' in nxt.op_str:
                calls.append((nxt.address, nxt.op_str))
        rows.append((xref, ins.address, ins.op_str, calls))
        break
print('stereo paths:', len(rows))
for xref, load, op, calls in rows:
    cs = ', '.join(f'{a:#x}:{s}' for a,s in calls) or '<no indirect call nearby>'
    print(f'gengine={xref:#x} stereo_load={load:#x} {op} calls={cs}')
