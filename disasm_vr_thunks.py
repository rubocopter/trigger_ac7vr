import pefile
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
exe=Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
pe=pefile.PE(str(exe), fast_load=False); data=exe.read_bytes(); base=pe.OPTIONAL_HEADER.ImageBase
sections=[]
for s in pe.sections:
    sections.append((s.Name.rstrip(b'\0').decode(errors='ignore'),s.PointerToRawData,s.SizeOfRawData,s.VirtualAddress,max(s.SizeOfRawData,s.Misc_VirtualSize)))
def rva_to_off(rva):
    for name,raw,rawsz,vrva,span in sections:
        if vrva <= rva < vrva+span: return raw+(rva-vrva)
    raise ValueError(hex(rva))
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
points={
'EnableHMD_exec':0x1192b80,
'GetHMDDeviceName_exec':0x1192db0,
'IsHMDConnected_exec':0x11937b0,
'IsHMDEnabled_exec':0x11937e0,
'IsVRGameMode_exec':0x924560,
'IsVRMode_exec':0x924590,
'IsVRUIMode_exec':0x9245c0,
}
for name,rva in points.items():
    off=rva_to_off(rva); blob=data[off:off+0x60]
    print(f'\n[{name}] rva={rva:#x} va={base+rva:#x}')
    for i,ins in enumerate(md.disasm(blob,base+rva)):
        print(f'{ins.address-base:#010x}: {ins.mnemonic:7s} {ins.op_str}')
        if i>=18 or (ins.mnemonic=='ret' and i>1): break
