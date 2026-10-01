# trigger_ac7vr

Experimental feasibility work for a native PC VR path in **ACE COMBAT 7: Skies Unknown**.

This repository is currently a **spike**, not a usable VR mod. The immediate goal is to determine whether the PC executable can reuse the game's retained Project Aces VR logic with a PC XR backend.

## Target build

- Game: Steam Windows build
- Engine: Unreal Engine 4.18 (`++UE4+Release-4.18`)
- `Ace7Game.exe` SHA-256: `C7DA97F5F8A807D4F1264ADBB074146FCFFE9BDC2FFA98791B822CD28E558F4F`
- Architecture: x64

## Static findings

The PC executable retains substantial game-specific VR code and data, including:

- `IsVRMode`, `IsVRGameMode`, `IsVRUIMode`, `bIsVRMode`
- `ECameraType::VR_CAMERA`
- `VRHangar`, `VRMissionSelect`, `VRMissionLoad`
- `ToggleVRTestMissionMenu`
- `bStartFromVRHangar`
- `SetVRCameraPositionX/Y/Z`
- `SetVRCompassDepth/Height/Scale`
- `SetVRHangarCameraPosition*` and `SetVRHangarCameraRotation*`
- VR-specific graphics settings and screen percentages
- UE4 HMD functions such as `EnableHMD`, `IsHeadMountedDisplayConnected`, and `IsHeadMountedDisplayEnabled`
- Morpheus/PSVR renderer strings and shaders

UE4 reflection registration tables also contain native targets for the VR/HMD UFUNCTIONs. The corresponding `.text` bytes in the executable on disk are protected/obfuscated, so useful code inspection must happen against the loaded process image.

The community UEVR compatibility plugin is also useful evidence: it resolves AC7 objects such as `AcePlayerPawn`, `CameraViewComponent`, and `NimbusPlayerCameraManager`, while its camera enum confirms a retained `VR_CAMERA` entry. Its normal path uses `COCKPIT` rather than AC7's internal `VR_CAMERA`, so the original VR path remains worth probing independently.

## Runtime probe

`src/xinput_proxy.cpp` builds a diagnostic `xinput1_3.dll` proxy. AC7 imports only XInput ordinals 2 and 3 (`XInputGetState` and `XInputSetState`), both of which are forwarded to the system DLL.

The probe does not patch game code. Ten seconds after load it records the main module identity and the first bytes at known VR/HMD reflection targets to:

`%LOCALAPPDATA%\trigger_ac7vr\probe.log`

Build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target xinput1_3
```

Output:

`build\Release\xinput1_3.dll`

For the first runtime test, place that DLL next to `Ace7Game.exe`, start the game manually, remain at the main menu for at least ten seconds, exit normally, then inspect the probe log. Remove the proxy DLL afterwards.

## Current decision gate

The next result decides the route:

1. If the VR/HMD thunks are readable/decrypted in memory, recover their runtime behavior and find the `IsVRMode` / `ToggleVRTestMissionMenu` gate.
2. If AC7 can enter its retained VR menu/camera path without a real HMD backend, add the smallest fake XR device needed to exercise that path.
3. Only after that gate works, prototype an actual OpenXR tracking/stereo backend.

