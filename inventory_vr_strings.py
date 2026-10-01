from pathlib import Path
import re
p=Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
d=p.read_bytes()
pat=re.compile(rb'[ -~]{4,}')
seen=[]
for m in pat.finditer(d):
    s=m.group().decode('ascii','ignore')
    if ('VR' in s or 'Psvr' in s or 'PSVR' in s or 'Morpheus' in s) and len(s) <= 140:
        if any(k in s for k in ['VR','Psvr','PSVR','Morpheus']):
            seen.append((m.start(),s))
for off,s in seen:
    if any(k in s for k in ['Mode','Mission','Camera','HMD','Morpheus','Psvr','PSVR','Stereo','AirShow','ScreenPercentage','Compass','Hangar','Enable','Disable']):
        print(hex(off),s)
