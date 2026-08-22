# ScaleNG.Drive — Full Project Documentation

**Goal:** A from-scratch DLSS/FSR upscaler integration for BeamNG.drive (DX12, v0.39) as an ASI plugin, loaded through OptiScaler's built-in ASI loader. Personal use first; public release only at v2+.

**Constraints (user):** upscaling only — no frame generation; user has no coding background (agent writes code, user tests); NVIDIA RTX GPU; Visual Studio 2022 Community installed; no code is written until the user explicitly approves.

---

## 1. Why this project exists

BeamNG.drive renders natively (no internal render-scale option), so DLSS/FSR cannot be enabled through the game. OptiScaler provides frame gen for unsupported games but cannot inject DLSS upscaling without the game cooperating. This plugin re-implements what the game lacks: internal render scale + DLSS upscale, injected via D3D12 API hooks.

## 2. The verification work (what was found, and why it matters)

All findings verified against PIX GPU captures and the raw C++ replay export (107,270 calls, 15,491 global IDs, 2 frames captured).

### 2.1 Frame architecture (verified, supersedes earlier wrong claims)

| Stage | Global IDs | Resources / PSOs | Notes |
|---|---|---|---|
| Shadow probe passes | ~15–63 | PSO 254/RS 255 → res 223 (128×128 R16G16B16A16_FLOAT) + depth 225; also RS 236/PSO 256 with camera pair 201/202 | **NOT the main camera** — was previously misidentified as such |
| Main scene | 3456–4936 (list 64) | PSOs 12675–12716, RS 12681, RTV **10911** (R16G16B16A16_UNORM 1920×992), DSV **3051** (D24_UNORM_S8_UINT), camera CB **12672** | Full native res, no render scale. Terrain draw: DrawIndexedInstanced(26112) |
| Velocity compute | 4939 | PSO 13253/RS 13254, CBV 13233 (176B from Res 9 @ 137,922,560), Dispatch 240×124 → UAV 4092 → copy to 5178 (R16G16_FLOAT) | |
| Velocity render | ~4942+ | PSO 13318/RS 13267, camera CB 12672 at root param 1, depth 10888 read-only → 5178 | |
| Half-res effects chain | 7624–7632 | 1249→1957→1958 (PSOs 13397/13408/13415, RS 234), 768×396 | CBVs from Res 9 @ 138175488/138176512/138177536 |
| Combine | 7633–7634 | PSO 13413/RS 13414 → HDR 10062 (R16G16B16A16_FLOAT 1920×992) + depth 3051 | PS samples 3 textures: inputTex0 t0, inputTex1 t1, blendTex t2 |
| Tonemap/blit | 7635–7636 | PSO 13397 (pure blit, sample t0 → SV_Target, no tonemap) → LDR 227 (R8G8B8A8_UNORM 1920×992) | |
| Final blit | before Present | PSO 12007/RS 12008 pure blit | |
| Present | 7673 (frame 1) / 15512 (frame 2) | copy 227→10901 → backbuffer copy source 13387 / 13873 | |

Sky is drawn into 10062 first (GlobalId 3452, PSO 233/RS 234); 10062 snapshotted to 10890 at 7622–7623.

### 2.2 Camera constant buffer — the core discovery

Resource **12672** (1792 bytes), bound at root param 2, cb0/space2, named `RenderPassConstBuffer`. Layout confirmed by matching PSO 12685 VS disassembly field names/offsets against actual bytes:

