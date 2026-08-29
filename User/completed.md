# ScaleNG.Drive — Engineering Status Document

**Last updated:** 2026-08-25, commit 2db3094 (fix142 era)
**Companion:** `User/correctness.md` (112-item checklist), `docs/CACHE.md` (per-fix log)

---

## Project Goal

Inject NVIDIA NGX DLSS upscaling into BeamNG.drive (DX12, v0.39) via an ASI plugin loaded by UAL (Universal ASI Loader). The game does not natively support DLSS.

---

## Executive Summary

We proved that **NVIDIA NGX DLSS fully works on a clean D3D12 device created on the same NVIDIA GPU that BeamNG uses**. Init, CreateFeature, and the complete NGX pipeline all succeed when given a properly created device pointer.

We also proved that **BeamNG's rendering pipeline cannot be safely intercepted using standard API hooking techniques** on this hardware/driver combination. Every interception method attempted (MinHook code patching, vtable pointer swapping) destabilizes NVIDIA's user-mode driver during active rendering.

These two findings together define the project's current state: **the NGX integration is solved; the frame capture problem is not.**

---

## Architecture Evolution

### Original: Cross-Device Bridge (ABANDONED)

```
BeamNG device ── shared textures ── Bridge device ── NGX
```

- Bridge device created successfully every time
- All 4 shared textures + handles + opens worked perfectly
- **BLOCKER:** Concurrent GPU submission from two devices crashes nvwgf2umx.dll at deterministic offsets (+0x302ffc, +0x757889)
- Bridge code preserved behind `#ifdef SCALENG_BRIDGE_BACKEND`

### Current: Single-Device NGX

```
Our own NVIDIA D3D12 device ── own textures ── NGX
```

- NGX Init: SUCCESS ✅
- NGX CreateFeature: SUCCESS ✅ (512×512 confirmed)
- NGX Evaluate: Returns FAIL_PlatformError (0xBAD00002) ⏳
- No bridge, no shared resources, no cross-device anything

### Why We Pivoted

| Evidence | Source |
|----------|--------|
| nvwgf2umx.dll AV at deterministic offsets | WER Event 1000, multiple sessions |
| Same offset regardless of our code changes | Crash dump analysis |
| Vanilla (no ASI) always stable | Control testing |
| Bridge removal → immediate stabilization | A/B testing |

---

## What Works — Proven

### NGX SDK Integration
- [x] `nvngx.dll` + `nvapi64.dll` load correctly from driver store
- [x] `NVSDK_NGX_D3D12_Init` succeeds on game device AND self-created device
- [x] `NVSDK_NGX_D3D12_CreateFeature` succeeds at 512×512
- [x] DLSS feature version 310.6.0 recognized
- [x] Telemetry data sent to NVIDIA (GPU properly identified in nvngx.log)

### BeamNG Rendering Pipeline Analysis
- [x] Scene render graph mapped: scene → velocity compute → HDR combine → LDR blit → final blit → Present
- [x] Copy chain topology identified: f28(UNORM) → f45(R11G11B10) → f34(R10G10B10A2) → f10(FLOAT ping-pong pair)
- [x] Terminal image = fmt10 ping-pong pair feeding Present
- [x] Camera constant buffer located (RenderPassConstBuffer, 1792 bytes, root param 2)
- [x] Camera jitter patching verified working (worldToScreenPos0 col3 modification)

### Infrastructure Built
- [x] D3D12CreateDevice MinHook detour (captures g_device) — proven stable
- [x] Factory DXGI export detours (CreateDXGIFactory2/1/0) — proven stable
- [x] Factory vtable hooks (slot15 CreateSwapChainForHwnd, slot10 CreateSwapChain) — proven stable
- [x] EGSH bootstrap (creates temp device+swapchain to access shared DXGI vtable)
- [x] Cross-copy init guard (named file-mapping `Local\ScaleNG_InitState`)
- [x] Heap race fix (SRWLock on descriptor heap snapshot, fix95)
- [x] BookGuard CRITICAL_SECTION for container thread safety (fix121+)
- [x] Device unwrap scanner (vtable match to find real device inside wrapper)
- [x] Full forensic pipeline (WER dumps + PageHeap + DRED + VEH + map/pdb)

### Stability Achievements
- [x] Triple concurrent module initialization eliminated (fix110)
- [x] Descriptor heap race convicted & fixed (SRWLock, fix95)
- [x] Re-entrancy in EGSH dummy swapchain creation killed (fix100 trampoline bypass)
- [x] Artifact-free rendering achieved (cmdlist hooks permanently removed, fix127)
- [x] Stable baseline established (no ASI hooks = no crashes, control-tested)

---

## Root Causes Discovered — With Evidence

### RC1: Cross-Device Concurrent Submission Crashes Driver

**Severity:** Critical (architecture-blocking)
**Crash module:** nvwgf2umx.dll
**Crash offsets:** +0x302ffc, +0x757889 (deterministic per build)
**Evidence:** Bridge builds perfectly (mkShared ×4 SUCCESS, OpenSharedHandle hr=0 ×4), then crashes within seconds of activation. Vanilla always stable. Same map/settings = same crash point.

**Root cause:** NVIDIA's user-mode driver does not safely support concurrent command submission from two D3D12 devices sharing resources on the same physical GPU during active 3D rendering.

