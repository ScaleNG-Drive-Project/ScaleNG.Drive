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
   motion vectors (`R16G16_FLOAT`, display-size; primary + ALT rotation —
   a retired/wrong-sized primary falls through to a live display-sized
   ALT, `src/d3d12_hooks.cpp:7155-7165`; format gate
   `src/d3d12_hooks.cpp:7170`),
   depth (depth-family formats 39/40/41/44/45/46 per `IsDepthFamilyFormat`,
   `src/d3d12_hooks.cpp:697-705`; SRV-sourced preferred;
   copy-heuristic adopts only depth-family targets and never overwrites
   SRV-sourced depth, `src/d3d12_hooks.cpp:8635-8670`). Typeless D24 is
   passed to NGX raw with no conversion (`Depth` set at
   `src/dlss_ngx.cpp:671`; verified by inspection of the eval path —
   SDK-sanctioned per current docs). MSAA, tiny, and unknown-format targets are
   excluded (`SampleDesc.Count != 1` filter,
   `src/d3d12_hooks.cpp:552-555`; `g_depthMsaa` rejection,
   `src/d3d12_hooks.cpp:7175`). All tracked engine pointers are weak by design (no AddRef).
   Limitation: the freshness gate is asymmetric and weak — MV older than
   10 frames is rejected but depth up to 20000 frames old is accepted
   (`src/d3d12_hooks.cpp:7177-7178`), so depth may be stale, and there
   is no MV–depth co-freshness check (verified by inspection; observed
   effects in [STATUS.md](STATUS.md)).
3. **NGX init.** After ~600 quiet presents the plugin loads the real
   driver core (`nvngx.dll` from the DriverStore, not the feature
   snippet) and calls `Init_Ext` (classic `Init` fallback), both
   SEH-wrapped. Init runs on the game device. Single attempt, retried
   from camera acceptance and at 1/60 presents until ready.
4. **Feature creation.** One DLSS feature at native size
   (`render == display == backbuffer`, retargeted per display size via
   `UpdateSizes`; the INI `scale` value is currently NOT honored on this
   path). Create flags (`src/dlss_ngx.cpp:608-612`): `IsHDR` (default ON, F7 flips to LDR +
   recreates); `MVJittered` (INI `mvJittered=1`) — set while jitter is
   always 0/0, i.e. a subtract-zero no-op, self-consistent
   (`src/dlss_ngx.cpp:609`, `src/d3d12_hooks.cpp:7229`); `MVLowRes`
   (defined `src/dlss_ngx.h:50`) is never set — correct iff the MV is
   full-res, which the eval path enforces by size check
   (`src/d3d12_hooks.cpp:7172`); `DepthInverted` (defined
   `src/dlss_ngx.h:52`) is never set, i.e. the code assumes the standard
   near-0/far-1 convention — UNPROVEN, near/far planes are never sent;
   `AutoExposure`
   (INI `autoExposure=1`, no exposure texture supplied). Camera matrices
   (`InvViewProjectionMatrix` / `ClipToPrevClipMatrix`) are deliberately
   unset (`src/dlss_ngx.cpp:689-691`). `Reset=1` on the first evaluate
   (`src/dlss_ngx.cpp:683`; re-armed by `ResetFeature`,
   `src/dlss_ngx.h:182`). 1/sec retry
   throttle, device-removed pre-check. Own 4096-descriptor
   shader-visible heap is bound around evaluation; engine heaps restored
   after.