| Field | Byte offset | Content (capture values) |
|---|---|---|
| `ambientSH9[9]` | 0 | ambient SH coefficients |
| `ambient` | 144 | ambient color |
| `fogData` / `accumTime` | 160 | fog params, time |
| `fogColor` | 192 | fog color |
| `oneOverFarplane` | 208 | 1/far |
| `worldToCamera` | 224 | **view matrix; col3 @ 272 = camera world pos (797.6, 3382.9, 70.3)** |
| `worldToCameraPos0` | 288 | origin-rebased view (col3 = 0,0,0,1) — engine renders camera-relative |
| `worldToScreenPos0` | 352 | jittered view-proj; **col3 @ 400 = (-0.1565, 0.1354, 1.0002, 1.0) = jitter** |
| `cameraRemainder` | 416 | remainder |
| `cameraToScreen` | 432 | projection; **col3 @ 480 = (-0.1565, 0.1354) = jitter** |
| `viewProj` | 496 | composite matrix; col3 @ 544 = camera pos0 coords (2.73, 0.39, 0.44) |
| `vEye` | 560 | eye vector |
| `eyePosWorld` | 576 | sun position (-2796.6, 813.7, 1897.8) — note: sun, not eye |
| `eyeMat` | 592 | sun basis matrix |
| `clipPlane0` | 672 | clip plane |
| `projectionParams` | 688 | far plane 6060.7 |
| `viewportParams` | 704 | viewport |
| `LightDataPSSM` | 768+ | cascade shadow data |
| `viewProjPrevFrame` | 1184 | prev-frame view-proj (engine already tracks it) |
| `worldToScreenPos0PrevFrame` | 1248 | prev-frame jittered view-proj |
| `prevEyePosWorld` | 1312 | prev eye/sun |
| `lastFrameExposure` | 1344 | ~0.01 |
| `fogSunDirection` | 1360 | sun direction |

> **Jitter note:** the values seen at 480–495 in the captured initial data turned out to be from *mirrored* (multi-camera) views — see §7.2 for the corrected, byte-verified jitter placement, which patches the live matrix coefficients instead of assuming a fixed translation column.

**Per-frame update mechanism:** 12672 is filled each frame via `CopyBufferRegion` from Res 9 @ **136,891,904** (frame 1) / **137,932,800** (frame 2), 1616 bytes (CommandLists_000.cpp:41231, CommandLists_001.cpp:5109). Res 9 is a big ring buffer; its initial data is **not per-frame trustworthy** (frame-2 slot was full of NaN).

### 2.3 Main-scene root signature RS 12681 (verified)

| Root param | Type | Register | Bound resource |
|---|---|---|---|
| 0 | CBV | cb4, space1 | 11497 (256B, cspMaterial) |
| 1 | CBV | cb5, space1 | 11498 (4096B, terrain PS data) |
| 2 | CBV | cb0, space2 | **12672 (1792B, RenderPassConstBuffer = camera CB)** |
| 3 | CBV | cb1, space3 | 11100 (cspPrimitive, per-draw, identity — nothing to hook) |
| 4 | Table | SRV t10/t13/t18, s1 | heap266@4484 etc. |
| 5 | Table | SAMPLER s0, s2 | heap266@129 |
| 6 | Table | SRV t11, s3 | heap266@4818 |
| 7 | Table | SRV t3, s4 | heap266@13300 |

Stencil ref 14 for scene draws.

### 2.4 Mistakes corrected (important)

