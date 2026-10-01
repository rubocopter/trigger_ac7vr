from pathlib import Path
import string
p=Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
d=p.read_bytes()
for off in [0x2789f3d,0x2848953,0x284896b,0x2848993,0x284c263,0x284c2cb,0x284c303,0x284e51b,0x284ec83,0x284f34b]:
    lo=max(0,off-256); hi=min(len(d),off+384); chunk=d[lo:hi]
    runs=[]; cur=[]; start=None
    for i,b in enumerate(chunk):
        if 32 <= b < 127:
            if start is None: start=i
            cur.append(chr(b))
        else:
            if len(cur)>=4: runs.append((lo+start,''.join(cur)))
            cur=[]; start=None
    if len(cur)>=4: runs.append((lo+start,''.join(cur)))
    print(f'\n--- around {off:#x} ---')
    for pos,s in runs:
        print(hex(pos),s)