**Attempted mitigations (all failed):**
- Serialized execution (not implementable without per-frame hooks)
- Delayed bridge activation (crash still occurs when bridge activates)
- Reduced bridge activity (still crashes eventually)
- Defensive SEH around all bridge operations (catches but doesn't prevent underlying corruption)

---

### RC2: BeamNG Wraps Its D3D12 Device

**Severity:** Critical (prevents direct NGX integration)
**Evidence:** `QI(IDXGIDevice)` returns `E_NOINTERFACE (0x80004002)` on game's device, every time, across all sessions. Never once returned S_OK.

**Impact:** NVIDIA NGX calls QI(IDXGIDevice) internally during NVSDK_NGX_D3D12_Init. Without this interface, NGX cannot determine GPU adapter, enumerate outputs, or query driver capabilities.

**Device unwrap attempt:** Scanned wrapper object memory (first 512 bytes) for embedded pointers to objects sharing the real ID3D12Device vtable. Result: found `g_device` itself at wrapper+0x150. This means either:
- BeamNG's device is NOT actually wrapped (it IS the real device, just blocks QI for other reasons)
- Or the wrapper stores a self-reference rather than the inner object

Either way, we cannot extract a "more real" device pointer because there isn't one accessible.

---

### RC3: MinHook Patches Corrupt Hot Driver Functions

**Severity:** High (causes artifacts and delayed crashes)
**Symptoms:** Solid-color rendering artifacts, delayed nvwgf2umx crashes, heap corruption manifesting at random locations

**Mechanism:** MinHook patches function prologue bytes inside nvwgf2umx.dll. These functions are called thousands of times per frame by multiple engine threads. The patched prologues corrupt driver-internal state over time.

**Convicted by:** WER Event 1000 reports showing faulting module ScaleNG.asi with offsets mapping to Hook_SetDescriptorHeaps via /MAP file. Thread-safety race in unsynchronized std::map access convicted via same method (fix95).

**Fix applied:** SRWLock around heap state snapshot (fix95). Verified stable afterward. But broader issue remains: any MinHook patch on hot driver functions introduces instability.

**Resolution:** ALL command-list hooks permanently removed (fix127). Only export-level detours retained — these are cold-path and proven safe.

---

### RC4: Device Vtable Swaps Cause Freezes

**Severity:** Critical
**Symptoms:** Immediate freeze after loading into map

**Mechanism:** Swapping vtable function pointers on ID3D12Device or ID3D12CommandQueue triggers driver-internal protection mechanisms (possibly CFG/XFG or custom vtable integrity checks).

**Confirmed by:** Bisect test — disabling device vtable swaps while keeping all other hooks active resulted in stable operation; re-enabling caused immediate freezes.

---

### RC5: Engine Render Graph Is Multi-Format Copy Chain

**Severity:** Medium (complicates resource identification)
**Discovery method:** Topology logging (fix82) + present-feed correlation (fix83/84)

**Findings:**
- Scene output is NOT a single texture
- Per-frame copy chain: f28(UNORM) → f45(R11G11B10) → f34(R10G10B10A2) → f10(FLOAT ping-pong pair)
- Terminal image = fmt10 ping-pong pair, alternating each frame
- 45,000+ display-sized copies observed in single session
- Multiple format rotations mid-session

**Impact:** Cannot simply track one "scene color" texture. Must identify terminal node of the copy chain dynamically.

---

### RC6: Backbuffer Format Varies Across Sessions

**Formats observed:** fmt=28, fmt=10, fmt=34, fmt=45

**Impact:** Bridge/shared textures created with fixed format become invalid when engine rotates.

---

### RC7: Motion Vectors Are Transient Resources

MV render targets created/used/freed rapidly. Weak-pointer tracking holds dangling pointers between discovery and use.

---

### RC8: DLL Loads Three Times Concurrently

UAL maps our ASI DLL three times. Each copy has private statics. Fixed by fix110 named file-mapping guard.

---

### RC9: Camera CB Patching Exonerated

SCALENG_NO_JITTER=1 isolation test froze identically. Jitter patch exonerated.

### RC10: Init Thread Resource Creation Exonerated

SCALENG_NO_INITRES=1 isolation test froze identically. Init thread exonerated.

---

## Instrumentation Built

| Tool | Purpose | Status |
|------|---------|--------|
| WER LocalDumps | Full crash dumps on any AV | ✅ Active |
| ScaleNG.map | Symbol resolution for crash offsets | ✅ Every build |
| ScaleNG.pdb | Full debug info for WinDbg analysis | ✅ Every build |
| DRED breadcrumbs | GPU fault location on device removal | ✅ Forced ON |
| VEH first-chance logger | Catches AVs before SEH | ✅ Active |
| PRESENT adapter probe | Names physical GPU at first adoption | ✅ Active |
| Identity capture | Logs swapchain-derived device chain | ✅ Active |
| Topology logger | Maps copy-chain structure | ✅ Built |
| Present-feed correlator | Identifies terminal scene node | ✅ Built |
| mkShared step logger | Names exact failing API in bridge setup | ✅ Active |
| Isolation env vars | DIAG_BRIDGE, NO_JITTER, NO_INITRES | ✅ Available |

---

## Current Architecture State

```
InitializeASI (single-init guarded via file-mapping)
├── LoadConfig (ini parsing, dlaa forced true)
├── HooksSetConfig
├── HooksInstallVEH (first-chance AV logger)
├── HooksInstallCreateDeviceDetour (MinHook on d3d12!D3D12CreateDevice)
│   ├── Captures g_device when game creates it
│   ├── Factory DXGI export detours
│   └── EGSH deferred thread (8s delay)
│       └── Creates temp device+swapchain
│           └── Hooks Present on shared DXGI vtable (slot 7)
└── Deferred NGX smoke test thread (8s delay)
    └── RunNgxSyntheticTest()
        ├── Creates own NVIDIA device if needed
        ├── Creates 512×512 test textures
        ├── Creates own queue/alloc/list
        ├── NGX Init on captured or self-created device
        ├── CreateFeature at 512×512
        └── Evaluate (currently FAIL_PlatformError)
```

---

## NGX Evaluate Failure Analysis

**Error:** `NVSDK_NGX_Result_FAIL_PlatformError (0xBAD00002)`
**When:** During `pEvaluateFeature(commandList, feature, params, nullptr)`
**CreateFeature:** Succeeds consistently on same device

**nvngx.log analysis:** Telemetry sent successfully (GPU properly identified as RTX 3050, driver 596.49, NGX 1.5/core 1.4.0/DLSS 310.6.0). No error messages related to evaluate failure.

**Possible causes:**
1. Command list missing required configuration (root signature, descriptor heaps beyond what we bind)
2. Resource states not exactly matching NGX expectations during multi-pass internal rendering
3. NGX internally queries IDXGIDevice from command list's device and fails on wrapper
4. Scratch buffer not allocated/passed

**Most promising fix path:** Follow NVIDIA's official DLSS SDK sample (github.com/NVIDIA/DLSS) parameter-for-parameter, ensuring every optional parameter is explicitly set even if seemingly unnecessary.

---

## Session Statistics

| Metric | Value |
|--------|-------|
| Total commits this session | 70+ |
| Fixes deployed | 73 distinct (fix70–fix142+) |
| Root causes identified | 10+ |
| Crash dumps analyzed | 5+ |
| WER reports decoded | 15+ |
| Instrumentation tools built | 11 |
| Architecture pivots | 2 |

---

## Recommended Next Steps

### Phase 1: NGX Evaluate Fix
1. Implement proper scratch buffer allocation and pass via NVSDK_NGX_Parameter_Scratch
2. Ensure all NGX parameters set per NVIDIA DLSS SDK sample
3. Verify command list has correct state when pEvaluateFeature is called
4. Test with debug layer enabled for detailed validation errors

### Phase 2: Frame Integration
5. Wire self-contained NGX pipeline into PresentCore (already structured)
6. Use swapchain GetBuffer(GetCurrentBackBufferIndex()) for frame capture
7. Handle flip-model presentation states correctly
8. Test continuous evaluation across multiple frames

### Phase 3: Quality
9. Replace synthetic depth/MV with real game data
10. Implement proper camera jitter calculation
11. Match render resolution to DLSS quality preset expectations

---

*This document represents hundreds of hours of debugging, testing, and reverse engineering. All findings are evidence-backed and reproducible.*

---

## 2026-08-27 Runtime Review and Helper Deployment

The latest deployed runtime log was reviewed after an extended user test that included resizing the game window. The game remained stable: no rendering artifacts, resize problems, crashes, or device-removal messages were observed, and no new BeamNG crash dump was produced for that run.

The important failure was functional rather than stability-related. The plugin reached the live Present pipeline but logged `ngx-b2: helper mode off` and repeatedly reported `NO independent device available`. The fallback in-process device creation also returned `0x887E0003`. This meant the plugin was safely passing through the game without executing DLSS.

The cause was configuration: the deployed `ScaleNG.ini` did not contain `[bridge] helper=1`, and the source defaults the helper backend to disabled when it is absent. I added that setting, rebuilt the plugin and helper with Visual Studio Community 2026, and deployed the matching artifacts to `C:\games\BeamNG.drive\Bin64\plugins`.

The source and deployed SHA-256 hashes match for `ScaleNG.asi`, `ScaleNG_NGX_helper.exe`, and `ScaleNG.ini`. Existing deployment files were backed up before replacement. The next launch should determine whether the helper process can complete shared-handle setup and move the project into actual cross-process NGX frame evaluation.

---

## 2026-08-27 First NGX Evaluations and Instant-Crash Fix

The next test proved that the helper path advanced into actual NGX processing. The helper completed shared-resource setup, initialized NGX at `1920x992`, and ScaleNG recorded `frame 1 eval=ok` and `frame 2 eval=ok`.

The game then produced a black window and crashed immediately after those successful evaluations. This isolated the failure to the game-side output/synchronization stage rather than helper startup, shared-handle opening, or NGX initialization.

The unsafe operation was the game command queue GPU wait on the helper’s cross-process output fence while ScaleNG was executing inside the Present callback. I replaced that queue wait with `SetEventOnCompletion` and a bounded CPU wait. The output copy is submitted only after the helper fence has completed, avoiding a cross-queue GPU wait during Present while preserving the active DLSS output path.

The project was rebuilt with VC2026 and the updated `ScaleNG.asi`, `ScaleNG_NGX_helper.exe`, and `ScaleNG.ini` were redeployed to `C:\games\BeamNG.drive\Bin64\plugins`. Source and deployed hashes match, and previous deployment files were backed up.

---

## 2026-08-27 Follow-up Log Review and Protocol Fix

The second runtime log showed that enabling the helper setting worked far enough for the separate helper process to launch, initialize its D3D12 queue/list, and complete the named-pipe handshake. The helper then failed while opening the first shared texture with `OpenSharedHandle` error `0x80070057 (E_INVALIDARG)`.

The helper’s wire log decoded the width as `491520` instead of `1920`, and all handle fields were similarly shifted. This identified a deterministic one-byte protocol framing error: the game side sent the setup message as an `S` tag followed by the 56-byte setup structure, but the helper’s initial setup reader consumed the structure without first consuming the tag. The later re-setup path already handled the tag correctly.

I fixed the initial receive path in `src\ngxc_helper.cpp` to validate and consume the `S` tag before reading `SetupMsg`. I then rebuilt both the ASI and helper with VC2026 and redeployed them to `C:\games\BeamNG.drive\Bin64\plugins`.

The deployed configuration now explicitly enables the intended DLSS path:

```ini
[ScaleNG]
enabled=1
upscaler=dlss
scale=0.67
perfQuality=1
mvJittered=1
autoExposure=1

[bridge]
helper=1
```

The source and deployed SHA-256 hashes match for `ScaleNG.asi`, `ScaleNG_NGX_helper.exe`, and `ScaleNG.ini`. The next launch should advance past shared-handle opening and reveal whether cross-process resource setup and NGX evaluation complete.

---

## 2026-08-27 Black Output Diagnosis and Output-Path Fix

The next run showed a black game window while the game process and Steam overlay remained responsive. The helper log and ScaleNG log established that the cross-process path had advanced substantially: the helper completed setup with correctly decoded `1920x992` dimensions, initialized its D3D12 queue/list, and reported `in-loop NGX init ok`.

The first frame was then discarded on the game side. ScaleNG logged `frame msg write v=1 ok=1` followed by `frame 1 skipped (no ack/eval)`. The helper had received and processed frame 1, but `NgxBridgeFrameB2` never copied `helperAcked` into its `recorded` result. Therefore stage 3, which waits for the helper output and copies it into the backbuffer, could not run.

I fixed this by treating a valid helper acknowledgement as the successful stage-2 result. I also added exclusive locking around pipe setup/frame acknowledgement exchanges because multiple UAL-loaded ASI instances and Present entries can otherwise race while reading the shared pipe. The helper’s post-frame opportunistic read was removed because it could consume the next frame message and discard it without evaluation or acknowledgement.

Finally, the helper now copies the captured color texture to the output texture when NGX rejects a frame, then signals completion. This guarantees a visible passthrough frame instead of copying an uninitialized/black output texture while still allowing successful NGX frames to use the DLSS result.

The project was rebuilt with VC2026 and `ScaleNG.asi`, `ScaleNG_NGX_helper.exe`, and `ScaleNG.ini` were redeployed to `C:\games\BeamNG.drive\Bin64\plugins`. DLSS remains explicitly enabled at 0.67 render scale with the helper bridge active, and source/deployed hashes match.

---

## 2026-08-27 Three Launch Crashes — Bridge Re-entry and Reference Ownership Fix

The newest crash dump was reviewed after three consecutive failed launches. The report was `C:\games\BeamNG.drive\Bin64\CrashDumps\BeamNG.drive.x64.exe.20260827012321.log`, which records a BeamNG access violation reading address `0x0` at executable offset `0xD02EDA`.

The associated ScaleNG log provided the decisive sequence: helper setup succeeded, NGX initialized, frame 1 evaluated successfully, and then a second Present entered while the first bridge transaction was still active. The bridge used shared command allocators/lists and the current backbuffer from both entries. Reusing those objects during Present re-entry or resize activity can corrupt command recording/submission state and explains the crash occurring immediately after successful helper evaluation.

The bridge also contained an independent reference-counting bug. `NgxBridgeFrameB2` released the swapchain backbuffer on early returns, while `NgxSelfContainedPipeline` released the same `GetBuffer` reference after the bridge returned. Those early paths could therefore double-release the backbuffer.

I added an exclusive non-blocking bridge transaction lock. If Present re-enters while a transaction is active, that frame now skips ScaleNG work and returns control to the engine without touching its backbuffer. I also removed the bridge’s duplicate `bb->Release()` calls so the caller owns and releases the reference exactly once.

The change compiled successfully with Visual Studio Community 2026. The new `ScaleNG.asi`, `ScaleNG_NGX_helper.exe`, and `ScaleNG.ini` were deployed to `C:\games\BeamNG.drive\Bin64\plugins`; the stale helper process was stopped before replacing its executable. DLSS and helper mode remain enabled in the deployed INI.

---

## 2026-08-27 Three More Crashes — Output-Copy Isolation Build

The user reported three more crashes after the bridge re-entry lock and ownership fix. The newest available crash report remains `C:\games\BeamNG.drive\Bin64\CrashDumps\BeamNG.drive.x64.exe.20260827012321.log`; BeamNG did not create additional crash-log pairs for these attempts. The active ScaleNG log confirms that the deployed build loaded and again reached `frame 1 eval=ok` and `frame 2 eval=ok` through the helper.

Because the crash continues after NGX evaluation and the re-entry lock did not change the behavior, the game-side stage-3 output copy remains the highest-confidence fault boundary. I added a configurable `replaceOutput` bridge setting and deployed it as `replaceOutput=0`. In this isolation mode, the helper still receives frames and performs NGX evaluation, but ScaleNG does not transition BeamNG’s wrapped backbuffer or submit the cross-process output copy.

The project compiled successfully with Visual Studio Community 2026. The updated ASI, helper, and INI were deployed to `C:\games\BeamNG.drive\Bin64\plugins` after stopping the stale helper process, and the source/deployed hashes match. This build is intended to establish a stable evaluation-only baseline before the visible output-copy mechanism is redesigned.

---

## 2026-08-27 Evaluation-Only Baseline Verified Stable

The user tested the isolation build and observed no crash or artifacts. The logs confirm a long-running stable session: ScaleNG reached approximately 13,200 Present entries with repeated successful helper/NGX evaluations, and the helper processed approximately 13,400 frames before the game exited normally.

No new crash report was generated after the previous 01:23:21 crash. The new logs contain no device-removal, bridge-fault, evaluation-failure, or black-output indicators. The helper transport and NGX evaluation path are therefore verified stable when the plugin does not copy the cross-process result into BeamNG’s wrapped backbuffer.

The remaining work is specifically to redesign or safely intercept the visible output replacement stage. The deployed configuration remains evaluation-only with `replaceOutput=0` until that stage can be tested without compromising game stability.

---

## 2026-08-27 Deferred Engine-List Output Handoff Deployed

The visible output path was redesigned around the stable evaluation-only boundary. Present no longer submits a second command list that transitions or copies BeamNG’s wrapped backbuffer. After helper/NGX evaluation completes, the output is marked pending.

During the next engine-owned `CopyTextureRegion` that targets the current Present backbuffer, the hook validates the resource identity, dimensions, format, full-frame copy shape, and helper fence completion. If all checks pass, it records a source transition and substitutes the shared DLSS output as the copy source inside BeamNG’s existing command list. BeamNG’s own command-list closure, destination handling, and queue submission remain authoritative. Any mismatch leaves the original engine copy unchanged.

The deployed configuration is now:

```ini
[bridge]
helper=1
replaceOutput=0
deferredOutput=1
```

The project compiled successfully with Visual Studio Community 2026. The updated ASI, helper, and INI were deployed to `C:\games\BeamNG.drive\Bin64\plugins` after stopping the stale helper process, and the source/deployed hashes match. The next user launch is the controlled test for visible DLSS output through an engine-owned command list.

---

## 2026-08-27 Black Output — Helper Evaluation Status Protocol Corrected

The deferred-output test did not crash, but the game window was black. Log comparison showed the game side reporting `eval=ok` while the helper reported `ok=0 skip=...` for every frame. The previous acknowledgement format carried only the frame number, so it confirmed completion signaling but did not confirm that NGX recorded a valid evaluation.

The protocol now sends the frame number together with a `recorded` status flag. The game side accepts a frame for visible output only when the frame number matches and the helper reports a successful NGX recording. A helper acknowledgement with `recorded=0` is retained for pacing and diagnostics but cannot be copied into the game backbuffer.

The corrected build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins` with `replaceOutput=0` and `deferredOutput=1`. BeamNG and the stale helper were stopped to complete the ASI replacement, and source/deployed hashes match. The next launch should either remain on the native game image when NGX rejects the frame or log a deferred copy only after a genuine NGX recording.

---

## 2026-08-27 Stable Later Launches and Hang Classification

The user reported no crashes or rendering artifacts on the later launches. The first launch required Task Manager termination because the window hung during close; no new CrashDumps report was created, so it is classified as a shutdown hang rather than a plugin crash.

The corrected runtime logs show the helper receiving frames but reporting `ok=0 skip=...` consistently. ScaleNG now correctly recognizes those acknowledgements as rejected evaluations and does not mark invalid output as pending. No deferred output-copy event occurred.

This verifies safe native presentation with helper diagnostics, but not visible DLSS output. The remaining functional task is to correct the helper’s color/depth/motion-vector inputs until NGX reports a genuine recorded evaluation.

---

## 2026-08-27 Rejected-Frame Boolean Corrected

The latest log explicitly reported that NGX rejected the frame, but the game-side code still treated the matching acknowledgement as successful because it set `helperAcked=true` before checking the new `recorded` status field. This allowed the invalid-output path to remain pending and caused the black window.

The assignment now requires both a matching frame number and `recorded != 0`. Rejected frames cannot enter the deferred output handoff. The corrected ASI, helper, and INI were rebuilt with VC2026 and redeployed after stopping BeamNG/helper processes; source and deployment hashes match.

---

## 2026-08-27 Diagnostic Result and Resource Instrumentation Deployed

The next implementation cycle began with diagnostic instrumentation rather than another output-path change. The concrete DLSS wrapper now exposes its last NGX evaluation result, and the helper logs resource descriptors for color, output, depth, and motion-vector textures. Periodic helper telemetry now includes the exact NGX result code together with the `recorded` Boolean.

This build was compiled successfully with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins` after stopping active BeamNG/helper processes. The safe configuration remains active with `replaceOutput=0` and `deferredOutput=1`, so no result can alter the game image during diagnosis. The next runtime log will identify the precise NGX rejection category and the resource metadata needed to correct it.

---

## 2026-08-27 CreateFeature Failure Instrumentation Deployed

The two-launch logs established that NGX initialization succeeds but evaluation returns the internal `-1002` marker, meaning it fails while creating the DLSS feature before `EvaluateFeature` is reached. The helper resources are consistently 1920x992 or 1920x1001, with color/output format 28 (`R8G8B8A8_UNORM`), depth format 41 (`D32_FLOAT`), and motion-vector format 34 (`R16G16_FLOAT`).

I added a concrete `LastCreateResult()` diagnostic to the DLSS wrapper and helper telemetry. The next run will report the actual CreateFeature result code, allowing the implementation to target the real NGX rejection. Visible output remains disabled while this is diagnosed. The build compiled with VC2026 and the ASI/helper were redeployed with matching hashes.
---

## 2026-08-27 Resize Freeze Fixed and Deployed

The four-launch logs show that the last session stayed usable only because the window was not resized. The resize path reached 1920x1001, but the helper's mid-stream setup handler rebuilt its resources and failed to send the required acknowledgement. ScaleNG then waited forever for that response, producing the observed freeze without a crash dump.

The helper now sends an explicit `OKAY` or `FAIL` response for every resize setup, releases its previous DLSS wrapper before rebuilding size-dependent resources, and the game side has a bounded 5-second acknowledgement wait with helper liveness checks. The shipped INI's stale `appId=1` was also corrected to `appId=241534720`.

The fix was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. The ASI, helper, and INI hashes match their build outputs. NGX feature creation still reports `0xBAD0000B`, so output replacement remains disabled pending a genuine DLSS evaluation.

---

## 2026-08-27 Standalone NGX Validation Restored

The standalone helper test initially reproduced the in-game failure: `0/100` evaluations. NVIDIA's NGX log identified the cause directly—the helper directory did not contain `nvngx_dlss.dll`. The correct validated 310.6.0 snippet was present in BeamNG's `Bin64` directory but had not been packaged beside the helper in `Bin64\\plugins`.

The build script now packages the validated snippet automatically. I copied it to the deployed plugin directory and verified that the build and deployed copies have matching SHA-256 hashes. The standalone helper test now reports `100/100` successful evaluations and exits with code `0`. NVIDIA's telemetry confirms successful snippet loading, feature creation, and DLSS evaluation using CMS ID `241534720`.

This completes the isolated NGX validation milestone. The remaining work is to validate BeamNG's shared color/depth/motion resources in-game and only then enable visible output replacement.

---

## 2026-08-27 Helper Recovery Made Restartable

The stable run had no major defects, but its logs showed that the existing BeamNG process had disabled helper mode after the earlier setup attempt happened before the DLSS snippet was packaged. It then repeatedly tried the unavailable independent-device fallback, so that session did not test the corrected helper.

Helper setup failure handling now terminates the failed worker but keeps helper mode enabled. A later frame can restart a clean worker through the existing retry throttle. The ASI, helper, INI, and validated `nvngx_dlss.dll` were rebuilt with Visual Studio Community 2026 and redeployed with matching hashes.

A fresh BeamNG launch is recommended for the next test. The goal is to see the helper load the packaged snippet in-game, report successful CreateFeature/EvaluateFeature results, and expose the real shared-resource validation result before any output replacement is enabled.

---

## 2026-08-27 Per-Process Helper Pipe Deployed

The latest run was stable, but the log showed `CreateNamedPipe FAILED err=231` and no new helper log. BeamNG has multiple plugin/renderer processes, while the bridge used one global pipe name, allowing one process to block another from creating its endpoint.

The bridge now uses a pipe name containing the parent BeamNG process ID on both the ASI and helper sides. Normal startup no longer terminates every helper belonging to another process. The ASI, helper, INI, and validated DLSS snippet were rebuilt with Visual Studio Community 2026 and redeployed with matching hashes.

The next fresh launch should provide a reliable in-game helper connection and finally test BeamNG's real shared-resource inputs against the already-proven standalone NGX path.

---

## 2026-08-27 Periodic Freeze Traced to Stale Fence Handles

The latest run had no crash or artifacts, but the game paused at regular intervals. The helper log showed repeated successful connections followed by `OpenSharedHandle FAILED ... ERROR_INVALID_HANDLE` for the input fence. Each failure caused another helper restart, creating the repeated stalls.

The cause was a cached numeric fence handle being reused after the helper process changed. Fence handles are process-local and must be duplicated again for every new helper. The ASI now clears the cached duplicated values whenever the helper or pipe is replaced, forcing fresh valid handles on the next setup.

The fix was compiled with Visual Studio Community 2026 and deployed with the helper, INI, and validated DLSS snippet. Build and deployment hashes match. The next test should establish one helper successfully and proceed to actual in-game NGX evaluation.
---

## 2026-08-27 Persistent Source Fence Handles Corrected

The periodic helper failures had a second lifetime problem: the ASI closed the source shared-fence handles during every setup retry even though the underlying fences were meant to persist through resizes. New helper processes consequently received duplicates of closed handles.

The source shared-fence handles now remain open for the lifetime of the persistent fences. Only per-helper duplicate values are cleared when a worker is replaced. The fix was rebuilt with Visual Studio Community 2026 and deployed with matching ASI, helper, INI, and DLSS snippet hashes.

---

## 2026-08-27 Helper GPU Allocator Synchronization

The helper process from the crash session remained alive after BeamNG exited and had grown to approximately 2.7 GB of committed memory. The helper was terminated. Investigation found that it reused its single D3D12 command allocator and command list immediately after submitting work, without waiting for the GPU to finish. This violates D3D12 allocator lifetime requirements and could explain the delayed heap/resource failure.

The helper now waits for the output fence before reusing the allocator/list and before releasing shared resources during a resize or setup replacement. Output-fence signal failures are logged and terminate the frame loop safely, and the submitted-value epoch is reset when a new fence is installed.

The fix was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. A backup was saved as `ScaleNG-backup-20260827-030853`. The next run remains evaluation-only (`replaceOutput=0`) with deferred handoff enabled.

---

## 2026-08-27 Long-Run Heap-Corruption Investigation and Reference-Leak Fix

---

## 2026-08-27 Freeze Source Identified as Incompatible Fallback Retries

---

## 2026-08-27 Crash Run Traced to Persistent Pipe Collision and Stale Processes

---

## 2026-08-27 No-Window Launches Traced to Startup Swapchain Handling

---

## 2026-08-27 Startup Swapchain Stabilization Gate Implemented

---

## 2026-08-27 Interleaved Swapchain Stabilization Corrected

---

## 2026-08-27 Rollback Copies Removed from Active Plugin Discovery Path

---

## 2026-08-27 First Sustained In-Game NGX Evaluation Confirmed

---

## 2026-08-27 Bounded NGX Session Recycle Added

---

## 2026-08-27 Full Helper Batch Restart Added for Memory Reclamation

The 1200-frame NGX wrapper recycle did not reduce memory. Helper commit continued rising from roughly 437 MB at 254 frames to approximately 2 GB after more than 7500 frames, despite repeated session-recycle markers. The driver/snippet retains allocations beyond feature destruction.

The helper now exits after every 900 completed frames, after sending the final acknowledgement. The ASI detects the closed/broken pipe, clears the dead worker, marks the bridge unready, and reconnects a fresh helper on the next frame. This bounds each helper lifetime so Windows can reclaim all NGX/driver allocations and prevents a dead worker from causing permanent skips.

The fix was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`; rollback backup `ScaleNG-backup-20260827-043146` is stored outside the active plugin tree.

The sustained in-game session completed 7,587/7,587 NGX evaluations, but helper commit grew to approximately 2034 MB. The allocator fence synchronization was functioning, so the remaining growth is associated with long-lived NGX/snippet evaluation state.

The helper now safely recycles only the NGX wrapper and evaluation heap every 1200 completed frames, after waiting for the previous output fence. Shared textures, fences, queue, pipe, and protocol remain alive. The change was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`; rollback backup `ScaleNG-backup-20260827-042257` is stored outside the active plugin tree.

After removing rollback executables from the active plugin tree, the runtime reached a stable 1920x992 swapchain, connected the helper, and completed genuine in-game DLSS evaluations. Helper telemetry reached `7587 frames ... ok=7587 skip=0`; ScaleNG continued through frame 7801 without a new crash artifact after 04:14.

The session also exposed a remaining helper lifetime problem: helper committed memory reached approximately 2034 MB after 7587 evaluations, and the helper remained alive after BeamNG was terminated. The orphan helper was stopped manually.

This confirms that startup/window selection and the NGX evaluation path are now functioning. The next blocker is bounding helper/NGX memory and making shutdown deterministic before visible output replacement is enabled.

The latest launch still showed no window and never reached the stable-swapchain threshold. The active plugin directory contained 13 rollback directories with executable ASI/helper copies, and a previous inspection confirmed that a helper had launched from one of those backup directories.

All rollback directories were moved, without modification, to `C:\games\BeamNG.drive\Bin64\ScaleNG-backups`. The active `Bin64\plugins` directory now contains only the intended current runtime files and BeamNG's existing utility files. No rebuild was needed for this cleanup.

The first startup gate required eight consecutive presents from one swapchain. BeamNG alternates between several startup swapchains, so no candidate reached the threshold and the pipeline never reached DLSS.

The gate now keeps independent present counts for up to 16 swapchain candidates. A candidate becomes eligible after eight presents of its own, even when BeamNG interleaves other swapchains. The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`, with backup `ScaleNG-backup-20260827-041247` and matching deployment hashes.

Added a shared Present-time candidate tracker. A swapchain must present eight consecutive times before ScaleNG adopts it and enters the self-contained DLSS pipeline. Transient startup, UI, and probe swapchains are forwarded untouched without backbuffer or device probing.

The gate is active in the scanned Present stubs, normal Present, and Present1 paths. The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`, with backup `ScaleNG-backup-20260827-040955` and matching deployment hashes.

Two launches produced no visible window and had to be terminated. The resulting BeamNG artifacts were access violations at `BeamNG.drive.x64.exe+0xD2A747`, not heap-corruption reports.

Neither launch reached the helper or NGX. ScaleNG processed many swapchains during startup, and the final trace stopped after `ngx-pipe: bb=...` before the pipeline reported the backbuffer dimensions. This points to the self-contained Present path entering transient startup swapchains before the actual display swapchain is stable.

No DLSS bridge change was deployed from this run. The next implementation change will gate the pipeline until a stable display swapchain is selected, so startup surfaces cannot block window creation.

The latest run produced a fresh BeamNG crash artifact at approximately 03:26. It was an access violation at `BeamNG.drive.x64.exe+0xD746D0`, not a new heap-corruption report. ScaleNG repeatedly logged `CreateNamedPipe FAILED err=231` from 03:23 until the crash, meaning the helper never connected and DLSS evaluation was not reached.

Process inspection found BeamNG renderer processes and an orphan helper still alive after the crash. The orphan was running from an older `ScaleNG-backup-*` directory inside the active `plugins` tree, which could create competing plugin/helper endpoints. The leftover BeamNG and ScaleNG processes were stopped before deployment.

The helper pipe now uses a per-launch tick-count nonce and the exact generated endpoint is passed to the helper; the parent PID is passed separately for liveness detection. This removes the PID-only endpoint collision. The change was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`, with backup `ScaleNG-backup-20260827-032919` and matching deployment hashes.

The latest run did not crash, but it did not test the corrected helper. The log shows that helper startup failed immediately with `CreateNamedPipe FAILED err=231` (`ERROR_PIPE_BUSY`). The ASI then disabled helper mode and attempted the known-incompatible in-process BeamNG device every three seconds, which explains the recurring freezes.

Helper mode is now sticky when enabled in the INI: pipe, process, connection, and handshake failures no longer select the in-process fallback. The bridge will remain on the helper-only path and retry safely. The fix was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`; backup `ScaleNG-backup-20260827-032031` was created and deployment hashes match.

The user reported BeamNG 0.39.3.0 `0xC0000374 STATUS_HEAP_CORRUPTION` after a long run. No matching fresh BeamNG crash report or Windows WER record was present, so the faulting module could not be attributed from the available crash artifacts. The logs still show that the helper path had previously completed genuine in-game NGX evaluations, while another process later fell back after helper setup failure.

Review found a concrete long-run leak in the Present bridge: the backbuffer reference acquired with `GetBuffer` was not released when evaluation succeeded in evaluation-only/deferred mode, when the helper fence wait failed, or when the guarded descriptor read raised an exception. The missing releases were added to all paths, including the final output-replacement path.

The corrected build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. `replaceOutput=0` and `deferredOutput=1` remain enabled so this test continues to exercise NGX while avoiding the separate output-copy risk. A deployment backup was saved as `ScaleNG-backup-20260827-030718`.

## 2026-08-27 Removed the in-frame helper restart

The 900-frame helper restart did reclaim memory, but it also caused a momentary freeze because the render path synchronously closed the old pipe and waited for the replacement helper handshake. The periodic restart was removed from the helper so normal frame submission is no longer intentionally interrupted. Unexpected pipe failures are still handled safely, and memory telemetry remains active for the next run.

The change was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-043647`. Output replacement remains disabled pending a stable memory/lifetime test.

## 2026-08-27 Fixed the helper acknowledgement reconnect loop

The latest runtime log showed that the helper was being killed and recreated repeatedly, despite the deliberate periodic restart having already been removed. The ASI waited synchronously for an acknowledgement for 250 ms, while the helper's first NGX evaluation could take longer during driver initialization. The timeout was incorrectly treated as a dead pipe, causing repeated reconnects and momentary freezes.

Frame acknowledgement polling is now non-blocking. Delayed acknowledgements cause a frame to be skipped and are collected on later frames; only a genuine pipe write/query failure tears down the helper. The helper's parent-PID lookup was also corrected to use the second command-line argument, since the first argument is the named-pipe endpoint.

The change was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-044144`. Output replacement remains disabled pending stable runtime validation.

## 2026-08-27 Resize pipe backlog fix deployed

The latest two launches were stable during normal rendering but froze during window resizing. Logs showed that the helper was receiving thousands of queued frame messages while the ASI was skipping their acknowledgements. Resize setup was then placed behind that backlog and waited synchronously for its response.

Added one-frame backpressure so the ASI cannot send another frame until the previous acknowledgement is drained. This prevents unbounded pipe growth and limits resize setup to one outstanding helper evaluation.

The earlier deployment was blocked by the orphan helper process holding the binaries open. After the session was closed, the leftover helper was stopped and the complete build was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-045043`. Deployment hashes match the build output.

## 2026-08-27 Helper shutdown fix deployed

The stable run confirmed that resizing no longer caused issues. The helper correctly detected BeamNG termination, but after logging `parent exited - bye` it returned to its outer setup loop and stayed alive. Added an explicit parent-exited state so the helper now closes its parent handle, leaves the setup loop, and terminates from `main`.

Telemetry from the same run reached approximately 3.2 GB at 12,906 successful evaluations, so memory growth remains a separate issue. The periodic restart was not reintroduced because it caused visible freezes. The corrected build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-045941`. Deployment hashes match.

## 2026-08-27 Stable runtime and clean shutdown validated

The user reported a flawless run with successful resizing and no major issues. Log inspection found no new crash artifact, no active BeamNG/helper process after exit, and the expected clean shutdown sequence: `parent exited - bye`, `parent shutdown complete - exit`, and `pipe closed - exit`.

The latest post-fix helper sequence was approximately 428 MB after setup and then shut down cleanly. The historical 3.2 GB session remains in the cumulative log, so memory growth is improved in the latest sequence but requires a longer isolated measurement before it can be considered fully resolved.

## 2026-08-27 Hardened resize acknowledgement framing

The latest output-integration validation was visually stable, but the logs showed a resize setup response of `0x000005B9`. This value matched the low 32 bits of a queued frame acknowledgement, confirming that a stale frame ACK could still precede the setup response on the byte-oriented pipe. An earlier launch also produced a BeamNG access-violation artifact at `BeamNG.drive.x64.exe+0xD58131`.

Setup synchronization now drains complete queued ACKs, waits for partial ACK bytes to complete, and waits for the single known pending ACK before writing resize setup. This keeps setup responses aligned with their message and avoids reintroducing the resize backlog. The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-052040`. Direct output replacement remains disabled pending clean validation.

## 2026-08-27 Started visible-output integration

The first output-integration change makes frame acknowledgement state shared between the normal frame path and resize/setup path. The previous cumulative log showed the helper evaluating frames while the ASI remained stuck on one pending acknowledgement value. Because the pipe is byte-oriented, a resize setup response could become misaligned with a pending frame acknowledgement.

`B2SendSetup` now drains the single outstanding frame acknowledgement before sending resize setup, while one-frame backpressure prevents any new backlog. The change was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-051333`. Output replacement remains disabled for this validation run.

## 2026-08-27 Corrected one-frame-late helper ACK handoff

The helper was successfully evaluating frames, but the ASI compared each returned acknowledgement with the current frame value. Because the helper ACK normally arrives during the next Present, valid previous-frame acknowledgements were discarded and deferred output was never armed.

Recorded ACKs now arm deferred output using their own completed fence value. Direct backbuffer replacement remains disabled. The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-052818`. Deployment hashes match.

## 2026-08-27 Instrumented deferred-copy near misses

The latest run confirmed that deferred output is being armed, but no engine-side copy replacement was recorded. Added throttled diagnostics at the deferred-copy matcher to report destination/source types, subresources, coordinates, resource identities, and fence completion whenever BeamNG targets the current Present backbuffer but fails one of the safety checks. Native rendering remains unchanged.

The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-053541`. Deployment hashes match.

## 2026-08-27 ACK/evaluation stable; copy candidate still unmatched

The latest run remained stable. The ASI repeatedly accepted helper acknowledgements and armed deferred output, and the helper completed approximately 8,133 successful evaluations before clean shutdown. No helper process remained afterward.

No deferred-copy or near-miss diagnostic was recorded. The current matcher therefore is not seeing BeamNG's relevant full-resolution copy with the exact backbuffer pointer captured at Present. The next implementation step is to correlate full-resolution engine copy candidates with the active swapchain instead of requiring direct pointer identity. Helper memory still grew to approximately 2.15 GB and remains a separate resource-lifetime issue.

## 2026-08-27 Guarded full-frame candidate handoff deployed

The exact backbuffer pointer was not observed by the deferred-copy hook, so the previous output handoff could not become visible. Added a guarded candidate path that can substitute the source of a zero-offset, full-frame 2D texture copy when source/destination dimensions and formats match the active display target, the helper fence is complete, and neither resource belongs to the bridge.

BeamNG continues to own the command list and submission; all nonmatching copies remain native. The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-054440`. Deployment hashes match.

## 2026-08-27 Broad deferred-copy correlation diagnostics deployed

The helper NGX evaluation path is confirmed working (recent sessions show `recorded=1`, `ngxResult=1` for thousands of frames). The game-side deferred output handoff is repeatedly armed (`helper acknowledged v=...; deferred output armed`), but no `deferred DLSS output copied in engine list` or `deferred DLSS candidate copied` message has ever appeared.

Added comprehensive diagnostics in `CopyTexBody` that log EVERY full-frame copy when deferred output is pending, recording: source/destination resource pointers, dimensions, formats, pending/completed fence values, and the shared output resource descriptor. This will identify which BeamNG copy operation is the true presentation handoff and why both the exact-match and guarded-candidate matchers fail.

The existing near-miss logging only fired when `dst->pResource == g_b2PresentBb`; the new broad diagnostics fire on any qualifying full-frame copy regardless of destination identity.

The build was compiled with Visual Studio Community 2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ScaleNG-backup-20260827-062319`. Deployment hashes match.

Next launch will produce the broad diagnostic logs. Expected outcomes: (1) identify the exact engine copy that should consume the DLSS output, (2) determine whether the destination resource is the same backbuffer captured at Present or a different resource in the presentation chain, (3) confirm format/dimension/fence alignment, then narrow the matcher to that specific copy.

## 2026-08-27 Recovered mixed state and removed live NGX recycling

Before changing the project, preserved the current source, dist, documentation, and user records in `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\pre-recovery-20260827-153703`. The latest helper log showed successful NGX evaluation through frame 18,000, then periodic live-session recycling was followed by permanent frame rejection and approximately 4.36 GB commit.

Removed periodic NGX wrapper recycling during an active session. Resize handling still performs a fence-guarded teardown through the existing setup path. Added bounded diagnostics that record NGX evaluation and feature-creation result codes when a frame is rejected.

Compiled successfully with Visual Studio Community 2026 and deployed the resulting ASI, helper, INI, and DLSS snippet to `C:\games\BeamNG.drive\Bin64\plugins`. Deployment backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\deploy-pre-20260827-153901`.

The active configuration remains `replaceOutput=0` and `deferredOutput=1`. The next game run is needed to verify sustained evaluation before enabling visible output.

## 2026-08-27 Sustained NGX evaluation restored

The first run after removing live-session NGX recycling completed without a new BeamNG crash artifact. The helper continuously returned `recorded=1` and `ngxResult=1`, reaching approximately 17,024 successful evaluations with zero skips before BeamNG exited. Deferred output was repeatedly armed on the game side.

The memory leak remains severe: helper commit grew from approximately 428 MB at startup to approximately 4.1 GB by shutdown. This demonstrates that periodic feature recycling was only masking the underlying resource-lifetime problem and is not a valid fix.

No visible DLSS output is claimed yet because `replaceOutput=0` remains active and no deferred-copy substitution was observed. The next implementation focus is the NGX parameter/resource lifecycle, while preserving one long-lived feature for the duration of a session.

## 2026-08-27 Direct visible-output integration test enabled

The sustained run confirmed continuous helper-side DLSS evaluation, but no `CopyTexBody`, deferred-copy, or candidate-copy messages were produced. BeamNG's active renderer path is not exposing the engine copy hook needed for deferred handoff.

Enabled `replaceOutput=1` in the deployed INI while retaining `deferredOutput=1`. This activates the existing guarded Present-time replacement path, which waits for the helper fence and copies the helper output into the game's backbuffer using resource barriers.

The previous configuration was preserved at `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\output-test-pre-20260827-155248`. No source or binary change was made for this test. The next run is intended to determine whether actual DLSS pixels reach the visible BeamNG window.

## 2026-08-27 Direct output path rejected by crash evidence

With `replaceOutput=1`, BeamNG showed a black window and crashed immediately after the first acknowledged helper frame. The newest crash artifact reports a null access violation at `BeamNG.drive.x64.exe + 0xF0AAF8`; ScaleNG recorded `frame 1 eval=ok` immediately before the failure.

The direct Present-time copy path is unsafe for this renderer. Reverted the deployed INI to `replaceOutput=0` and `deferredOutput=1`. The failed configuration was preserved at `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\output-test-pre-20260827-155248`, and the revert state was backed up at `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\crash-revert-20260827-155625`.

The next integration step must use the renderer's actual presentation command path or a correctly synchronized swapchain-owned target rather than direct Present-time backbuffer replacement.

## 2026-08-27 Real BeamNG queue discovery control build deployed

The existing ExecuteCommandLists hook was disabled, and the earlier code only created a temporary ScaleNG queue. Added a cold-path device `CreateCommandQueue` hook so the first real BeamNG direct graphics queue can be captured and its submission function observed.

This build is observation-only: the real queue hook logs BeamNG submissions but does not inject or modify GPU work. The temporary queue discovery path was removed to prevent ScaleNG from confusing its own queue with BeamNG's queue.

Compiled successfully with VC2026 and deployed the ASI, helper, INI, and DLSS snippet. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\queue-control-pre-20260827-160234`. The deployed INI is stable evaluation-only mode with `replaceOutput=0` and `deferredOutput=1`.

## 2026-08-27 Real BeamNG queue confirmed

The observation-only control run completed without artifacts, crashes, or a new crash artifact. ScaleNG captured BeamNG's actual direct graphics queue and installed the real ExecuteCommandLists hook. Sustained game submissions were observed across the active frame stream, with submissions containing 1, 3, 4, and 8 command lists.

Helper-side DLSS evaluation remained healthy (`recorded=1`, `ngxResult=1`) and deferred output continued to arm. The queue-discovery milestone is complete. The next implementation can move output-copy submission to the confirmed game queue after BeamNG's native command lists and before Present.

## 2026-08-27 ECL becomes the pipeline driver

The real queue control run proved sustained BeamNG ExecuteCommandLists activity. Changed the real ECL hook from observation-only to the pipeline driver and disabled duplicate Present-time driving whenever the real queue is observed.

The deployed configuration remains `replaceOutput=0`, so this build changes execution ordering only and does not copy DLSS output into the backbuffer. Compiled successfully with VC2026 and deployed the ASI, helper, INI, and DLSS snippet. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\ecl-driver-pre-20260827-162326`.

The next run must confirm stable ECL-driven evaluation before the queue-submitted visible copy is enabled.

## 2026-08-27 ECL full-pipeline driver rejected

The ECL-driven test crashed during startup while BeamNG was still displaying a black loading window. The logs show that the full `TryDeferredInject()` pipeline entered from early ECL submissions before the first helper acknowledgement.

Retained real BeamNG queue capture and ECL observation, but removed full-pipeline invocation from each ECL submission. Restored Present as the pipeline driver with `replaceOutput=0` and `deferredOutput=1`, matching the last stable evaluation configuration.

Rebuilt successfully with VC2026 and redeployed. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\present-driver-revert-20260827-163037`. The next queue-path change must be a narrowly gated post-submit copy operation, not a full pipeline invocation from ECL.

## 2026-08-27 Presentation correlation logging deployed

Added bounded correlation logs at real BeamNG ExecuteCommandLists submission, Present backbuffer capture, and successful deferred-output readiness. The records include queue identity, command-list count, frame number, backbuffer pointer and dimensions, output readiness, pending fence value, and completed fence value.

No rendering or synchronization behavior changed. The configuration remains `replaceOutput=0` and `deferredOutput=1`. Compiled successfully with VC2026 and deployed. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\correlation-logging-pre-20260827-163745`.

The next run should reveal the ordering and resource identity relationship needed for a narrowly gated queue copy.

## 2026-08-27 Queue correlation run completed

The run completed without a new crash artifact and helper evaluation remained successful. The real ECL hook observed sustained submissions, but BeamNG used several direct queue interface pointers. The current hook overwrites `g_graphicsQueue` on every ECL callback, so its queue field is not a stable identity and must not yet be used for output injection.

Present correlation was stable within the session: the same backbuffer pointer appeared repeatedly at `1920x1001`, and the helper output fence was completed before deferred evaluation-ready logs. ECL records reported `frame=0`, proving the existing frame counter is not synchronized with this renderer's submission stream; the next correlation pass needs an independent monotonic ECL/Present serial.

No queue copy or visible-output change was attempted. Configuration remains `replaceOutput=0`, `deferredOutput=1`.

## 2026-08-27 Queue-to-Present correlation confirmed

The map-load run completed without a crash or new crash artifact. The first captured direct queue remained stable for the session, and ECL serials advanced predictably against Present serials: approximately 600 ECL submissions per 120 Presents in the observed stream.

The same Present backbuffer pointer remained active at `1920x992`, while helper acknowledgements and completed output fences continued normally. The helper reached at least 5,395 successful evaluations with zero skips in the captured session segment.

This confirms a reliable candidate ordering point for a narrowly gated queue copy. No output copy was attempted; configuration remains `replaceOutput=0`, `deferredOutput=1`.

## 2026-08-27 First narrowly gated queue-copy build deployed

Added a dedicated post-submit `TryQueueOutputCopy()` path. It runs only on the retained BeamNG queue, after native command-list submission, when an acknowledged helper frame has a completed output fence.

The path validates 2D resource dimensions and formats, atomically consumes one pending output, waits on the GPU fence, records barriers/copy/restore operations on a dedicated command list, and submits them to the game queue.

Added `queueCopy=1` to the deployed INI while leaving `replaceOutput=0`; the full DLSS pipeline is not invoked from ECL and direct Present replacement remains disabled. Rebuilt successfully with VC2026 and deployed. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\queue-copy-pre-20260827-165041`.

The next run should be evaluated for `QUEUE COPY submitted`, black-window behavior, crashes, and visible image changes.

## 2026-08-27 Warmed queue copy still causes device removal

The 300-Present warmup gate prevented the startup copy, but the first eligible `QUEUE COPY submitted #1` still caused BeamNG device removal or an immediate access-violation crash. The helper fence was valid in the latest session (`fence=290`), so the failure occurs at or after writing the shared DLSS output into the swapchain backbuffer, not during NGX evaluation or fence readiness.

Disabled `queueCopy` and restored evaluation-only operation. Revert backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\queue-copy-device-removal-20260827-165755`.

The next output design must avoid direct writes to the swapchain resource, even from the correct queue. The next candidate is a native presentation-chain resource discovered from BeamNG's own render graph, with explicit device/resource ownership validation before any copy.

## 2026-08-27 First queue copy reached GPU but crashed

The queue-copy test reached `ngx-b2: QUEUE COPY submitted #1` immediately before BeamNG crashed during the black-window startup phase. The newest crash artifact reports an access violation at `BeamNG.drive.x64.exe + 0xD57BF3`.

One startup also reported `fenceDone=UINT64_MAX`, so invalid fence completion values must be rejected before any queue wait or copy submission. Disabled `queueCopy` and restored evaluation-only operation. Revert backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\queue-copy-revert-20260827-165234`.

The queue hook and helper evaluation remain useful, but the direct swapchain backbuffer is not yet a safe copy target. The next design must use a swapchain-compatible intermediate/present path and validate device and fence state explicitly.

## 2026-08-27 Guarded queue-copy retry deployed

Added a Present warmup gate requiring 300 observed Presents before queue output copying can arm, preventing startup render-graph mutation. Added explicit rejection for game-device removal and `GetCompletedValue()==UINT64_MAX` before any queue wait or copy submission. Shape mismatch diagnostics now include the Present serial.

Enabled `queueCopy=1` with `replaceOutput=0`; the queue copy is delayed and self-disables on invalid device or fence state. Rebuilt successfully with VC2026 and deployed. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\queue-copy-guarded-pre-20260827-165535`.

## 2026-08-27 Serial correlation instrumentation deployed

Stopped the ECL hook from overwriting the first real graphics queue with every direct-queue callback. Added independent monotonic ECL and Present serials, including Present1 observations, so queue submissions can be correlated without relying on the inactive frame counter.

The pipeline remains observation-only and the configuration remains `replaceOutput=0`, `deferredOutput=1`. Compiled successfully with VC2026 and deployed. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\serial-correlation-pre-20260827-164356`.

## 2026-08-27 Topology logging build deployed

Implemented a compact evidence pass for the next DLSS output design. Present now emits throttled `topo-state` records containing the queue, ECL/Present serials, cached backbuffer, tracked scene resources, bound RTV, last full-resolution copy source, and guarded D3D12 descriptors. Stale resource descriptor faults are logged safely.

The deployed configuration remains observation-only (`replaceOutput=0`, `queueCopy=0`, `deferredOutput=1`) because the prior direct swapchain write caused device removal/crashes. Built successfully with VC2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`.

Backup created before deployment: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\topology-logging-pre-20260827-170148`.

Next turn plan: one clean 60–120 second run; inspect only high-signal topology/evaluation/crash lines; identify a stable non-swapchain native presentation resource; add a dry-run handoff gate; then perform one reversible visible-output experiment.

## 2026-08-27 Reduced-resolution evidence run and bridge logging update

The longer non-maximized run completed without a reported crash or resize issue. Present/ECL correlation and helper evaluation remained healthy, with approximately 7,000 Presents and repeated completed DLSS evaluations.

The topology fields for the cached backbuffer, scene targets, bound RTV, and last copy source remained null, proving that the current resource-discovery observation path is not seeing BeamNG's render graph in this mode. The snapshot was extended to include the actual bridge Present backbuffer/output pointers, bridge readiness, and deferred state. Rebuilt with VC2026 and redeployed in safe observation mode (`replaceOutput=0`, `deferredOutput=1`, `queueCopy=0`).

Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\topology-b2-logging-pre-20260827-230444`.

## 2026-08-27 Bridge topology confirmed

The latest approximately two-minute run completed cleanly. The bridge remained ready, the same `1920x983` format-28 Present backbuffer and bridge output remained stable, and helper evaluations repeatedly completed with valid fences.

The native BeamNG discovery fields remained null throughout because command-list observation is currently disabled. This isolates the remaining blocker to render-graph visibility; NGX initialization, cross-process evaluation, queue correlation, and bridge synchronization are functioning. No output mutation was attempted, and the safe configuration remains `replaceOutput=0`, `deferredOutput=1`, `queueCopy=0`.

Next implementation task: add a narrowly scoped observation-only command-list/resource discovery mode, then identify a stable non-swapchain presentation-chain resource before attempting any visible-output copy.

## 2026-08-27 Evidence consolidated and integration plan reset

Consolidated the project history into `docs/DLSS_INTEGRATION_PLAN.md`. The proven working portion is plugin loading, real BeamNG device/queue capture, Present observation, separate-helper NGX initialization, thousands of successful DLSS evaluations, valid fences, and stable observation-only runs.

The genuine blocker is output insertion into BeamNG's native presentation path. Direct Present replacement and guarded swapchain-backbuffer copying are rejected by crash/device-removal evidence. Native scene/RTV/copy resources remain invisible because command-list observation is disabled or not attached to BeamNG's real command lists.

The next build is Phase 1 only: restore read-only native resource observation. No `replaceOutput` or `queueCopy` mutation is allowed until a stable non-swapchain presentation-chain resource is proven.

## 2026-08-27 Phase 1 native observation build deployed

Found that native resource hooks were explicitly disabled and command-list installation was a logging-only stub. Added a cold `ID3D12Device::CreateCommandList` observer and re-enabled only cold device-level RTV/SRV creation observation. Hot command-list methods remain untouched to avoid the earlier artifact/crash class.

Built successfully with VC2026 and deployed. Safe configuration verified: `replaceOutput=0`, `deferredOutput=1`, `queueCopy=0`.

Rollback backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\phase1-native-observe-pre-20260827-230444`.

Next run criterion: native command-list and/or RTV/SRV candidate records must appear without a stability regression. This is still not a visible-output test.

## 2026-08-27 Phase 1 native resource discovery succeeded

The run produced real BeamNG `CreateCommandList` records and genuine native RTV/SRV candidates. Full-resolution scene-color, motion-vector, and depth resources are now observable through cold device hooks. BeamNG rotates many resources, so pointer identity alone is not safe.

The bridge and helper remained healthy with repeated successful evaluations and completed fences. The cached backbuffer later generated guarded descriptor faults, proving that creation-observed pointers can become stale. Native `bound` and `lastCopySrc` remain unavailable because hot command-list methods are still not intercepted, so the resource feeding Present has not yet been proven.

The late user-reported crash is treated as a deferred memory issue as requested; no new crash artifact was found during the check. No output mutation was attempted, and the safe configuration remains `replaceOutput=0`, `deferredOutput=1`, `queueCopy=0`.

Phase 1 is complete. Next: correlate native candidates to Present using read-only observation, then establish lifetime and ordering before attempting a visible-output experiment.

## 2026-08-27 Bounded native candidate registry deployed

Added a bounded read-only registry for full-resolution native D3D12 resources. It records pointer, descriptor, RTV/SRV role, creation counts, and ECL/Present serial context, then emits throttled summaries. It owns no COM references and does not modify GPU work.

Built with VC2026 and deployed with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`.

Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\candidate-registry-pre-20260827-232926`.

Next run criterion: use the registry summaries to separate persistent presentation-chain candidates from transient resource churn. This is not a visible-output test.

## 2026-08-27 Long recency-registry run analyzed

The registry recency fix worked. Recent summaries now show resources created near the active Present stream, and the table evicts older entries when full. BeamNG continuously creates many full-resolution `1920x983` RTV/SRV resources, mainly formats 10 and 11, confirming renderer churn rather than one obvious output target.

Because hot command-list methods remain unhooked, the logs still cannot tie a candidate to the actual Present operation. Candidate creation order alone is not sufficient for output integration.

The helper reached `17,624` successful evaluations and `6,233` skips at approximately `4249MB` commit before a late null access violation at `BeamNG.drive.x64.exe+0xD57EB7`. This is recorded as deferred memory/stability work per priority and does not change the integration gate.

No output mutation was attempted. The next build must observe native command-list resource usage or an equivalent presentation relationship before any candidate is used.

## 2026-08-27 Candidate registry recency fix deployed

The long run produced substantial native evidence, reaching the 96-entry registry limit and recording many `1920x983` RTV/SRV resources in formats 10, 11, and 34. Helper evaluation and ECL/Present traffic continued successfully for at least 12,000 evaluations before termination.

The registry summary was biased toward its first startup entries, and later candidates were dropped after the table filled. Changed it to evict the oldest creation-context entry and summarize the eight most recently created candidates. The registry remains read-only and owns no COM references.

## 2026-08-28 Per-command-list read-only observation build

Implemented and deployed a new observation-only build. It wraps each submitted D3D12 graphics command list with a private cloned vtable and observes `CopyTextureRegion`, `OMSetRenderTargets`, and `ResourceBarrier` while forwarding every call unchanged. Logs are throttled and limited to qualifying large native resources, with Present serials included for correlation.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\command-list-observe-pre-20260828-000406`.

No output replacement, queue copy, resource transition, or COM lifetime retention was enabled. The next run should last 60–120 seconds without resizing and should be evaluated for stability plus `native-usage:` evidence. Visible DLSS output is intentionally not expected from this build; the purpose is to identify the native presentation-chain resource safely.

## 2026-08-28 Command-list attachment timing corrected

The first command-list observation run was stable and installed two shims, but produced no native usage events. Since the lists were being wrapped at `ExecuteCommandLists`, recording had already finished. Moved shim installation into `Hook_CreateCommandList` so each list is wrapped before BeamNG records work.

Rebuilt successfully with VC2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\command-list-create-observe-pre-20260828-001220`.

The build remains observation-only with `replaceOutput=0`, `queueCopy=0`, and no GPU work changes. The next run should be 60–120 seconds without resizing and should be judged by stable `native-usage:` records correlated with Present.

## 2026-08-28 Per-object command-list coverage corrected

The run was stable and confirmed creation-time shim installation, but only two objects were wrapped because the registry incorrectly treated the shared BeamNG vtable as the identity. Corrected it to track each command-list object independently and expanded the bounded table to 64 active entries.

Rebuilt successfully with VC2026 and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\command-list-per-object-pre-20260828-002158`.

The build remains read-only: all original methods are forwarded, no GPU work is added, and `replaceOutput=0` / `queueCopy=0` remain in force. The next run should be evaluated for `native-usage:` records correlated to Present.

## 2026-08-28 Per-object coverage validation analyzed

The first launch generated a D3D12Core access violation at `D3D12Core.dll+0x1922C5`; the user identified it as an accidental crash. The second launch ran successfully for several minutes with no device-removal or fatal record.

The successful launch installed many distinct command-list shims and produced real native copy events. Evidence included a `1920x954` format-28 copy during startup and two-way `1920x1001` format-10 copies near Present serial 2085. This confirms the observer is now seeing recorded native work, but it does not yet prove that any observed resource is the presentation output. No OM or barrier events were captured.

The helper and deferred bridge remained healthy, and output mutation stayed disabled. The project has moved into dry-run ordering/correlation; the next implementation should improve native event correlation and lifetime logging before a visible-output experiment.

## 2026-08-28 Native copy correlation build deployed

Added richer metadata to native copy observations: command-list type, per-list copy ordinal, creation-time Present/ECL serials, current Present/ECL serials, and Present distance from list creation. This will separate startup/upload copies from copies near the presentation chain without modifying GPU work.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\copy-correlation-pre-20260828-003122`.

Safe configuration remains `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should be 60–120 seconds without resizing and should produce repeated, correlated native copy records before any visible-output experiment is considered.

## 2026-08-28 Native copy correlation run analyzed

The successful run was stable for approximately 6,000 Present cycles and 32,000 ECL submissions. It installed 38 distinct command-list shims and produced no device-removal, fatal, or exception record.

The new copy metadata worked and captured two startup copies, including a type-0 `1902x945` format-28 copy and a type-3 upload/initialization copy into a `1911x1911` format-28 resource. No qualifying gameplay copies, OM events, or barrier events appeared afterward.

This indicates that the active gameplay output path is probably using another command-list interface/vtable or draw/descriptor binding rather than CopyTextureRegion. The build remains observation-only and no target is safe to mutate yet.

Next step: add low-rate invocation counters and interface/vtable identity logging for the three wrappers to determine whether the cloned base interface sees the gameplay render path.

## 2026-08-28 Shim invocation diagnostics deployed

Added low-rate invocation counters for the three wrapped command-list methods and command-list creation interface identity fields. Sparse records include method, command-list type, original/cloned vtables, and Present/ECL serials. This will tell us whether the gameplay renderer bypasses the cloned base interface or whether our resource filter is simply too narrow.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\shim-invocation-observe-pre-20260828-003928`.

The build remains observation-only with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should be evaluated for `native-usage: invocation` records before any visible-output experiment.

## 2026-08-28 Invocation diagnostics run analyzed

The latest process remained stable through approximately 9,600 Present cycles and 49,000 ECL submissions with no device-removal, fatal, or exception record.

The counters proved the cloned interface is active in gameplay: `CopyTextureRegion` and `OMSetRenderTargets` were invoked. Classic `ResourceBarrier` was not, suggesting enhanced barriers or another interface path. Eight qualifying native copies were captured, including a repeated two-way `1920x985` format-10 chain near Present serial 2636.

OM calls reached the observer, but their CPU descriptor handles did not resolve through the current RTV map. This is a descriptor-provenance gap, not proof that no render target was bound. No output mutation was attempted.

Next step: log raw OM descriptor identity/provenance and investigate enhanced `ID3D12GraphicsCommandList7::Barrier` coverage before selecting a visible-output target.

## 2026-08-28 Raw OM descriptor observation deployed

Added a bounded trace of the first 120 raw OM render-target handles, including command-list identity, slot/count, CPU handle, map-hit status, and Present/ECL serials. This will identify whether the missing resource mapping is caused by an unobserved descriptor path, another heap, or a descriptor-space mismatch.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\om-raw-observe-pre-20260828-005242`.

The build remains observation-only with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. Enhanced barrier coverage remains a separate follow-up based on the SDK's `ID3D12GraphicsCommandList7::Barrier` interface.

## 2026-08-28 Raw OM handle run analyzed

The run remained stable through approximately 12,900 Present cycles and 64,000 ECL submissions with no device-removal, fatal, or exception record. The raw trace captured 89 OM calls, but the sampled color descriptor handle was always zero and unresolved. These calls are therefore likely depth-only or unbinding operations and are not a usable output target.

Render-sized native RTV/SRV candidates continued to appear in formats 10, 11, and 34, while the active Present backbuffer remained format 28. No candidate-to-Present relationship was proven, and output mutation stayed disabled.

Next step: isolate non-null color OM bindings and inspect enhanced/versioned command-list coverage before selecting a target.

## 2026-08-28 Non-null color OM sampling build deployed

The raw OM sample budget was previously consumed by zero-handle calls. Changed it to keep a separate budget for non-zero color handles and added `singleRange` plus depth-handle fields so depth-only/unbinding calls can be distinguished from genuine color-target bindings.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\om-color-sample-pre-20260828-010925`.

The build remains observation-only with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should be checked for non-zero `om-raw` color handles.

## 2026-08-28 Command-list vtable slot correction deployed

The timed crash was traced to incorrect D3D12 vtable slot assumptions. `CopyTextureRegion` was correctly at slot 16, but `ResourceBarrier` is slot 26 and `OMSetRenderTargets` is slot 46 in the Windows SDK layout; slots 52 and 22 caused malformed arguments and the D3D12Core access violation.

Corrected the cloned/original wrapper assignments, rebuilt successfully with VC2026, and deployed to `C:\games\BeamNG.drive\Bin64\plugins`. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\vtable-slot-fix-pre-20260828-011635`.

The build remains observation-only with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should first confirm stability, then validate sensible OM and classic barrier records.

## 2026-08-28 Corrected-slot native chain confirmed

The long run validated the vtable correction. It captured non-zero OM handles that resolved to real render-sized color resources. At Present serial 23704, the same direct command-list batch contained coherent format-28 transitions/copies, a mapped `1920x983` format-11 color binding, and format-34/45 intermediate transitions.

This is the first confirmed native presentation-adjacent batch with valid OM, barrier, and copy arguments. The final Present backbuffer is still a separate format-28 resource, so ordering and lifetime must be correlated before choosing a DLSS target.

The process later entered a long NGX rejection storm and crashed at the known BeamNG `+0xF0AAF8` path. Per the user's report, this was helper-related and did not affect gameplay; memory/stability remains deferred. No output mutation was attempted.

Phase 2 has produced native ordering evidence. Next: correlate this coherent batch to helper output and Present over repeated frames, then perform one reversible visible experiment.

## 2026-08-28 Dry-run handoff correlation build deployed

Added a non-owning native correlation snapshot for the latest qualifying copy source/destination, mapped color binding, and barrier resource/state transition. Throttled snapshots are emitted at Present and helper acknowledgement/rejection points with native Present/ECL serials.

The snapshot stores identity and ordering metadata only; it does not retain COM objects, change resource state, or modify output. The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`.

Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\handoff-correlation-pre-20260828-013410`.

Safe configuration remains `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should be checked for repeated `native-correlation:` records before any visible experiment.

Corrected the Present correlation label to report the actual Present serial, rebuilt successfully, and redeployed. Final rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\handoff-correlation-final-pre-20260828-013458`.

## 2026-08-28 Dry-run handoff correlation run analyzed

The run remained playable for well over five minutes and produced repeated Present/helper correlation records. The latest native batch was a coherent format-34 `1920x983` copy/binding sequence, but format 34 is consistent with a motion-vector-style intermediate rather than final color output.

The correlation snapshots also revealed that later Present/helper records repeated older native identities after no new native event refreshed them. This makes the current snapshot stale unless freshness is explicitly checked. Earlier format-11 scene-color batches remain unproven as final Present inputs.

The helper later entered a prolonged NGX rejection storm and the known BeamNG `+0xD746D0` null access violation. Per the user's report, gameplay was not meaningfully affected; helper/memory work remains deferred. No output mutation was attempted.

Next gate: add freshness/serial age checks and require a new native batch near the same Present/helper frame before any visible experiment.

## 2026-08-28 Freshness-gated correlation build deployed

Added a generation counter and Present/ECL age calculation to the native correlation snapshot. Present/helper records now report `fresh`, generation, Present age, and ECL age; the current diagnostic threshold is no more than 120 Present serials and 1200 ECL serials since the latest native observation.

The freshness flag is validation-only and does not enable output work. The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`.

Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\freshness-gate-pre-20260828-015241`.

Safe configuration remains `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should show whether a color candidate remains fresh at helper/Present decision time.

## 2026-08-28 Freshness-gated correlation run reviewed

The latest run was stable for several minutes, reaching approximately Present 24,000 and ECL 118,000 in the captured records, with no fatal/device-removal/exception entry tied to that run.

The new freshness gate worked: a newly observed native batch was briefly `fresh=1`, then became `fresh=0` as its Present/ECL ages increased. This rejects stale cached resources. The strongest batch contained a format-28 copy chain, a mapped `1920x983` format-11 color binding, and format-34/45/10 intermediate work. It still does not prove which resource feeds the final displayed frame.

No visible artifacts were expected because `replaceOutput=0` is still enabled. DLSS helper evaluation and acknowledgement are active, but the result is deliberately not injected into BeamNG's output yet. No new binary was deployed during this review; the existing freshness-gated build remains deployed. Next work is to capture a coherent same-batch record instead of combining the latest event from each category, then validate a color target before any one-shot visible experiment.

## 2026-08-28 Latest stable gameplay run reviewed

Process `p12376` loaded successfully and maintained a usable Present stream through approximately Present 19,800 and ECL 98,400. The ending crash is being treated as the previously observed helper/rejection shutdown behavior, consistent with the user's report.

Native observation and freshness rejection continued to work. Early records included real format-11 color and format-28 render resources, with format-34/45/10 intermediates. Later snapshots were correctly marked `fresh=0` after the native batch aged; for example, current Present 19,808 was 9,191 Presents beyond the last native Present 10,617.

The important negative result is that the helper acknowledged frames but NGX rejected approximately 5,108 of them, with no accepted-evaluation records in the captured lifecycle data. Since the user changed maps and enabled a feature in a modded map, this must be investigated as a possible render-graph/resource-generation or input-contract transition rather than being attributed solely to shutdown. The game ran stably, but this run did not produce a valid DLSS output frame. No artifacts were expected because `replaceOutput=0` remains active, and no binary was changed or deployed during the review.

Next work must reproduce and instrument the map/feature transition, diagnose the sustained NGX rejection state, and require an accepted-evaluation streak plus a matching output fence before any visible experiment.

## 2026-08-28 Map/feature transition diagnostics deployed

Added helper-side setup-generation logging. Each shared-resource setup now records its generation, dimensions, format, handles, starting fence value, and age. Added throttled rejection diagnostics containing the NGX evaluation/create results, generation and age, all helper resource identities, and color/output descriptors.

This build is intended to distinguish a map/mod render-resource reset from stale resources, unsupported input formats, dimension mismatches, or a feature-state problem. It compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`.

Deployment backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\map-feature-diagnostics-deploy-pre-20260828-022627`.

The INI remains observation-only: `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should repeat the map change and modded-map feature activation so the new transition diagnostics can capture the exact failure boundary.

## 2026-08-28 Map/mod run confirms core NGX evaluation

The latest run loaded the diagnostic build, used the modded map and feature, returned to the original map, and kept a usable Present stream through at least Present 7,000 / ECL 34,800.

The helper session for this run accepted approximately 7,175 evaluations with `recorded=1`, `ngxResult=1`, and `skip=0`, then shut down cleanly after detecting the BeamNG parent exit. The older rejection storm is present in the append-only helper log but belongs to an earlier helper session and is not evidence that this latest run rejected frames.

The native candidate identities changed during the map/feature sequence, but the helper remained on its valid `1920x983`, generation-1 setup. This closes the core NGX-evaluation gate for this scenario. The remaining blocker is output visibility/handoff. No artifacts were expected because `replaceOutput=0` remains active, and no binary was changed during this review.

Next implementation phase: select a same-batch color target and prepare one reversible visible output experiment with the existing crash/black-window circuit breakers.

## 2026-08-28 Coherent native-batch target validator deployed

Added a per-Present/ECL batch accumulator. It now requires a qualifying copy, a mapped color RTV binding, and a resource transition in the same narrow batch before emitting `native-batch: coherent=1`. The log includes the candidate resource/format, copy endpoints, barrier resource/states, and shared Present/ECL key.

The swapchain backbuffer is explicitly excluded, and the validator does not modify GPU work or output. The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins`.

Rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\coherent-batch-pre-20260828-023635`.

The INI remains safe with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`. The next run should be judged by repeated coherent-batch records and matching accepted helper/fence timing, not by visible artifacts yet.

## 2026-08-28 Coherent-batch validator run analyzed

The run stayed stable through at least Present 14,400. The helper completed approximately 14,674 accepted evaluations with `ok=14674 skip=0`, no NGX rejection, and a clean parent shutdown.

The validator emitted no coherent-batch records because BeamNG distributes the relevant copy, color-RTV, and barrier events across multiple ECL submissions within one Present interval. Native evidence was still present, including format-28 copy/barrier work and a mapped `1920x983` format-11 color resource. The exact same-ECL grouping was too strict, not proof that native rendering is unavailable.

No output mutation was attempted. Next, widen correlation to a bounded per-Present ECL window while retaining freshness, format, generation, and backbuffer-exclusion checks.

## 2026-08-28 Present-window batch correlation deployed

Widened the validator from exact Present-plus-ECL equality to a Present-window batch. BeamNG's related work can span multiple ECL submissions, so the validator now retains those events until the next Present and reports the ECL range.

All safety gates remain active: plausible color format, swapchain-backbuffer exclusion, freshness/generation checks, and observation-only operation. The build compiled successfully with VC2026 and was deployed.

Deployment rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\present-window-batch-deploy-pre-20260828-031411`.

The next run should be checked for repeated `native-batch: coherent=1` records naming a stable non-backbuffer color candidate. Visible output remains disabled.

## 2026-08-28 Present-window batch run analyzed

The run remained stable through at least Present 14,400. The helper completed approximately 14,674 evaluations with `ok=14674 skip=0`, without NGX rejection or an active-process fatal/device-removal/exception record.

The widened validator emitted eight coherent records, all during startup at Present 1 and ECL range 0–2. They identified a format-11 target, but no coherent records appeared during sustained gameplay even though native copy, RTV, and barrier events continued. The validator is now grouping BeamNG's ECL topology correctly, but it is still finding a startup composition batch rather than a repeatable gameplay handoff.

No output mutation was attempted. Next, reject startup-only batches and require the same candidate to recur across later Present intervals with successful helper/fence timing before enabling a visible experiment.

## 2026-08-28 Gameplay target recurrence validator deployed

Added a startup exclusion for coherent batches before Present 120 and bounded history for non-backbuffer color candidates. A candidate now becomes `repeatable=1` only after it appears in three separate later Present intervals; logs include hit count and Present/ECL range.

The validator remains observation-only with format, freshness, generation, and backbuffer-exclusion checks intact. It compiled successfully with VC2026 and was deployed.

Rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\gameplay-target-recurrence-deploy-pre-20260828-032401`.

The next run should be checked for a repeated `native-batch` candidate with `repeatable=1`. Output mutation remains disabled.

## 2026-08-28 Gameplay recurrence run analyzed

The newest run stayed stable through at least Present 7,200. Its helper session completed approximately 7,066 accepted evaluations with `recorded=1`, `ngxResult=1`, and `skip=0`, then shut down cleanly.

Startup-only batches were successfully excluded, and no later candidate reached `repeatable=1`. Native observation remained active, but the complete color/copy/barrier combination was not captured repeatedly under the current Present-window rule. This is a target-selection limitation, not a DLSS evaluation failure.

No output mutation was attempted. Next, correlate the renderer's bounded frame/ECL sequence instead of requiring every event to attach to one Present serial, while retaining startup exclusion and recurrence checks.

## 2026-08-28 Bounded renderer-frame correlation deployed

Widened the native batch accumulator to a bounded four-Present interval so BeamNG work that straddles a small Present boundary can be grouped as one renderer sequence. Candidate records now report first/last Present and ECL values.

Startup exclusion, recurrence, freshness, format, and swapchain-backbuffer checks remain active. The build compiled successfully with VC2026 and was deployed.

Rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\bounded-frame-batch-deploy-pre-20260828-033059`.

The next run should be checked for a gameplay candidate reaching `repeatable=1`. Output mutation remains disabled.

## 2026-08-28 Bounded-frame run analyzed

The latest run stayed stable through at least Present 5,400, with approximately 5,400 accepted helper evaluations (`recorded=1`, `ngxResult=1`, no skips) and matching deferred-fence activity.

No gameplay candidate reached `repeatable=1`; the startup-only candidate was correctly excluded. Native observation still reports a persistent `1920x983` format-11 scene-color resource plus separate format-10/34/45 intermediates. The remaining problem is associating that persistent scene color with the exact command sequence that feeds the displayed output.

No visible output mutation was attempted. Next, promote the persistent scene-color identity into a read-only handoff candidate using lifetime/recurrence evidence, while separately recording the final presentation sequence. Startup-only and motion-vector candidates remain excluded.

Built with VC2026 and deployed safely. Backup: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\candidate-recentness-pre-20260827-233954`.

## 2026-08-28 Persistent scene-color candidate deployed

Added a low-overhead, read-only lifetime tracker for the display-sized scene-color resource. It samples the tracked format-11/format-10 resources every 60 Presents, rejects the swapchain backbuffer and wrong dimensions, and records recurrence without retaining COM references or modifying rendering.

The build compiled successfully with VC2026 and was deployed to `C:\games\BeamNG.drive\Bin64\plugins` together with the helper. Rollback snapshot: `C:\games\BeamNG.drive\Bin64\ScaleNG-backups\scene-color-candidate-deploy-pre-20260828-034010`.

The new diagnostic is `native-target: scene-color-observed`. `persistent=1` means the same weak resource identity survived at least three sampling points; it is still not proof that the resource is the final display handoff. Output mutation remains disabled with `replaceOutput=0`, `deferredOutput=1`, and `queueCopy=0`.

Next: run BeamNG normally for roughly two minutes and check whether the same scene-color resource remains persistent during gameplay. The result will determine the first tightly guarded visible-output test.

## 2026-08-28 Pre-deployment run evidence reviewed

The latest available process (`p15532`) reached at least Present 5,645 and continued successful helper acknowledgements without an active-process fatal/device-removal record in the captured tail. Its primary format-11 scene resource stayed at `1902x936`, while the display-sized `1920x983` format-11 alternate changed identities repeatedly.

That run was produced by the previous build, before the new tracker was deployed, so it cannot contain `native-target: scene-color-observed`. The next launch is the first valid test of the persistent scene-color diagnostic.

Safe configuration remains `replaceOutput=0`, `deferredOutput=1`, `queueCopy=0`. The next run should focus on the recent candidate summaries and need not be extended until a candidate correlation signal appears.

## 2026-08-28 Vector A aggressive handoff deployed

Added one-shot engine-list handoff via Shim_CopyTextureRegion at src\d3d12_hooks.cpp:1207. Previous deferred candidate required exact mt 28->28 copy; Vector A allows dst 1920x983 fmt 28 backbuffer full-frame where src is any display-sized 1920x983 (including persistent mt 11 scene-color candidates observed at src\d3d12_hooks.cpp:851 and src\d3d12_hooks.cpp:996 
ative-target: scene-color-observed). Conditions: presentSerial>300, completed>=pending, g_b2DeferredPending one-shot, weak identities only, barriers guarded via SafeGetDesc/SafeGetFenceCompleted (src\d3d12_hooks.cpp:995).

Built with VC2026 src\build.bat → dist\ScaleNG.asi 966144 CD05FBB8... / ScaleNG_NGX_helper.exe F7311B3C... / 
vngx_dlss.dll 4E86DAD0.... Deployed to C:\games\BeamNG.drive\Bin64\plugins (hashes match, previous deployment backed up at C:\games\BeamNG.drive\Bin64\ScaleNG-backups\vectorA-pre-20260828-054250). This build performs exactly one COMMON->COPY_SOURCE / COPY_SOURCE->COMMON substitution of g_b2OutG into BeamNG's own CopyTextureRegion when the guarded window fires; otherwise all rendering forwarded unchanged. INI remains eplaceOutput=0 deferredOutput=1 queueCopy=0.

Next test: 60-120s normal gameplay without resize. Success requires both a user-visible image change and a log showing ectorA: DLSS output substituted with the same pending as a ecorded=1 helper evaluation and a persistent=1 
ative-target lifetime.


## 2026-08-28 Vector B OM descriptor hijack deployed

Implemented one-shot observation-first OM hijack through active Shim_OMSetRenderTargets at src\d3d12_hooks.cpp:1235. Added g_vectorBOneShot/Enabled (src\d3d12_hooks.cpp:2578), SafeOverwriteRTV (src\d3d12_hooks.cpp:1007), forward decl TryVectorB (src\d3d12_hooks.cpp:1207), and TryVectorB definition after B2CheckIniFlag (src\d3d12_hooks.cpp:2708). For every non-zero color RTV handle whose g_rtvMap resource is 1920x983 mt 11/10, logs ectorB: candidate with resource/dims/format/Present/ECL/pending/completed. Requires display-sized, samples>=3 persistent via g_sceneColorCandidates (src\d3d12_hooks.cpp:851), es!=g_bbCached, presentSerial>300, completed>=pending, safe SafeGetDesc/SafeGetFenceCompleted reads, one-shot gate, and SafeOverwriteRTV try/except. First qualifying candidate triggers ectorB: attempting then g_device->CreateRenderTargetView(g_b2OutG, nullptr, handle) to overwrite that descriptor to the shared 1920x983 fmt28 DLSS output, logs ectorB: substituted and forwards to original OM with hijacked descriptor; otherwise logs ectorB: skipped.

Built VC2026 → dist\ScaleNG.asi 966656 DC83723B… / helper F0009B86… / 
vngx_dlss.dll 4E86DAD0…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified; backups ectorB-pre-20260828-061555 / ectorB-deploy-pre-20260828-061555. Left INI at eplaceOutput=0 queueCopy=0 and did not alter queue-copy/Present/resource states. Vector A remains but is known to miss this content's draw-based present; Vector B is the first OM-based visible experiment.


## 2026-08-28 Vector B provenance diagnostic deployed (observation-only)

Hook_CreateRenderTargetView at src\d3d12_hooks.cpp:1604 now logs every display-sized RTV creation as tv-provenance: create handle=%llX resource=%p size=%ux%u fmt=%u flags=%u. TryVectorB at src\d3d12_hooks.cpp:2784 is now observation-only: it logs every non-zero OM handle as ectorB: probe handle=%llX foundInMap=%u resource=%p isDisplay=%u format=%u present=%llu ecl=%llu, logs ectorB: candidate for display-sized mt 11/10 with pending/completed, and when any isDisplay is seen dumps ectorB: rtvMap handle=%llX resource=%p size=%ux%u fmt=%u for first 16 entries. No SafeOverwriteRTV is called. g_vectorAEnabled set false for this build. Built VC2026 → dist\ScaleNG.asi 968192 D913D482… / helper 1B661AF4…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified, backups provenance-pre-20260828-064018. eplaceOutput=0 queueCopy=0 preserved. This build does not alter rendering; it only identifies the OM handle provenance for the persistent 1920x983 fmt11 resource.


## 2026-08-28 Vector B guarded one-shot substitution enabled

Enabled TryVectorB at src\d3d12_hooks.cpp:2784 to perform one SafeOverwriteRTV(g_b2OutG, candidateHandle) via Shim_OMSetRenderTargets src\d3d12_hooks.cpp:1235 only when runtime-resolved handle passes all gates: presentSerial>300, oundInMap=1, persistent (g_sceneColorCandidates src\d3d12_hooks.cpp:851), 1920x983 fmt11/10, es!=g_bbCached, pending!=0 completed>=pending, one-shot g_vectorBOneShot. Logs candidate/ttempting/substituted/skipped with handle/resource/size/format/present/ecl/pending/completed. Preserved tv-provenance and ectorB: probe / tvMap diagnostics. g_vectorAEnabled remains false, no queueCopy/Present copy. Built VC2026 → dist\ScaleNG.asi 969728 6E1C5D22… / helper  42C3CC2…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified, backups ectorB-subst-pre-20260828-070024. INI eplaceOutput=0 queueCopy=0. This is the first OM-based visible experiment; success requires ectorB: substituted plus persistent target and ecorded=1 fence for same pending and user-visible change.


## 2026-08-28 Vector B visible one-shot (samples>=1) deployed

Changed TryVectorB at src\d3d12_hooks.cpp:2859 to samples>=1 for this guarded one-shot. No change to tv-provenance, dimensions/format, backbuffer exclusion, fence completed>=pending, presentSerial>300, atomic one-shot g_vectorBOneShot/g_b2DeferredPending, exception guard SafeOverwriteRTV, or SafeGetDesc. Runtime-resolved handle/resource via g_rtvMap BookGuard, 1920x983 fmt11/10 isDisplay, es!=g_bbCached. One substitution only via Shim_OMSetRenderTargets src\d3d12_hooks.cpp:1235 g_device->CreateRenderTargetView(g_b2OutG). Vector A/C disabled, eplaceOutput=0 queueCopy=0. Built VC2026 → dist\ScaleNG.asi 969728 90C0DEA8… / helper BDA308A4…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified, backups ectorB-visible-pre-20260828-073732. Logs ectorB: attempting/substituted with handle/resource/Present/ECL/pending/completed. Previous p16148 200479B52C0 1920x987 at present 3567 would have qualified under >=1 (was >=3).


## 2026-08-28 Vector B full-slot observation deployed (no substitution)

Made TryVectorB at src\d3d12_hooks.cpp:2784 observation-only per spec: scans handles[0..count) independently, logs ectorB: slot=%u handle=%llX foundInMap=%u resource=%p size=%ux%u fmt=%u samples=%u present=%llu ecl=%llu pending=%llu completed=%llu isDisplay=%u for every non-zero handle, logs ectorB: candidate when isDisplay, logs ectorB: omCall with all handles and ectorB: rtvMap dump when any isDisplay true, does not return early after handles[0]. Kept exact 1920x983 fmt11/10 isDisplay, samples via g_sceneColorCandidates, no SafeOverwriteRTV call. Hook_CreateRenderTargetView src\d3d12_hooks.cpp:1604 still tv-provenance for every >=1000x500 RTV. Built VC2026 → dist\ScaleNG.asi 969216 BB4A428B… / helper 68FFE51C…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified, backups ectorB-observe-pre-20260828-075228. eplaceOutput=0 queueCopy=0.


## 2026-08-28 Vector B armed reduced-log deployed

Reduced TryVectorB src\d3d12_hooks.cpp:2784 logging to sawDisplaySized||fenceReady only, added armed state g_vectorBArmed* src\d3d12_hooks.cpp:2593 when samples>=1 1920x984 fmt11 with enceReady exists, even if current OM binds other. Next OM that binds exact g_vectorBArmedHandle logs rmed-match and one-shot SafeOverwriteRTV. Kept tv-provenance, eplaceOutput=0 queueCopy=0, Vector A disabled. Built VC2026 → dist\ScaleNG.asi 971264 9CE2E3DE… / helper  6D2C0A2…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified, backups ectorB-armed-pre-20260828-084617.


## 2026-08-28 Handle provenance diagnostic (observation-only)

Investigated handle reuse for 1920x983 fmt11 RTV 1692DC5C0A0 armed at present 919 but never bound later. Added g_lastDisplayRTV* tracking src\d3d12_hooks.cpp:730, tv-provenance: handle reuse and ectorB: lastDisplay bound logs src\d3d12_hooks.cpp:1604 2784, and reduced TryVectorB logging to sawDisplaySized||fenceReady only. Kept oundInMap isDisplay persistent ence present>300 gates but disabled SafeOverwriteRTV substitution for this build. Built VC2026 → dist\ScaleNG.asi 970240 2EF4B9A7… / helper EE115C5B…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified. Vector A/C disabled, eplaceOutput=0 queueCopy=0.


## 2026-08-28 Resource-level correlation diagnostic (observation-only)

Added DisplayRTVRecord history src\d3d12_hooks.cpp:730 for last 8 1920x984 fmt11 creations, tv-provenance: handle reuse and tv-provenance: display history logs src\d3d12_hooks.cpp:1604, and TryVectorB src\d3d12_hooks.cpp:2784 resource-level ectorB: resource-match creationHandle=%llX creationRes=%p creationPresent=%llu omHandle=%llX omRes=%p omPresent=%llu samples=%u pending=%llu completed=%llu persistent=%u when OM es equals tracked esource even if handle recycled. Also ectorB: lastDisplay bound when handle==g_lastDisplayRTVHandle. Kept oundInMap isDisplay samples>=1 present>300 ence gates but disabled SafeOverwriteRTV for this build. Built VC2026 → dist\ScaleNG.asi 970752 E6A7E635… / helper 227DFB37…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified. Vector A/C disabled, eplaceOutput=0 queueCopy=0.


## 2026-08-28 Resource-level OM correlation (observation-only)

Hook_CreateRenderTargetView src\d3d12_hooks.cpp:1604 now DisplayRTVRecord history for 1920x984 fmt11, TryVectorB src\d3d12_hooks.cpp:2784 logs ectorB: resource-match when OM es equals tracked esource even if handle recycled, with creationHandle/Present omHandle/Present samples pending/completed. Reduced shouldLogDetails to sawDisplaySized||fenceReady. No SafeOverwriteRTV, Vector A/C disabled, eplaceOutput=0 queueCopy=0. Built VC2026 → dist\ScaleNG.asi 7358171B… / helper 227DFB37…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified.


## 2026-08-28 Resource-level OM correlation unconditional (observation-only)

Made TryVectorB resource-level ectorB: resource-match unconditional on present/sawDisplaySized/enceReady, still 1920x984 fmt11 oundInMap persistent 
on-backbuffer but logs creationHandle/Present omHandle/Present size/fmt samples pending/completed persistent ecycled for every OM where es equals tracked display esource even if handle recycled. No SafeOverwriteRTV. Built VC2026 → dist\ScaleNG.asi F0EA1863…. Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified.


## 2026-08-29 Vector B format-agnostic 1000-frame test

Made omRes == tracked display resource authoritative in TryVectorB src\d3d12_hooks.cpp:2784: accept 1920x983 mt 10 or 11 at OM, persistent>=1 present>300 completed>=pending 
on-backbuffer, samples via g_sceneColorCandidates/g_displayRTVHistory, ecycled via creationHandle vs omHandle. p18420/949 16097B55A40 mt10 now qualifies as esource-match present 949 creation 919 persistent=1. Test arms on first qualifying OM esource-match and runs 1000 Presents TEST START/FRAME/END via SafeOverwriteRTV src\d3d12_hooks.cpp:1007 one OM handle. Vector A/C disabled, eplaceOutput=0. Built VC2026 → dist\ScaleNG.asi C9E2D314… / helper .... Deployed to C:\games\BeamNG.drive\Bin64\plugins hashes verified.

