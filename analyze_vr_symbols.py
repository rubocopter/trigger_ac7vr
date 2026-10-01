import pefile, struct
from pathlib import Path
from bisect import bisect_right
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP

exe=Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
pe=pefile.PE(str(exe), fast_load=False)
data=exe.read_bytes(); base=pe.OPTIONAL_HEADER.ImageBase
sects=[]
for s in pe.sections:
    name=s.Name.rstrip(b'\0').decode(errors='ignore')
    sects.append((name,s.PointerToRawData,s.SizeOfRawData,s.VirtualAddress,s.Misc_VirtualSize))

def off_to_rva(off):
    for n,raw,sz,rva,vsz in sects:
        if raw <= off < raw+sz: return rva+(off-raw),n
ranges=sorted((e.struct.BeginAddress,e.struct.EndAddress) for e in getattr(pe,'DIRECTORY_ENTRY_EXCEPTION',[]))
starts=[a for a,b in ranges]
def containing(rva):
    i=bisect_right(starts,rva)-1
    if i>=0:
        a,b=ranges[i]
        if a<=rva<b:return (a,b)

names=[
'IsHeadMountedDisplayConnected','IsHeadMountedDisplayEnabled','EnableHMD','GetHMDDeviceName',
'IsVRMode','IsVRGameMode','IsVRUIMode','bIsVRMode','VRMissionSelect','VRMissionLoad',
'CockpitCameraComponent','FirstPersonCameraComponent','NimbusPlayerCameraManager','GetHMDDevice'
]
# Collect all target string VAs.
target_vas={}
va_to_labels={}
for name in names:
    entries=[]
    for enc,kind in ((name.encode(),'a'),(name.encode('utf-16le'),'w')):
        start=0
        while True:
            off=data.find(enc,start)
            if off<0: break
            m=off_to_rva(off)
            if m:
                rva,sec=m; va=base+rva
                e=(kind,off,rva,sec,va)
                entries.append(e); va_to_labels.setdefault(va,[]).append((name,kind,off,rva,sec))
            start=off+1
    target_vas[name]=entries

# One linear disassembly pass; collect RIP-relative effective addresses.
text_sec=next(s for s in pe.sections if s.Name.rstrip(b'\0')==b'.text')
text=data[text_sec.PointerToRawData:text_sec.PointerToRawData+text_sec.SizeOfRawData]
text_va=base+text_sec.VirtualAddress
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
refs={va:[] for va in va_to_labels}
for ins in md.disasm(text,text_va):
    for op in ins.operands:
        if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
            tgt=ins.address+ins.size+op.mem.disp
            if tgt in refs:
                rva=ins.address-base
                refs[tgt].append((ins.address,ins.mnemonic,ins.op_str,containing(rva)))

for name in names:
    print(f'\nTARGET {name}')
    es=target_vas[name]
    if not es:
        print('  none'); continue
    for kind,off,rva,sec,va in es[:16]:
        print(f'  {kind} off={off:#x} rva={rva:#x} va={va:#x} sec={sec}')
        # absolute qword pointers to the string, typically registration/name tables
        needle=struct.pack('<Q',va); start=0; n=0
        while n<8:
            po=data.find(needle,start)
            if po<0: break
            pr=off_to_rva(po)
            print('    abs_ptr',hex(po), ('rva='+hex(pr[0])+' sec='+pr[1]) if pr else '')
            start=po+1; n+=1
        for addr,mn,ops,fn in refs.get(va,[])[:12]:
            fs='-' if not fn else f'{fn[0]:#x}-{fn[1]:#x}'
            print(f'    xref {addr:#x} {mn} {ops} fn={fs}')
