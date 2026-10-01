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

The current feasibility probe installs synthetic XR/HMD and stereo interfaces as soon as `GEngine` becomes available, but only when the corresponding PC-build slots are still null. Known ABI calls have explicit implementations; the remaining vtable slots use numbered neutral stubs so the next missing interface surface can be identified from one run. Both interfaces start disabled, and the probe currently observes enable requests without forcing them.

The probe records the main module identity, code bytes at known VR/HMD targets, the HMD/stereo interface pointers, startup settings, and per-slot call counts to:

`E:\trigger_ac7vr\probe.log`

First hits to unknown slots and access-violation context are flushed immediately to:

`E:\trigger_ac7vr\evidence\persistent_slots.log`

At the ten-second diagnostic snapshot, the probe also captures the loaded `.text`, `.rdata`, and `.pdata` sections on its monitoring thread. The manifest `evidence\runtime_image.tsv` records the module base, RVAs, sizes, and completion status. This provides runtime code, vtables, and function boundaries for offline inspection rather than requiring another manual launch for each small code window. These ignored captures are diagnostic artifacts, not an executable to launch.

Inspect a runtime code RVA or a table of pointers after the capture:

```powershell
python inspect_runtime_image.py 0x1ACCFD9
python inspect_runtime_image.py 0x2D4E7E0 --table 16
```

The inspector requires the same `capstone` Python package used by the existing disassembly scripts and rejects incomplete captures.

Build:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target xinput1_3
```

Output:

`build\Release\xinput1_3.dll`

For the persistent-interface test, place that DLL next to `Ace7Game.exe`, start the game manually, enter the 3D aircraft viewer, remain there for roughly 10-15 seconds, then exit normally. A crash is also useful evidence because the immediate slot trace is designed to survive it. Do not launch the game from the probe tooling.

The monitor stereo experiment is opt-in: create `E:\trigger_ac7vr\enable_stereo_probe.flag` before a manual launch to request it. The probe first captures its disabled baseline at ten seconds, then invokes the recovered `EnableHMD(true)` native at RVA `0x0118F1E0` once at fifteen seconds, provided it still owns both engine interfaces. The native only dispatches into the probe's HMD/stereo enable methods in this build. The request and resulting enable state are recorded in `probe.log`. Removing the flag before the next launch restores the disabled baseline; it does not disable an already running experiment.

For this experiment, leave the 3D aircraft viewer open for at least twenty seconds. The stereo interface splits each viewport into adjacent left/right rectangles, provides a 90-degree horizontal perspective with infinite reversed-Z depth, and offsets the two parallel cameras by a fixed 64 mm IPD along the camera's rotated right axis. It uses the engine's normal render target and reports no custom present, target manager, stereo layers, or spectator screen. This is a monitor-only rendering probe: there is still no headset transport or live head tracking. The canvas initialization hook is a no-op, so UI placement is not a validation target yet.

## Current decision gate

The configuration and minimal-ABI gates are closed: `bStartInVR` reaches the real `UGeneralProjectSettings` default object, and the reconstructed interfaces satisfy the known `IsHMDConnected`, `IsHMDEnabled`, `EnableHMD`, device-enable, and stereo-enable calls.

The first persistent-interface test crashed at RVA `0x009B9939` while releasing an uninitialized shared-pointer controller. The immediate diagnostic captured the same result-buffer address for XR slot 22 and the shared-pointer destructor. UE4 4.18 identifies slot 22 as `GetXRCamera`, which returns a 16-byte `TSharedPtr` through a hidden result buffer. Returning scalar zero did not construct that result.

That shared-pointer failure did not recur in the 2026-10-01 22:48 manual run, with no new probe exception and no matching `Ace7Game.exe` Windows Application Error/WER entry in the run window. The synthetic interfaces received sustained traffic: XR slots 22/23/26/29/30 reached thousands to more than 120,000 calls, stereo slots 0/1 exceeded 90,000 calls, and the underlying HMD device was exercised continuously. The final `ownership_lost` event had both engine slots cleared to null and occurred at shutdown, so it currently matches normal engine teardown rather than the earlier crash. This establishes stability of the queried interfaces, not activation of stereo rendering.

The UE4 4.18 interface definitions also expose a second return-by-value hazard: XR slot 24, `GetStereoRenderingDevice`, returns another 16-byte `TSharedPtr`. An ABI regression test now covers both shared-pointer results, plus the floating-point return ABI for HMD slot 25 (`GetLensCenterOffset`). The probe provides explicit neutral implementations for the known lifecycle and rendering calls observed so far: XR slots 24/25/27-30, HMD slots 9/12/13/25/33/35/36, and stereo slot 13 (`GetStereoLayers`). Run `ctest --test-dir build -C Release --output-on-failure` after building `fake_interface_abi_test`.

In the 2026-10-01 23:21:27 and 23:29:17 manual runs, the snapshot explicitly reported `hmd_enabled=0 stereo_enabled=0`, with no calls to the mapped enable methods (HMD device slot 11 / stereo slot 2). The engine's repeated queries therefore do not establish VR activation. The monitor stereo experiment now supplies the view/projection methods before requesting enablement. Its success gate is a recorded enabled state, calls to stereo slots 3/5/6 (view rectangle, eye offset, projection), and an observed pair of rendered views. Passing the offline ABI tests alone does not establish this gate.

The 2026-10-01 23:43:11 opt-in run crossed the activation half of that gate: `EnableHMD(true)` returned true, both synthetic interfaces reported enabled, and AC7 immediately called stereo slots 3, 5, and 6. The run then crashed inside the probe's projection stub. The exception showed `RDX=1` and `R8` pointing at the renderer's matrix output buffer, proving that AC7 calls slot 6 as `this, StereoPass, hidden FMatrix result`, while the first implementation had those last two arguments reversed and attempted to write the matrix to address `0x1`. The ABI test now pins the observed ordering. The same run also exposed HMD-device slot 40; UE4.18 maps it to `GetTextureScaleLeft()`, and the probe now initializes its hidden `FVector2D` result to zero instead of leaving the caller's buffer untouched. A fresh manual stereo run is still required to establish the rendered-view half of the gate.

The 23:29:17 run captured all three runtime sections completely: 38,857,296 bytes of `.text`, 17,074,486 bytes of `.rdata`, and 2,446,980 bytes of `.pdata`. It resolves the slot-52 route: the engine dispatch at `0x01ACCFD3` through `+0x378` reaches the function at `0x01AD3BC0`, which checks the `nohmd` command-line option, obtains the HMD device through XR slot 23, checks `IsHMDConnected`, and tail-jumps through HMD device `+0x1A0` at `0x01AD3C38`. This explains why the neutral stub's return address named the engine's outer callsite. No explicit arguments beyond `this` are prepared for that final dispatch, and the caller ignores its return, so the probe now supplies a void no-op startup hook at slot 52. The exact AC7 method name remains unknown; slot 52 is outside the standard UE4.18 device interface ending at slot 48.

The other unknown method, XR slot 9, matches `GetCurrentPose` in the UE4.18 headers and has a direct `+0x48` call at RVA `0x0165F56B` with device 0 and quaternion/vector output buffers. It still returns false through the neutral stub; the observed caller initializes those buffers to a default pose before the call.

Returning neutral values still does not implement head tracking or headset rendering. OpenXR integration remains gated on identifying the necessary interface surface and confirming where stereo view setup begins.
