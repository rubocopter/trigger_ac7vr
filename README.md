# trigger_ac7vr

Experimental, AC7-specific VR integration for **ACE COMBAT 7: Skies Unknown** on Windows.

**Project paused by the owner on 2026-10-03.** No further development or headset tests are requested. The [project assessment](docs/project-scope.md) records the existing community mods, the lack of a demonstrated advantage over UEVR, and the criteria for any future resumption. The latest pose/FOV submission correction is preserved as an unvalidated experimental checkpoint.

This is a working experimental prototype, not a finished VR mod. A PSVR2/SteamVR user has confirmed binocular fusion, visible head rotation, apparent positional tracking, and a short gameplay session. Performance remains irregular, and complete missions, UI behavior, and broad compatibility have not been validated.

## What this project does and does not demonstrate

Both this prototype and UEVR can use Unreal's real stereo renderer. This project implements HMD/stereo interfaces specifically for AC7 and provides its own D3D11/OpenXR bridge. **It has not demonstrated better performance, image quality, or functionality than UEVR.** No controlled comparison with UEVR has been performed.

The additional research objective is to determine whether retained Project Aces VR logic can be reused on PC. Finding VR symbols and code does **not** demonstrate that original PSVR missions, assets, menus, or gameplay are available or recoverable. None of those original missions has been restored by this project.

