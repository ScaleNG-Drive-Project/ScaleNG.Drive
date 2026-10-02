# ScaleNG.Drive — genuine DLSS/DLAA integration plan

Updated 2026-10-02 (Updated with ReShade Integration Lessons)

## Goal

Produce visible DLSS or DLAA output in BeamNG.drive while preserving a normal, resizable, stable game window. A run counts as successful only when the user sees a repeatable image change attributable to the plugin and the log proves that NGX evaluation and the game-frame handoff occurred in the same frame sequence.

## Current evidence (Updated with ReShade findings)

| Area | Proven result | Consequence |
|---|---|---|
| Plugin loading | ReShade dxgi.dll proxy loads into BeamNG successfully. | Loader/deployment is not the blocker. |
| BeamNG device/queue | ReShade captures device and graphics queue via swapchain. | We have an ordering anchor. |
| Present | Stable Present stream and active backbuffer observed. | Present is a reliable observation point. |
| NGX In-Process | ReShade could run NGX in-process on game's device. | Cross-process helper NOT needed. |
| Native render graph | ReShade's `init_resource` event discovers depth/MV/color at creation. | Native render graph IS visible at resource creation time. |
| Direct Present replacement | ReShade manages runtime per swapchain cleanly. | Present-time injection via backbuffer copy works. |
| Stability | ReShade runs stable for hours in BeamNG. | DXGI proxy + event system is stable. |
| Resize | ReShade handles swapchain resize via `init_swapchain` event. | Resize handled natively. |

## Actual blocker (Updated)

The blocker was never DLSS initialization — it was **cross-process architecture + D3D12 vtable hooks**. 

ReShade proves: **DXGI proxy + event-based resource discovery + in-process NGX = stable DLSS integration.**

---

## Rules for every build (Updated)

1. One build has one hypothesis.
2. Observation builds must not write to game resources, alter command lists, or use cross-process helper.
3. Mutation builds are disabled by default and enabled only after the preceding observation gate passes.
4. Every mutation has a one-shot limit, pointer/descriptor/fence logging, and a circuit breaker on device removal, black-window behavior, or crash-adjacent failure.
5. Width and height alone never qualify a resource. Ownership, format, ordering, and lifetime must also be proven.
6. Visible DLSS is not claimed from helper success alone; it requires a user-visible change plus matching handoff evidence.
7. **NEW: Hook at DXGI level (CreateDXGIFactory, CreateSwapChainForHwnd), NOT D3D12 vtables.**

---

## Phases and exit criteria (Revised — ReShade-based)

### Phase 0 — Deploy ReShade-style DXGI proxy

Deploy `dxgi.dll` (renamed ScaleNG.dll) to BeamNG Bin64. Verify:
- dxgi.dll loads automatically (no UAL needed)
- `CreateDXGIFactory1` hook fires
- `CreateSwapChainForHwnd` hook fires
- Game launches and reaches main menu

### Phase 1 — Event-based resource observation

Implement ReShade-style event system:
1. `init_device` → capture device pointer
2. `init_swapchain` → capture swapchain, back buffer count
3. `init_resource` → discover depth (D32_FLOAT/D24S8), motion vectors (R16G16_FLOAT), color (R16G16B16A16_FLOAT/UNORM)
4. `copy_texture_region` → trace render pipeline (src→dst copies)
5. `copy_buffer_to_texture` → track upload heap → texture copies

Exit: One stable depth, motion vector, and color resource discovered at creation time, tracked through render pipeline.

### Phase 2 — In-process NGX initialization

At `init_effect_runtime` equivalent (swapchain + device + queue captured):
1. Initialize NGX on game's device (same device ReShade uses)
2. Create NGX feature with render/display dimensions
3. Verify NGX evaluates on synthetic frames

Exit: NGX Init + CreateFeature + Evaluate all succeed on game's device.

### Phase 3 — Present-time NGX evaluation

At Present event:
1. Get current back buffer: `swapchain->get_back_buffer(0)`
2. Barrier: backbuffer PRESENT → COPY_SOURCE
3. Copy backbuffer → color input (g_color)
4. Barrier: g_color COPY_DEST → SRV
5. NGX Evaluate: color + depth + motion vectors + jitter → output
6. Barrier: output UAV → COPY_SOURCE
7. Copy output → backbuffer
7. Barrier: backbuffer COPY_DEST → PRESENT
8. Submit command list on graphics queue

Exit: NGX evaluates every frame, output visible in backbuffer, no crashes.

### Phase 4 — ImGui overlay + user validation

1. Register ImGui overlay via `register_overlay`
2. Show DLSS status, render/display resolution, FPS
3. User validates visual quality improvement

Exit: User confirms visible DLSS improvement, no artifacts.

### Phase 5 — Hardening

Handle resize, alt-tab, minimize, shutdown. Reduce logging. Package `dxgi.dll` + `dxgi.ini` + `nvngx_dlss.dll`.

---

## Immediate next action

**Phase 1:** Implement DXGI proxy (`dxgi.dll`) with event system (`init_device`, `init_swapchain`, `init_resource`, `copy_texture_region`, `copy_buffer_to_texture`). Deploy as `dxgi.dll` to BeamNG Bin64. Verify all events fire and depth/MV/color resources discovered.

---

## Stop conditions

Roll back if BeamNG crashes, reports device removal, becomes black, stops presenting, repeatedly faults on resource descriptors, changes candidate identity without a resize/reinitialization event, or if D3D12 vtable hooks are used.
