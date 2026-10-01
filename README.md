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

The HMD natives dispatch through an engine-owned interface pointer: both load the same engine global at RVA `0x03CBBC28`, then the HMD device/interface at offset `+0xAD8`. `IsHeadMountedDisplayConnected` calls virtual slot `+0xB8`; `IsHeadMountedDisplayEnabled` calls virtual slot `+0xD0` and returns false when the interface is absent or reports disabled. `EnableHMD` also uses the engine stereo rendering interface at `+0xAC8`, calling virtual slot `+0x10` with the requested enabled state.

Runtime probing confirms that both `engine + 0xAD8` (HMD) and `engine + 0xAC8` (stereo rendering device) are null on the PC build at menu/aircraft-viewer time. The retained startup settings are also real generated UE4 properties, with SetBit helpers that write full bytes rather than packed bit masks:

- `bStartInVR` -> `UGeneralProjectSettings + 0x10B`
- `bStartFromVRHangar` -> `UGeneralProjectSettings + 0x10C`
- `bStartInAR` -> `UGeneralProjectSettings + 0x10D`
- `bIsVRMode` -> owning object `+ 0x10`

`EnableHMD` resolves to a separate native target at RVA `0x0118F1E0`, while `GetHMDDeviceName` dispatches through the same `engine + 0xAD8` interface.

The community UEVR compatibility plugin is also useful evidence: it resolves AC7 objects such as `AcePlayerPawn`, `CameraViewComponent`, and `NimbusPlayerCameraManager`, while its camera enum confirms a retained `VR_CAMERA` entry. Its normal path uses `COCKPIT` rather than AC7's internal `VR_CAMERA`, so the original VR path remains worth probing independently.

## Runtime probe

`src/xinput_proxy.cpp` builds a diagnostic `xinput1_3.dll` proxy. AC7 imports only XInput ordinals 2 and 3 (`XInputGetState` and `XInputSetState`), both of which are forwarded to the system DLL.

The current feasibility probe installs synthetic XR/HMD and stereo interfaces as soon as `GEngine` becomes available, but only when the corresponding PC-build slots are still null. The five ABI calls already validated from runtime disassembly keep explicit implementations; the remaining vtable slots use numbered neutral stubs so the next missing interface surface can be identified from one run.

The probe records the main module identity, code bytes at known VR/HMD targets, the HMD/stereo interface pointers, startup settings, and per-slot call counts to:

`E:\trigger_ac7vr\probe.log`

First hits to unknown slots and access-violation context are flushed immediately to:

`E:\trigger_ac7vr\evidence\persistent_slots.log`

Build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target xinput1_3
```

Output:

`build\Release\xinput1_3.dll`

For the persistent-interface test, place that DLL next to `Ace7Game.exe`, start the game manually, enter the 3D aircraft viewer, remain there for roughly 10-15 seconds, then exit normally. A crash is also useful evidence because the immediate slot trace is designed to survive it. Do not launch the game from the probe tooling.

## Current decision gate

The configuration and minimal-ABI gates are closed: `bStartInVR` reaches the real `UGeneralProjectSettings` default object, and the reconstructed interfaces satisfy the known `IsHMDConnected`, `IsHMDEnabled`, `EnableHMD`, device-enable, and stereo-enable calls.

The first persistent-interface test crashed at RVA `0x009B9939` while releasing an uninitialized shared-pointer controller. The immediate diagnostic captured the same result-buffer address for XR slot 22 and the shared-pointer destructor. UE4 4.18 identifies slot 22 as `GetXRCamera`, which returns a 16-byte `TSharedPtr` through a hidden result buffer. Returning scalar zero did not construct that result.

That failure is fixed. In the 2026-10-01 22:48 manual run the retained path stayed active until the game was closed, with no new probe exception and no matching `Ace7Game.exe` Windows Application Error/WER entry in the run window. The synthetic interfaces received sustained traffic: XR slots 22/23/26/29/30 reached thousands to more than 120,000 calls, stereo slots 0/1 exceeded 90,000 calls, and the underlying HMD device was exercised continuously. The final `ownership_lost` event had both engine slots cleared to null and occurred at shutdown, so it currently matches normal engine teardown rather than the earlier crash.

The UE4 4.18 interface definitions also expose a second return-by-value hazard: XR slot 24, `GetStereoRenderingDevice`, returns another 16-byte `TSharedPtr`. An ABI regression test now covers both shared-pointer results, plus the floating-point return ABI for HMD slot 25 (`GetLensCenterOffset`). The probe provides explicit neutral implementations for the known lifecycle and rendering calls observed so far: XR slots 24/25/27-30, HMD slots 9/12/13/25/33/35/36, and stereo slot 13 (`GetStereoLayers`). Run `ctest --test-dir build -C Release --output-on-failure` after building `fake_interface_abi_test`.

Device slot 52 remains unidentified and is the next concrete reverse-engineering target. The stock UE4.18 `IHeadMountedDisplay` vtable ends at slot 48, so slot 52 is outside the standard interface surface and may belong to an AC7-specific extension. Unknown-slot logging now records the caller address/RVA and the first four stack arguments in addition to the register arguments, so the next manual aircraft-viewer run should expose the exact AC7 callsite for slot 52 without guessing its signature. Returning neutral values still does not implement head tracking or headset rendering; OpenXR integration remains gated on completing this retained interface surface and confirming where stereo view setup begins.
