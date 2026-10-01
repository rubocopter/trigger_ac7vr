import pefile, struct
from pathlib import Path
exe = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
pe = pefile.PE(str(exe), fast_load=False)
data = exe.read_bytes()
base = pe.OPTIONAL_HEADER.ImageBase
sections = []
for s in pe.sections:
    sections.append((s.Name.rstrip(b'\0').decode(errors='ignore'), s.PointerToRawData, s.SizeOfRawData, s.VirtualAddress, max(s.SizeOfRawData, s.Misc_VirtualSize)))

def rva_to_off(rva):
    for name, raw, rawsz, vrva, span in sections:
        if vrva <= rva < vrva + span:
            return raw + (rva - vrva)
    raise ValueError(hex(rva))

def describe(value):
    if not (base <= value < base + pe.OPTIONAL_HEADER.SizeOfImage):
        return ''
    rva = value - base
    for name, raw, rawsz, vrva, span in sections:
        if vrva <= rva < vrva + span:
            return f'{name} rva={rva:#x}'
    return 'image'

points = {
    'HMD_CONNECTED': 0x2b02d10,
    'HMD_ENABLED': 0x2b02d20,
    'ENABLE_HMD': 0x2b02c40,
    'GET_HMD_NAME': 0x2b02c60,
    'VR_GAME_MODE': 0x285fd38,
    'VR_MODE': 0x285fd48,
    'VR_UI_MODE': 0x285fd58,
}
for label, rva in points.items():
    off = rva_to_off(rva)
    print(f'\n[{label}] table_rva={rva:#x}')
    for delta in range(-0x30, 0x50, 8):
        pos = off + delta
        value = struct.unpack_from('<Q', data, pos)[0]
        print(f'{rva+delta:#010x} {value:#018x} {describe(value)}')