- **"Main camera = Res 9 slots 127307264/138933760" (Nemotron's report) is WRONG.** Those slots are bound to PSO 254/RS 255 — a 128×128 fullscreen-triangle **shadow-probe pass** into res 223. Content = NaN-padded light/shadow matrices (cascade splits 0.14/0.044/0.0168/0.0065, near/far 40/160, unit light dirs). Not a camera.
- Resources 201/202 (1792B pair) = same camera struct layout but bound in the 128×128 shadow passes (RS 236/PSO 256) — different camera/time, not the main view. Jitter values seen there (-0.1787/0.1308, -0.1713/0.0567) belong to those shadow cameras.
- Prior claims that "final composite = PSO 12007 pure blit" still hold; but the composite chain is richer than first thought (13397 blit → 13413 combine → 13397 blit → 227).

## 3. The DLSS injection design (accepted middle ground)

User decision: *"accept current understanding but figure out a better solution from there"* — i.e., do NOT fully decode every offset statically; accept the layout above and design for runtime discovery.

Pipeline position: DLSS runs **after the main scene (10911 RTV→SRV transition) and before the composite chain**, consuming low-res scene color + depth 3051 + velocity 5178 + patched camera matrices, producing native-res color that replaces 10911 at the PSO 13413 combine (inputTex0).

1. **Render scale** — hook `CreateCommittedResource`/`CreatePlacedResource` → shrink scene color (10911), depth (3051), velocity (4092/5178) by scale. Hook `RSSetViewports`/`RSSetScissorRects` → scale viewport. Hook the per-frame `CopyBufferRegion` into 12672 → widen `cameraToScreen`/`viewProj` FOV by 1/scale (frustum unchanged).
2. **Jitter** — replace engine jitter (cameraToScreen col3 @ 480) with DLSS halton jitter each frame; cache prev-frame matrices in plugin.
3. **DLSS call** — feed low-res 10911 + depth + velocity + patched matrices → DLSS upscale → native-res color into PSO 13413 combine (swap inputTex0).
4. **TAA conflict** — engine TAA (jitter proves it exists) must be disabled in-game, else double-temporal artifacts.

**Rule: per-capture resource IDs must NOT be hardcoded at runtime** — runtime discovery strategy: hook `CreateCommittedResource`/`CreatePlacedResource` matching format+dimensions+flags; root-signature bind patterns; swapchain copy chain; descriptor-heap scan.

> The final implemented approach refines this (see §7): **viewport/scissor-patch-only scale** (no FOV widen, no RTV swap), camera CB patched **in place on the upload ring**, and the DLSS evaluate injected **after** the velocity copy-back. Read §7 for the full decisions.

## 4. Tooling & environment (how the work gets done)

- **pix-mcp** — MCP server giving an AI agent PIX access. Editable install at `C:\Users\Admin\Documents\Default Project\pix-mcp\.venv`; server registered as `pix` in `C:\Users\Admin\.config\opencode\opencode.json` (env `PIX_MCP_PIXTOOL=C:\Program Files\Microsoft PIX\2603.25\pixtool.exe`). **Running server predates parser patches — restart opencode to pick them up.**
- **Parser patches** applied to `src\pix_mcp\cpp_export.py` (`_PSO_FUNC_RE`, `_PSO_STAGE_RE`, `_PSO_ROOTSIG_RE`) and `src\pix_mcp\resources_bin.py` (`_TRACKED_PREFIXES`) to support `CreateGraphicsPipelineState_` parsing and per-PSO stage extraction.
- **Index tools unavailable** — `pix_index_events`/`pix_get_event`/`pix_find_events` need the SQLite event index which requires Windows Developer Mode. Use the C++-export path instead: `pix_state_at_event`, `pix_get_resource_bytes`, `pix_dump_cbuffer_at_root_param`, plus direct grep/read of exported `.cpp`.
- **Tools that work from the export:** `cpp_export.parse_export(root)` → export; `cpp_export.parse_pso_shader_stages(export, pso_id)` → `PsoStageLayout(pso_id, root_signature_id, compressed_blob_size, stages=[VS/PS offset+length])`; `cpp_export.list_all_psos(export)` (takes the export object, not a Path); `resources_bin.ResourceBin.from_export(root)`; `rb.chunks_by_pso[pso_id]` → `ResourceChunk`s; `rb.read_chunk(chunk)` → blob bytes; `rb.read_resource_bytes(rid, offset, length)`.
- **dxc.exe** at `C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe` — `-dumpbin` works on DXIL and DXBC.
- **PIX captures** live in `Documents\PIX\Captures`; capture key F11 in-app (only when launched via PIX).
- PowerShell quirk: `$PID` is read-only — never use it as a variable name.

## 5. ASI plugin (current state: full DLSS integration implemented, in-game verification pending)

- Loaded by OptiScaler's ASI loader. Deploy: `dxgi.dll` (OptiScaler) into BeamNG `Bin64`, `plugins` folder beside it, `LoadAsiPlugins=1`.
- Exports `InitializeASI` (void) and `PatchResult` (bool).
- Writes a timestamped log to `ScaleNG.log` beside the module.
- Source in `src\` (main.cpp, camera_cb, d3d12_hooks, dlss_ngx, log.h, upscaler.h, build.bat, vendor\minhook + vendor\nvngx). Built with `src\build.bat` → `dist\ScaleNG.asi` + `dist\ScaleNG.ini`.
- Design details: §7 below. In-game verification: §8 below.

## 6. Every file in the project

### `src/main.cpp` — ASI entry point
- Loads `ScaleNG.ini` (next to the module), sets render scale / sharpness / perfQuality / mvJittered / autoExposure / appId, resolves `nvngx_dlss.dll` path next to the module.
- `InitializeASI()` — LogInit, config load, installs hooks (`HooksInstallCreateDeviceDetour`).
- `PatchResult()` — returns `false` (no OptiScaler patching requested).
- `DllMain` — no-op.

### `src/d3d12_hooks.h/.cpp` — hook core
- MinHook setup; D3D12CreateDevice detour (chains through OptiScaler's Detours hook; inert+log on failure); device/queue/command-list vtable hooks (cmdlist slots 15/16/21/22/26/28/46, device 17/18/20, queue 10); resource discovery (scene color RTV, motion-vector RTV, depth candidates); frame state machine (frame-start on exposure pass, injection after velocity copy-back); camera CB + velocity CB patching on the upload ring; DLSS injection recording per frame.
- Hooks deliberately avoid OptiScaler's slots (D3D12CreateDevice + device slots 8/16/19/21/23/25).

### `src/camera_cb.h/.cpp` — camera constant buffer handling
- Validation, jitter patch (`f[8]+=jx·f[11], f[9]+=jy·f[11]` on worldToScreenPos0@352/viewProj@496/prev@1184/@1248; `f[4]+=jx·f[7], f[5]+=jy·f[7]` on cameraToScreen@432), matrix inverse for velocity CB.

### `src/upscaler.h` — IUpscaler interface (DLSS/FSR2 extensible)

### `src/dlss_ngx.h/.cpp` — NVSDK_NGX DLSS backend
- Loads `nvngx_dlss.dll` from plugins dir (fallback System32 `nvngx.dll`), Init/CreateFeature/Evaluate, descriptor-heap save/restore, root-sig/PSO save-restore around Evaluate.

### `src/vendor/minhook` + `src/vendor/nvngx` — MinHook (BSD-2-Clause) + NVSDK_NGX headers

### `src/build.bat` — one-click build
- Locates VS via `vswhere.exe`, calls `vcvars64.bat`, compiles with `cl /O2 /EHsc /std:c++17 /MT /LD` (response file, .obj into `src\build\`), emits `ScaleNG.dll`, renames to `ScaleNG.asi` in `dist\`. Self-contained (cd's to its own folder).

### `dist/` — deployable
- `ScaleNG.asi` (built plugin) + `ScaleNG.ini` (config template: enabled, upscaler=dlss, scale=0.67, perfQuality, mvJittered, autoExposure, appId).

### `docs/README.md` (this file) — full documentation
- Every change + rationale, file inventory, architecture findings, **design notes (§7) and in-game verification guide (§8) — unified here.**

### `docs/CACHE.md` — fast lookup table
- Purpose: agent cache. **Must be updated at the end of every prompt, without exception.** See that file.

### `research/5 2 frame capture/` — PIX capture + C++ replay export + saved shader CSOs (ground truth).

## 7. Design notes

NVIDIA DLSS (render-scale upscaling only) for BeamNG.drive v0.39 DX12, injected as an ASI plugin through OptiScaler's ASI loader. Built and verified against the PIX GPU capture export, the shader bytecode (dxc -dumpbin) and the Windows SDK d3d12.h.

### 7.1 Verification results (from the implementation session)

| Question | Result |
|---|---|
| RTV creation for the scene target (10911) | Created ONCE at engine init (a single CreateRenderTargetView block over ~12 RTVs, 1920x992 R16G16B16A16_UNORM). Runtime discovery by content (format + full-res + first match) is safe. |
| Main-scene texture binding | Textures are bound via SetGraphicsRootDescriptorTable (descriptor tables, not root SRVs). We never intercept descriptor tables. |
| Combine PSO 13413 input format | 10911 is R16G16B16A16_UNORM (LDR). Our DLSS output uses the same format, so the dlssOut->10911 copy needs no format conversion. |
| Separate TAA pass PSO | None found in the export between scene end and combine (GlobalId ~5000-7600). The v0.39 DX12 path has no TAA pass; do not run engine TAA + DLSS together anyway. |
| Exact vtable slots | Verified against `d3d12.h` 10.0.26100.0 by counting the interface blocks (scripted): ID3D12GraphicsCommandList CopyBufferRegion=15, CopyTextureRegion=16, RSSetViewports=21, RSSetScissorRects=22, ResourceBarrier=26, SetDescriptorHeaps=28, OMSetRenderTargets=46; ID3D12Device CreateShaderResourceView=18, CreateRenderTargetView=20; ID3D12CommandQueue ExecuteCommandLists=10. |
| nvngx.dll export spellings | Matched exactly against the vendored NVSDK headers: `NVSDK_NGX_D3D12_Init`, `NVSDK_NGX_D3D12_CreateFeature`, `NVSDK_NGX_D3D12_EvaluateFeature`, `NVSDK_NGX_D3D12_Shutdown`, `NVSDK_NGX_D3D12_GetParameters`, `NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetULL/SetD3d12Resource/SetVoidPointer/GetUI`. |
| DLSS MV convention | Engine velocity = UV-space [0,1] deltas; DLSS expects pixel-space deltas, so MV.Scale = (1920, 992) (motion-vector buffer dimensions) per DLSS Programming Guide 3.6.1.1/3.6.3. Jitter must be in render-res pixels (guide: "in pixels") - we pass the same render-res pixel offsets used for the NDC patch. |
| Velocity buffer resolution | The engine's velocity is FULL-res (1920x992); the combine/exposure passes do not sample velocity, so MVLowRes=0 and no upsample is needed. |
| d3d12.dll device creation | BeamNG creates its device through the D3D12CreateDevice export (Agility SDK or system d3d12.dll - the plugin links only against the plain SDK headers, so it works either way). |

### 7.2 Decisions and rationale

**7.2.1 Render scale = viewport/scissor patch only**
The scene renders into the full-res 10911 target but with a shrunk viewport/scissor
(0,0,1286,664 at scale 0.67). All engine RTVs/DSVs and matrix math stay untouched.
Rationale: swapping RTV/DSV views or FOV-widening the projection would break other
full-res passes (e.g. the full-res sky pass 10062) and would be a much larger patch
surface. The viewport patch is the smallest correct intervention; DLSS is told
Width=1286/Height=664 (render res) and OutWidth=1920/OutHeight=992 and reads the
top-left 1286x664 region of 10911.

**7.2.2 Jitter placement (revised after byte-level verification)**
Earlier work assumed the engine's jitter lived in the 16 bytes at camera CB offset
480-495. Byte-level comparison of two captured frames showed the sampled copies were
MIRRORED camera views (all +-1.0 sign flips), so that assumption was invalidated.
Instead, the engine matrix convention was decoded from the bytes:

- Camera space: X-right, Y-forward (depth), Z-up.
- cameraToScreen (offset 432): clip.w = m[3][1]*camY (= +-1 * camY), clip.x = camX,
  clip.y = camZ -> NDC = (camX/camY, camZ/camY).
- worldToScreenPos0 (offset 352) / viewProj (offset 496) / their PrevFrame variants
  (1184 / 1248): clip.w = m[3][2]*wz (= +-1 * wz), clip.x = wx, clip.y = wy.

A jitter of (jx, jy) NDC units is therefore applied by adding jx*(the coefficient of
the depth input in clip.x) and jy*(coefficient of depth in clip.y):

- worldToScreenPos0: f[8] += jx*f[11]; f[9] += jy*f[11]
- viewProj:          f[8] += jx*f[11]; f[9] += jy*f[11]
- cameraToScreen:    f[4] += jx*f[7];  f[5] += jy*f[7]   (depth column is Y here)
- viewProjPrevFrame / worldToScreenPos0PrevFrame: same as above but with the
  PREVIOUS frame's jitter.

Multiplying by the live coefficient (which is +-1) makes the patch correct for
mirror/multi-camera variants automatically. jx = jitterPxX*2/renderW, jy = jitterPxY*2/renderH
with jitter in render-res pixels (Halton(2,3) sequence, +-0.5px, frame counter).

**7.2.3 Camera CB patched IN PLACE**
The engine copies 1616-byte camera CBs from a GPU_UPLOAD ring (ring resource 9) into
the camera buffer via CopyBufferRegion. Two variants are written per frame (one
placeholder without lighting, one full); we validate each copy's content and patch it
IN PLACE on the ring before the copy is recorded (Map on the upload resource is
CPU-side, no sync stall). The first validated 1616B copy of a frame is the frame-start
marker (advances the Halton counter, rotates curr/prev jitter, re-enables the
viewport patch). If validation fails, the copy is left untouched and the frame is
skipped (fail-safe).

**7.2.4 Velocity CB patched from the patched camera CB**
The velocity CB (176B, CopyBufferRegion 4092->5178) contains uTexSize, uScreenToWorldPos0
(inverse projection for world reconstruction, camera-relative: col3 = (0,0,0,1)) and
uPrevWorldToScreenPos0. Because the inverse is linear and camera-relative, patching
uScreenToWorldPos0 = inverse(patched worldToScreenPos0) and uPrevWorldToScreenPos0 =
patched worldToScreenPos0PrevFrame makes the velocity pass reconstruct world positions
in the jittered projection, producing motion vectors that are consistent with the
jittered frame - exactly what DLSS requires (guide 3.6.3).
Runtime validation (uTexSize ~= 1/1920,1/992 either order; col3 of uScreenToWorldPos0
~= (0,0,0,1)) rejects stale/mirror copies (observed in the captured ring).

**7.2.5 DLSS injection point**
The engine's per-frame sequence ends with: velocity dispatch 13253 (writes 4092) ->
CopyTextureRegion 4092->5178 -> barrier block. DLSS must read FRESH motion vectors,
so the injection block is recorded AFTER the engine's copy-back (inside the
CopyTextureRegion hook: the real copy is recorded first, then our barriers + DLSS
evaluate + dlssOut->10911 copy + restore barriers). 5178 is in COPY_DEST before the
engine's copy and after it too, so no barrier is needed for the copy itself; our
block then transitions 5178 SRV->COPY_DEST restore, 10888 (depth) COPY_DEST->SRV->COPY_DEST,
10911 SRV->COPY_DEST->SRV, dlssOut UAV->COPY_SOURCE->UAV - all driven by a tracked
state map built from the ResourceBarrier hook. If any resource's state is unknown the
injection is skipped for that frame (retried next frame) - never a device-removal risk.

**7.2.6 dlssOut resource**
Own committed R16G16B16A16_UNORM 1920x992 texture (UAV | COPY_SOURCE | COPY_DEST),
created lazily on first injection, initial COMMON, kept in UAV when idle. Matches
10911's format so the copy is format-identical.

**7.2.7 Descriptor heap save/restore**
D3D12 has NO GetDescriptorHeaps API (verified against the SDK header - the method
does not exist on any interface). Instead we hook SetDescriptorHeaps (cmdlist slot
28) as a pass-through recorder and restore the engine's last-bound heaps after
EvaluateFeature (NGX binds its own heaps). Note: CACHE.md lists OptiScaler's owned
hooks as D3D12CreateDevice + device slots 8/16/19/21/23/25 only; cmdlist slot 28 is
not in that set. If OptiScaler ever also hooks slot 28, MinHook chains through it
(both hooks run in sequence). Not restoring the engine's heaps would risk the next
engine pass drawing with NGX's heap bound - worse failure mode.

**7.2.8 DLSS feature parameters**
- Width/Height = 1286/664 (render res), OutWidth/OutHeight = 1920/992.
- MVJittered = 1 (velocity is captured with the jittered projection).
- MVLowRes = 0 (full-res velocity).
- DepthInverted = 0 (depth is linear, clip.z = -0.00134*wz + 0.20027, near=0.2 far=150).
- IsHDR = 0 (10911 is LDR UNORM16).
- AutoExposure = 1 (flag 1<<6) - configurable.
- PerfQualityValue from ScaleNG.ini (default 1 = Balanced; 5 = DLAA is REJECTED by
  config because DLAA is out of scope).
- InvViewProjectionMatrix / ClipToPrevClipMatrix NOT passed: the official DLSS
  Programming Guide (31 Mar 2026, sections 5.3/5.4) does not pass them at all, and
  they are optional via the classic NGX parameter map. Our reconstruction is based
  on uScreenToWorldPos0 instead (guide's recommended motion-vector-only path).
- Reset = 1 on the first evaluate.
- Jitter.Offset.X/Y = render-res pixel jitter (same values as the NDC patch, since
  DLSS input is the render-res region).

**7.2.9 Depth discovery**
10888 (the depth copy target) is discovered at runtime: the last full-res 2D->2D
copy whose dst is not the MV resource, the scene color or dlssOut is the depth
candidate (this is the engine's per-frame 3051->10888 depth copy). A fallback
candidate comes from full-res single-mip float SRV creation (device slot 18,
allowed - not in OptiScaler's set).

**7.2.10 Scene color and MV discovery**
Device slot 20 (CreateRenderTargetView, allowed): the first full-res
R16G16B16A16_UNORM RTV = scene color (records its CPU descriptor handle - the viewport
patch only applies when that exact handle is bound via OMSetRenderTargets, and the
post-injection exposure/bloom passes into 10911 therefore run UN-patched at full res);
the first full-res R16G16_FLOAT RTV = the motion-vector resource (1920x992).

**7.2.11 Coexistence with OptiScaler**
OptiScaler (Detours) owns D3D12CreateDevice and device slots 8/16/19/21/23/25
(CACHE.md). We use only device 17/18/20, queue 10 and cmdlist 15/16/21/22/26/28/46.
Our D3D12CreateDevice detour (MinHook) is installed on the ORIGINAL export; if
OptiScaler's Detours hook is present, MinHook relocates the JMP and chains through it.
If installation fails at any point the plugin logs it and stays inert.

**7.2.12 Fail-safe philosophy**
Every runtime observation is content-validated before acting (matrix w-rows +-1,
finite values, sane projection params, uTexSize reciprocity, descriptor-handle
equality, full-res copy boxes). Any failed validation leaves the frame untouched and
logs one line. The plugin cannot corrupt rendering by design; at worst DLSS does not
run (visual = engine default at full res).

### 7.3 Known limitations / residual risks

- Resolution hardcoded to the captured 1920x992 render: discovery requires
  Width==1920 && Height==992. Other resolutions -> plugin inert (logged).
- Multi-camera/mirror setups write several camera CB variants per frame; all
  validated copies are patched uniformly (live-coefficient formulas), but a mirrored
  view under DLSS can show edge ghosting (jitter+reprojection on a mirrored camera).
- If OptiScaler updates its hook set to include cmdlist slot 28, heap-restore still
  works through MinHook chaining (both hooks pass through).
- The game must run windowed at 1920x1080 for DLSS 1920x992 output to be displayed
  1:1 (the engine presents 1920x992 in a 1080p window).
- In-game TAA (if any is enabled) must be off - never combine engine TAA with DLSS.

## 8. Verification guide (in-game)

Steps to check the plugin works, in order. Run the game windowed at 1920x1080 with
`fullscreen resolution 1920x1080` in BeamNG (the render target is 1920x992).

### 8.1 Install

1. Copy `dist\ScaleNG.asi` and `dist\ScaleNG.ini` into the game's `plugins\`
   folder (the one OptiScaler's ASI loader scans; see next step).
2. OptiScaler config: copy `User\OptiScaler.ini` from this repo next to the game's
   `dxgi.dll` (Bin64). It already has `LoadAsiPlugins=true`, `Path=plugins`,
   `EnableDlssInputs=false` (keeps NVNGX calls in ScaleNG's hands — see §7.2.11),
   menu enabled (Insert), file logging on (Info).
2. Put a `nvngx_dlss.dll` (NVIDIA DLSS SDK) into the same `plugins\` folder. If the
   driver's `nvngx.dll` exists in System32 it is used as a fallback. ScaleNG never
   downloads anything - supply the DLL yourself.
3. In-game: disable any TAA / AA setting. Never run engine TAA together with DLSS.
4. Start the game. A file `plugins\ScaleNG.log` should appear next to the .asi.

### 8.2 Log lines to expect (ScaleNG.log)

| Stage | Log line |
|---|---|
| Load | `ScaleNG.asi loaded` ... `config: renderScale=0.67 ...` |
| Device | `hooks: D3D12CreateDevice detour installed` |
| Device created | `hooks: device 0000000000000000 created` |
| Resources | `hooks: scene color RTV 0000000000000000 (1920x992 R16G16B16A16_UNORM)` |
| | `hooks: motion vector RTV 0000000000000000 (1920x992 R16G16_FLOAT)` |
| | `hooks: depth candidate ... (full-res copy)` (once per frame is normal, plus one SRV line) |
| Command list | `hooks: command list slots 15/16/21/22/26/28/46 hooked` |
| Each frame | `hooks: frame N started (render 1286x664, jitter ...)` |
| | `hooks: camera CB patched in place` (2x per frame) |
| | `hooks: velocity CB patched in place` (1x per frame) |
| | `hooks: DLSS injection recorded for frame N` (1x per frame) |
| DLSS init | `DLSS: loaded plugins\nvngx_dlss.dll` and `DLSS: feature created (render 1286x664 -> display 1920x992)` |

If you see `injection skipped (viewport patch 0, ...)` or `DLSS evaluate failed`,
copy the log and report it - the plugin stayed inert by design (no visual damage).

### 8.3 A/B test

1. With the plugin active, play a fixed scene (same camera, same spot): record FPS
   (e.g. `F9` built-in FPS overlay) and note the sharpness.
2. Edit `ScaleNG.ini`, set `enabled=0`, restart, play the same scene. Compare:
   - FPS should be higher with DLSS (lower render resolution).
   - Image quality: with DLSS 0.67 it should be close to native 1920x992, with
     slight softness on thin detail (fences, foliage). Increase `sharpness` to
     0.2-0.5 if too soft.
3. Different scales: `scale=0.5` (more FPS), `scale=0.8` (higher quality).
4. Jitter sanity: stop the car in a fixed camera; the image must be stable (DLSS
   temporal stability). Heavy flicker on a still image = a validation problem -
   capture the log.

### 8.4 Perf expectations

- 1920x992 at 0.67 = 1286x664 rendering (~46% of the pixels).
- DLSS adds a small fixed cost per frame (temporal network pass at 1920x992).
- Net result is usually a 1.5-2.5x frame-time improvement in GPU-bound scenes.

### 8.5 Troubleshooting

| Symptom | Cause / action |
|---|---|
| No ScaleNG.log | ASI not loaded - check OptiScaler `LoadAsiPlugins=1`, plugin folder, file name `ScaleNG.asi`. |
| Log stops at "detour installed" | No D3D12 device was created through the export while the hook was up (rare) - or MinHook failed (log says inactive). Check the last lines. |
| `D3D12CreateDevice hook failed` | MinHook could not chain OptiScaler's hook. Report it - nothing else helps, the plugin refuses to run (safe). |
| `DLSS: loaded` missing | `nvngx_dlss.dll` not found in plugins and no System32 nvngx.dll. Supply the DLL. |
| `camera CB copy not validated` every frame | Validation is stricter than the engine's actual values - capture the log for analysis. |
| `DLSS: CreateFeature failed, result=N` | NGX rejects the configuration - check the log for the result code. |
| Crash / device removal | If anything crashes, the log always shows the last hook action - report it. |

### 8.6 When you change code

Run `src\build.bat` (double-click). It rebuilds `dist\ScaleNG.asi` using the
installed Visual Studio Build Tools. The log and the .asi must be replaced in the
game's plugin folder after each build.

## 9. Change log

| Date | Change | Why |
|---|---|---|
| — | v0.0 ASI skeleton + build.bat | Verify ASI load path; zero-risk first milestone |
| — | New 2-frame PIX capture registered & parsed | Fresh ground truth for verification |
| — | Parser patches (cpp_export.py, resources_bin.py) | Make PSO shader-stage extraction and per-PSO resource chunks work |
| — | Nemotron's report verified & **refuted** (Res 9 slots = shadow probe, not camera) | Wrong report would have sent the whole DLSS design off-course |
| — | Main-scene architecture discovered & verified (10911/3051/12672, RS 12681) | The actual render target the upscaler must hook |
| — | Camera CB layout decoded from PSO 12685 VS disassembly | Named fields + offsets; jitter identified in projection translation |
| — | v0.2 design decided (middle ground: accept layout, runtime discovery) | User decision; avoids over-investing in static decoding |
| 2026-08-19 | Phase 0 research complete (depth, color space, DLSS/FSR2 APIs, hook coexistence) | Removed all design-blocking unknowns |
| 2026-08-19 | Full DLSS plugin implemented + built (per NEMOTRON_PROMPT.md) | Render-scale + jitter + DLSS evaluate, viewport-patch approach |
| 2026-08-19 | Final fixes after first clean build | Frame-start fallback removed, cmdlist heap recorder, device riid flexibility |
| 2026-08-19 | Repo consolidated (src/dist/docs/research) | Standard layout; build.bat self-contained |
| 2026-08-19 | DESIGN_NOTES.md + VERIFY.md merged into this README (§7, §8) | Single docs file per user request |