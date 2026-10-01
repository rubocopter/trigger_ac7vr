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

The reflected `ToggleVRTestMissionMenu` entry maps to RVA `0x0091B9A0`. Runtime disassembly shows this target is a lazy UE4 reflected-function constructor rather than the gameplay implementation. A separate command/debug registration for the same name references RVA `0x00916380`, which is now probed as the stronger candidate for the actual toggle callback. Runtime probing also resolved the native functions called by the HMD state wrappers at RVAs `0x01190460` (`IsHeadMountedDisplayConnected`) and `0x011904C0` (`IsHeadMountedDisplayEnabled`).

The HMD natives dispatch through an engine-owned interface pointer: both load the same engine global at RVA `0x03CBBC28`, then the HMD device/interface at offset `+0xAD8`. `IsHeadMountedDisplayConnected` calls virtual slot `+0xB8`; `IsHeadMountedDisplayEnabled` calls virtual slot `+0xD0` and returns false when the interface is absent or reports disabled. The probe now records this runtime pointer chain and its method addresses directly.

Runtime probing confirms that this HMD slot is null on the PC build at menu time. The retained startup settings are also real generated UE4 properties, with SetBit helpers that write full bytes rather than packed bit masks:

- `bStartInVR` -> `UGeneralProjectSettings + 0x10B`
- `bStartFromVRHangar` -> `UGeneralProjectSettings + 0x10C`
- `bStartInAR` -> `UGeneralProjectSettings + 0x10D`
- `bIsVRMode` -> owning object `+ 0x10`

`EnableHMD` resolves to a separate native target at RVA `0x0118F1E0`, while `GetHMDDeviceName` dispatches through the same `engine + 0xAD8` interface.

The community UEVR compatibility plugin is also useful evidence: it resolves AC7 objects such as `AcePlayerPawn`, `CameraViewComponent`, and `NimbusPlayerCameraManager`, while its camera enum confirms a retained `VR_CAMERA` entry. Its normal path uses `COCKPIT` rather than AC7's internal `VR_CAMERA`, so the original VR path remains worth probing independently.

## Runtime probe

`src/xinput_proxy.cpp` builds a diagnostic `xinput1_3.dll` proxy. AC7 imports only XInput ordinals 2 and 3 (`XInputGetState` and `XInputSetState`), both of which are forwarded to the system DLL.

The probe does not patch game code. Ten seconds after load it records the main module identity, code bytes at known VR/HMD targets, and the current HMD interface pointer chain to:

`E:\trigger_ac7vr\probe.log`

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

1. Test the retained `UGeneralProjectSettings` startup route with `bStartInVR=True` and `bStartFromVRHangar=True` in the user's `Game.ini`.
2. Observe whether AC7 enters its retained VR menu/camera path even though the PC HMD interface is currently null.
3. If that route is live and the HMD interface is the remaining blocker, prototype the smallest UE4 HMD interface needed to satisfy the game before adding an OpenXR renderer backend.