Existing work materially changes that research objective: [Kosnag's UEVR Compatibility Mod](https://www.nexusmods.com/acecombat7skiesunknown/mods/2387) adapts VR cockpit/gameplay features, and [kokeo1's Pre-Campaign Mod](https://www.nexusmods.com/acecombat7skiesunknown/mods/3223) documents making the original VR mode playable on PC, including an UEVR variant requiring the compatibility mod. These author descriptions have been verified, but that combination has not been tested locally. Recovering original missions is therefore not a demonstrated unique opportunity for this project. For the goal of playing AC7 in VR, evaluating the existing combination should precede further standalone backend development.

The next decision gate is a comparison with UEVR under the same scene, settings, headset and runtime, alongside investigating a concrete game-specific VR feature. Continuing an independent implementation should be justified by a measured benefit or demonstrated additional functionality. Otherwise, adapting UEVR is a reasonable direction. See [the project scope and decision criteria](docs/project-scope.md).

## Current status (2026-10-03)

The D3D11/OpenXR bridge now submits paired views to SteamVR/PSVR2. Direct head rotation in stereo slot 5 was physically confirmed to move the rendered view on 2026-10-02. Runtime eye FOV fixed the fusion problem, and the user subsequently reported that head translation also appears correct while playing. The current deployed iteration reduces diagnostic I/O and records frame timing to investigate irregular performance. See [the current audit and next test](docs/current-status.md) for deployment evidence, remaining limitations, and the exact test. Older feasibility gates below describe historical runs.

New full runtime-memory captures are opt-in via `E:\trigger_ac7vr\enable_runtime_capture.flag`. With the flag absent, previously captured sections remain available and are not overwritten. Routine slot summaries are written once per second; exception and unknown-slot traces remain immediate. `graphics_bridge.log` now includes CPU frame timing summaries every 120 frames.

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

The original monitor experiment split each viewport into adjacent left/right rectangles, provided a symmetric 90-degree horizontal perspective with infinite reversed-Z depth, and offset the two parallel cameras by a fixed 64 mm IPD along the camera's rotated right axis. The current build retains that viewport split and IPD, uses the runtime's asymmetric eye FOV when available, and sends the backbuffer through a D3D11/OpenXR bridge. The canvas initialization hook remains a no-op, so UI placement is not yet a validated VR feature.

## Current decision gate

The configuration and minimal-ABI gates are closed: `bStartInVR` reaches the real `UGeneralProjectSettings` default object, and the reconstructed interfaces satisfy the known `IsHMDConnected`, `IsHMDEnabled`, `EnableHMD`, device-enable, and stereo-enable calls.

The first persistent-interface test crashed at RVA `0x009B9939` while releasing an uninitialized shared-pointer controller. The immediate diagnostic captured the same result-buffer address for XR slot 22 and the shared-pointer destructor. UE4 4.18 identifies slot 22 as `GetXRCamera`, which returns a 16-byte `TSharedPtr` through a hidden result buffer. Returning scalar zero did not construct that result.

That shared-pointer failure did not recur in the 2026-10-01 22:48 manual run, with no new probe exception and no matching `Ace7Game.exe` Windows Application Error/WER entry in the run window. The synthetic interfaces received sustained traffic: XR slots 22/23/26/29/30 reached thousands to more than 120,000 calls, stereo slots 0/1 exceeded 90,000 calls, and the underlying HMD device was exercised continuously. The final `ownership_lost` event had both engine slots cleared to null and occurred at shutdown, so it currently matches normal engine teardown rather than the earlier crash. This establishes stability of the queried interfaces, not activation of stereo rendering.

The UE4 4.18 interface definitions also expose a second return-by-value hazard: XR slot 24, `GetStereoRenderingDevice`, returns another 16-byte `TSharedPtr`. An ABI regression test now covers both shared-pointer results, the explicit `GetCurrentPose` outputs at XR slot 9, the floating-point return ABI for HMD slot 25 (`GetLensCenterOffset`), and the recovered stereo view/projection ABIs. The probe provides explicit neutral implementations for the known lifecycle and rendering calls observed so far. AC7 inserts an additional stereo virtual at slot 6, so the standard methods from `GetStereoProjectionMatrix` onward are shifted by one: projection is slot 7, render-target manager is slot 13, and stereo layers are slot 14. Run `ctest --test-dir build -C Release --output-on-failure` after building `fake_interface_abi_test`.

In the 2026-10-01 23:21:27 and 23:29:17 manual runs, the snapshot explicitly reported `hmd_enabled=0 stereo_enabled=0`, with no calls to the mapped enable methods (HMD device slot 11 / stereo slot 2). The engine's repeated queries therefore do not establish VR activation. The monitor stereo experiment now supplies the view/projection methods before requesting enablement. Its success gate is a recorded enabled state, calls to stereo slots 3/5/6/7, no new ABI exception, and an observed pair of rendered views. Passing the offline ABI tests alone does not establish this gate.

The 2026-10-01 23:43:11 opt-in run crossed the activation half of that gate: `EnableHMD(true)` returned true, both synthetic interfaces reported enabled, and AC7 immediately called stereo slots 3, 5, and 6. The run then crashed inside the slot-6 stub. The exception alone showed `RDX=1` and `R8` pointing at writable renderer state, which initially looked like a hidden matrix result. The complete captured `.text` resolves the ambiguity: the call at RVA `0x0183C514` has the same five-argument shape as the slot-5 view-offset call immediately above it (`this`, stereo pass, rotation reference, world-to-meters scale, location reference). Treating slot 6 as `GetStereoProjectionMatrix` would therefore write a 64-byte matrix over a 12-byte rotation object. The probe now treats slot 6 as AC7's additional view-offset-style virtual. The actual return-by-value projection call is slot 7 at RVA `0x0183CA58`; its caller passes a hidden 64-byte `FMatrix` result buffer and then copies four XMM words from the returned pointer. ABI regression tests cover both slots.

The 2026-10-02 follow-up crash disproved the stock UE4.18 interpretation of HMD-device slot 40. AC7's sole localized caller at RVA `0x01790EAB` obtains the device through XR `+0xB8`, explicitly loads `RDX` from `[renderer+0x78]`, calls device `+0x140`, ignores the return value, and continues. That is an object input, not a compiler-generated `FVector2D` result buffer. The previous `GetTextureScaleLeft` stub therefore wrote two zero floats over the object's first eight bytes; the later exception at RVA `0x01ACF6E4` observed a zero first qword and faulted while dispatching through it. Slot 40 is now an ABI-safe no-op for that explicit object argument, with a regression test that poisons the object's vtable and payload and verifies they remain unchanged. Its exact AC7 semantic name remains unknown.

The 2026-10-02 11:58 manual run closed the monitor-stereo integration gate. AC7 remained alive through sustained stereo rendering and the duplicated two-view image was visually confirmed on the monitor. At shutdown, stereo slots 3 and 7 had each reached 13,722 calls, slots 5 and 6 had each reached 20,583 calls, and device slot 40 had reached 6,861 calls. The final `ownership_lost` event again observed both engine interface slots already cleared to null, consistent with normal teardown. No new exception was recorded during this run.

The 2026-10-02 13:01 follow-up run validated the corrected explicit hooks in the deployed DLL. The duplicated two-view monitor image remained visible, XR/HMD slot 14 reached 7,209 calls, and device slots 37 and 41 each reached 14,418 calls before normal teardown. The run ended with `persistent_fake_hmd ownership_lost hmd_now=0 stereo_now=0`; after the final install marker the probe log contains no `exception=` or `unknown_slot` event. `persistent_slots.log` now contains only the expected view-rect and projection diagnostics, confirming that slots 14/37/41 are no longer falling through the generic unknown-slot path. Treat the synthetic ABI reconstruction phase as complete unless a concrete new crash provides contrary evidence.

That same run exposed three previously generic callsites and captured enough runtime code to resolve their argument shapes offline. XR/HMD slot 14 at RVA `0x0179123C` receives a writable result buffer and the caller immediately consumes three floats, matching a 12-byte audio-listener offset vector; the explicit hook now initializes it to zero. Device slot 37 at RVA `0x00EF9EFA` receives a caller-owned context in `RDX` plus an integer in `R8D`, with its return ignored, so its explicit hook preserves the context. Device slot 41 at RVA `0x019BF777` likewise receives caller-owned storage and has an ignored return. Although the UE4.18 reference names slot 41 `GetTextureScaleRight`, the AC7 callsite does not prove a hidden aggregate result, so the explicit hook preserves the storage until value semantics are demonstrated. Regression tests pin all three behaviors.

The complete runtime image is now the primary discovery source. Do not use repeated game launches to infer signatures that can be recovered from the captured callsites. After the offline ABI suite is green, a manual launch should be treated as a broad integration regression: paired monitor stereo must remain visible, no exception should occur, and the newly explicit slots 14/37/41 should sustain traffic without corrupting caller state. If a new slot or signature fails, analyze that callsite offline before another launch. Several further independent ABI failures after this process would be a reason to reassess the synthetic-vtable approach rather than continue patching one crash at a time.

The 23:29:17 run captured all three runtime sections completely: 38,857,296 bytes of `.text`, 17,074,486 bytes of `.rdata`, and 2,446,980 bytes of `.pdata`. It resolves the slot-52 route: the engine dispatch at `0x01ACCFD3` through `+0x378` reaches the function at `0x01AD3BC0`, which checks the `nohmd` command-line option, obtains the HMD device through XR slot 23, checks `IsHMDConnected`, and tail-jumps through HMD device `+0x1A0` at `0x01AD3C38`. This explains why the neutral stub's return address named the engine's outer callsite. No explicit arguments beyond `this` are prepared for that final dispatch, and the caller ignores its return, so the probe now supplies a void no-op startup hook at slot 52. The exact AC7 method name remains unknown; slot 52 is outside the standard UE4.18 device interface ending at slot 48.

The other previously unknown method, XR slot 9, matches `GetCurrentPose` in the UE4.18 headers and has a direct `+0x48` call at RVA `0x0165F56B` with device 0 and quaternion/vector output buffers. The explicit stub now writes an identity quaternion and zero position before returning false, so callers never consume poisoned or stale pose data.

Neutral interface methods established the ABI baseline. The later OpenXR bridge and direct slot-5 head rotation now implement headset submission and visible rotational tracking; translation and complete VR gameplay remain unverified.

The installed `Ace7Game.exe` imports `d3d11.dll` and `dxgi.dll` (`D3D11CreateDevice`, `CreateDXGIFactory`, and `CreateDXGIFactory1`). The proxy hooks DXGI factory/swapchain creation and Present, copies the side-by-side backbuffer into two OpenXR swapchains, and submits projection views. Stereo slot 12 (`GetCustomPresent`) still returns `nullptr`; submission is handled by the graphics bridge. `xrLocateViews` feeds XR slot 9 and the slot-5 rotation override. The current physical gate is measuring performance in the same scenario where the user observed irregularity.