5. **Evaluation (shadow path, once per Present).** Color input is the
   current presented backbuffer (`R8G8B8A8_UNORM`, post-HUD — UI pixels
   go through DLSS). MV/depth default to owned zero-filled placeholders
   (`R16G16_FLOAT` / `R32_FLOAT` at display size, parked in PSR,
   `src/d3d12_hooks.cpp:6917-6920`, `:6926-6955`; `mvScale` 1.0 on zeros,
   `src/d3d12_hooks.cpp:7142`); with
   `realInputs=1` or live F9, validated engine MV/depth are transitioned
   to shader-readable via tracked-state barriers and restored in the
   same command list (`src/d3d12_hooks.cpp:7186-7191`, `:7233-7237`).
   Jitter is always 0/0 (the render is unjittered;
   camera-CB jitter injection stays off outside DLAA mode,
   `src/d3d12_hooks.cpp:7229`). `mvScale` on real inputs is the MV buffer
   W/H (`src/d3d12_hooks.cpp:7188`) — a UV-delta→pixel assumption
   (`src/dlss_ngx.cpp:684-688`): documented, not measured (UNKNOWN).
   Sharpness comes from INI (0.0; the SDK declares
   sharpening unsupported). Own fence + 2 command-list slots; a present
   is skipped non-blockingly while the prior frame is in flight.
   Provenance/logging: per-eval input selection is logged on transitions
   plus periodically with the ok counter (`src/d3d12_hooks.cpp:7201-7220`,
   `:7296-7302`); owned placeholders vs engine candidates are
   distinguished by pointer identity plus the `IsOwnResource` guard
   (`src/d3d12_hooks.cpp:6791-6803`, `:415-432`; `self-input` rejection
   `:7171`); a bounded NGX parameter echo in Evaluate is being added
   separately (status in [STATUS.md](STATUS.md)).

   | NGX input / flag | Sent on this path | Evidence status |
   |---|---|---|
   | `Jitter.Offset.X/Y` | always 0/0 | verified, `src/d3d12_hooks.cpp:7229` |
   | `MV.Scale.X/Y` | MV buffer W/H (real) / 1.0 (zeros) | verified sent; UV-delta reading UNKNOWN |
   | `Reset` | 1 on first eval, else 0 | verified, `src/dlss_ngx.cpp:683` |
   | `MVJittered` | set while jitter is 0 | verified sent; self-consistent no-op |
   | `MVLowRes` | never set | verified absent; correct iff full-res MV |
   | `DepthInverted` | never set | verified absent; near-0/far-1 convention UNPROVEN |
   | Depth resource | typeless D24 passed raw, no conversion | verified sent; value convention UNKNOWN |
   | Camera matrices, near/far | never sent | verified absent, `src/dlss_ngx.cpp:689-691` |
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
`depth-stale`, `untracked-state`). Our own textures are blocked from
adoption as engine inputs in all observed cases (`IsOwnResource` +
`StoreTracked` bool + metadata early-outs; `self-adopt rejected`
observed live — guard live, not a proof of impossibility).

## Safety fallbacks

- **Fault circuit breaker (live but UNFIRED — reset/halt paths
  unexercised live):** 30 consecutive eval faults → feature reset
  (fresh NGX history, logged); 120 → halt evals for the session so the
  game presents unmodified frames (logged once). Fault logging is
  bounded (first 10 + every 600). `DestroyFeature` is SEH-guarded with
  null-before-release (no dangling handle).
- Failures never corrupt the frame: eval failure, feature failure, and
  halt all present the engine's original backbuffer (except the
  192627Z CreateFeature crash, which crashed before any frame
  fallback could present).

## Proven vs open (see STATUS.md for run IDs)

- Proven: NGX init → feature → thousands of consecutive eval+handoff
  frames (ok `#N` counters; `ok` lines sampled — up to #9000) on
  zeros, on engine MV + engine velocity-as-depth, and on engine MV +
  engine depth-family input (fmt-45 family; depth value convention
  UNKNOWN); F8 visibly gates handoff; F9 switches input sets
  (7/7 logged in user run 20261007T204943Z) with fail-closed
  fallback to zeros; breaker paths are live but unfired.
- Open / unknown: same-frame color/MV/depth association (frame clock
  frozen); MV sign/axis/scale and depth value conventions; any
  image-quality improvement (screenshot pixel-diff is invalid while the
  camera moves — null control differs 87–96% with no toggle); baseline
  crash-freedom UNKNOWN (CreateFeature AV 192627Z and two fault
  storms observed — 1904-fault cluster 194541Z and 7390-storm
  200904Z starting 1 ms after an F9 press with F9/focus involvement
  UNRESOLVED both directions, storm continuing after toggling back —
  causes undetermined). INI `scale` (sub-native rendering) and camera jitter
  injection are not active, so REAL currently differs from fallback only
  by MV/depth content at native resolution with frozen jitter.

## Inert subsystems (do not rely on)

Legacy engine-list injection (`DoInjection`,
`src/d3d12_hooks.cpp:2604-2710` — inert: early-returns in DLAA mode and
its trigger is gated to the non-DLAA path, `:2609`, `:8615-8617`; noted
unresolved, not fixed here: its render-vs-MV size pairing is SUSPECT —
feature render size vs display-size MV with `mvScale=g_mvW/H`,
`:2641-2650`), viewport render-scale
patch (replay-blocked: in DLAA mode viewports pass through untouched,
`Hook_RSSetViewports`, `src/d3d12_hooks.cpp:8727-8730`, and arming is
disabled via `!g_dlaaMode`, `:2333`; observationally the patch never
survives to trigger — the engine renders full-res, so native DLAA is the
only coherent target today, `src/d3d12_hooks.cpp:7061-7074`; about ten
display-size sizing assumptions thread through
discovery/sizing/validation — audit-carried count, not individually
re-verified here; an off-engine sub-native shadow eval is the ranked next
step, not promised; observations in [STATUS.md](STATUS.md)), bridge/b2 cross-device flow, HUD overlay drawing, synthetic
smoke test, deferred output. `dlaa=1` currently yields no eval path
(silent no-op). `[bridge]` INI keys are parsed but ineffectual while
bridge creation is disabled. UNRESOLVED inconsistency: `dist\`
ships `deferredOutput=1` vs code default `0`. `enabled=0` is dead
(forced on after load).
