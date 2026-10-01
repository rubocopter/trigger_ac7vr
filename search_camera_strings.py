from pathlib import Path
p=Path(r"C:\Program Files (x86)\Steam\steamapps\common\ACE COMBAT 7\Ace7Game.exe")
d=p.read_bytes()
names=['GetCurrentCameraViewType','SwitchToCockpitView','SwitchToFirstPersonView','SwitchToThirdPersonView','SwitchToVRView','SwitchToVrView','VR_CAMERA','VRCamera','VrCamera']
for n in names:
    hits=[]
    for enc in (n.encode(), n.encode('utf-16le')):
        pos=0
        while True:
            pos=d.find(enc,pos)
            if pos<0: break
            hits.append(hex(pos)); pos+=1
    print(n,hits[:16])
