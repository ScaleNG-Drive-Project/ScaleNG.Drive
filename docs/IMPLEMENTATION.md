# ScaleNG implementation (current, 2026-10-08)

This page describes what the shipped ASI build actually does. Prior
designs live in [archive/](archive/README.md) and
[TECHNICAL_REFERENCE.md](TECHNICAL_REFERENCE.md) (historical); when they
disagree with this page, this page wins. For operator steps see
[INSTALL.md](INSTALL.md); for test evidence see [STATUS.md](STATUS.md).

## What it is

ScaleNG is a UAL-loaded ASI plugin (`ScaleNG.asi`) that hooks BeamNG.drive's
Direct3D 12 calls and runs NVIDIA DLSS Super Resolution on the presented
frames. It works on the game's own D3D12 device (single-device design; the
older cross-device bridge is inert — creation disabled, `g_bridgeReady`
never true).

## Pipeline (live path)

1. **Hook install.** On load the plugin detours `D3D12CreateDevice` and
   installs a vectored exception handler. A camera-constant-buffer smoke
   test exists but is disabled (`src/main.cpp:218-227`).
2. **Discovery (always on).** Creation/binding/barrier/copy hooks track
   three engine input slots: scene color (`R16G16B16A16_*`, display-size),
   motion vectors (`R16G16_FLOAT`, display-size, primary + ALT rotation),
   depth (depth-family formats 39/40/41/44/45/46, SRV-sourced preferred;
   copy-heuristic adopts only depth-family targets and never overwrites
   SRV-sourced depth). MSAA, tiny, and unknown-format targets are
   excluded. All tracked engine pointers are weak by design (no AddRef).
3. **NGX init.** After ~600 quiet presents the plugin loads the real
   driver core (`nvngx.dll` from the DriverStore, not the feature
   snippet) and calls `Init_Ext` (classic `Init` fallback), both
   SEH-wrapped. Init runs on the game device. Single attempt, retried
   from camera acceptance and at 1/60 presents until ready.
4. **Feature creation.** One DLSS feature at native size
   (`render == display == backbuffer`, retargeted per display size via
   `UpdateSizes`; the INI `scale` value is currently NOT honored on this
   path). Create flags: `IsHDR` (default ON, F7 flips to LDR +
   recreates), `MVJittered` (INI `mvJittered=1`), `AutoExposure`
   (INI `autoExposure=1`, no exposure texture supplied). 1/sec retry
   throttle, device-removed pre-check. Own 4096-descriptor
   shader-visible heap is bound around evaluation; engine heaps restored
   after.
5. **Evaluation (shadow path, once per Present).** Color input is the
   current presented backbuffer (`R8G8B8A8_UNORM`, post-HUD — UI pixels
   go through DLSS). MV/depth default to owned zero-filled placeholders
   (`R16G16_FLOAT` / `R32_FLOAT` at display size, parked in PSR); with
   `realInputs=1` or live F9, validated engine MV/depth are transitioned
   to shader-readable via tracked-state barriers and restored in the
   same command list. Jitter is always 0/0 (the render is unjittered;
   camera-CB jitter injection stays off outside DLAA mode). mvScale is
   1.0 on zeros, MV pixel dimensions on real inputs (UV-delta
   assumption). Sharpness comes from INI (0.0; the SDK declares
   sharpening unsupported). Own fence + 2 command-list slots; a present
   is skipped non-blockingly while the prior frame is in flight.
6. **Visible handoff (F8, default ON).** On successful eval the DLSS
   output is copied back into the backbuffer before `Present`, with all
   states restored. On eval failure the original frame presents
   untouched. The handoff copy is independent of the input source.

## Modes

| Mode | Inputs | Default | How to select |
|---|---|---|---|
| Fallback (zeros) | backbuffer color + owned zero MV/depth, jitter 0/0 | **ON** (`realInputs=0`) | default; F9 toggles back |
| Real inputs | backbuffer color + engine MV/depth (fail-closed validation; any doubt → zeros) | OFF | INI `realInputs=1` or live F9 |
| Handoff | writes eval output to the presented frame | ON (`shadowHandoff=1`) | live F8 |

Validation rejects to zeros with a logged reason (`no-mv`,
`mv-retired`, `mv-format`, `self-input`, `mv-size`, `no-depth`,
`depth-retired`, `depth-fmt`, `depth-size`, `mv-stale` (>10 frames),
`depth-stale`, `untracked-state`). Our own textures can never be adopted
as engine inputs (`IsOwnResource` + `StoreTracked` bool + metadata
early-outs; `self-adopt rejected` proves it live).

## Safety fallbacks

- **Fault circuit breaker:** 30 consecutive eval faults → feature reset
  (fresh NGX history, logged); 120 → halt evals for the session so the
  game presents unmodified frames (logged once). Fault logging is
  bounded (first 10 + every 600). `DestroyFeature` is SEH-guarded with
  null-before-release (no dangling handle).
- Failures never corrupt the frame: eval failure, feature failure, and
  halt all present the engine's original backbuffer.

## Proven vs open (see STATUS.md for run IDs)

- Proven: NGX init → feature → thousands of consecutive eval+handoff
  frames (up to ok #9000) on zeros, on engine MV + engine velocity, and
  on engine MV + engine depth-family input; F8 visibly gates handoff;
  F9 switches input sets (7/7 logged); fallbacks and breaker paths.
- Open / unknown: same-frame color/MV/depth association (frame clock
  frozen); MV sign/axis/scale and depth value conventions; any
  image-quality improvement (screenshot pixel-diff is invalid while the
  camera moves — null control differs 87–96% with no toggle); baseline
  crash-freedom (CreateFeature AV and two fault storms observed, causes
  undetermined). INI `scale` (sub-native rendering) and camera jitter
  injection are not active, so REAL currently differs from fallback only
  by MV/depth content at native resolution with frozen jitter.

## Inert subsystems (do not rely on)

Legacy engine-list injection (`DoInjection`), viewport render-scale
patch, bridge/b2 cross-device flow, HUD overlay drawing, synthetic
smoke test, deferred output. `dlaa=1` currently yields no eval path
(silent no-op). `[bridge]` INI keys are parsed but ineffectual while
bridge creation is disabled.
