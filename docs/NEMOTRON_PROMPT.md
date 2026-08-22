# NEMOTRON 3 ULTRA — TASK PROMPT: ScaleNG.Drive — full DLSS integration for BeamNG.drive DX12

> **Note (2026-08-19):** this prompt was executed; the deliverables `DESIGN_NOTES.md` and `VERIFY.md`
> it mentions below were merged into `docs\README.md` (§7 design notes, §8 verification guide) and deleted.

Copy everything between the lines below into a NEW chat. Do not truncate it. You have web access and local tool access (read/grep/bash over the project files, and the pix-mcp tools to analyze the GPU capture). Verify anything you doubt from the sources named; do not silently guess.

---

```
You are writing a complete, production-quality C++ ASI plugin that integrates NVIDIA DLSS (render-scale upscaling,
no frame generation) into BeamNG.drive (DX12 renderer, v0.39, on NVIDIA RTX hardware, Windows 11, VS2022 toolchain).

The user has no coding background. Your job: deliver complete, compilable code files + a build script + a short
verification guide. Do not write pseudo-code, do not leave TODO stubs. Every function must be fully implemented.
If a decision is ambiguous, pick the safest option and note it in a DESIGN_NOTES.md.

You are allowed (encouraged) to:
- Read the GPU-capture C++ export (path below) and the .cso shader dumps to verify every fact you rely on.
- Fetch NVIDIA DLSS SDK (NVSDK_NGX) headers from https://github.com/NVIDIA/DLSS (branch main; the headers are
  sdk/include/nvsdk_ngx.h, nvsdk_ngx_defs.h, nvsdk_ngx_helpers.h, nvsdk_ngx_params.h) and the FidelityFX FSR2
  headers from https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (only if you implement the FSR2 path).
- Ask the user for the nvngx_dlss.dll binary only via instructions, not by trying to download it.

--------------------------------------------------------------------------------
1. HOW THE PLUGIN LOADS (non-negotiable, already decided)
--------------------------------------------------------------------------------
- Loader: OptiScaler's ASI loader. Deploy: copy OptiScaler as dxgi.dll into BeamNG Bin64\; create Bin64\plugins\;
  set LoadAsiPlugins=1 in d3d11.ini [RENDERER]. Our plugin compiles to plugins\ScaleNG.asi.
- ASI contract: extern "C" __declspec(dllexport) void InitializeASI(); (called at load) and
  extern "C" __declspec(dllexport) bool PatchResult(const char* patchId, void* param); (optional).
  Use _CRT_SECURE_NO_WARNINGS, /MT (static CRT), /O2, C++17 or C++20.
- HOOK COEXISTENCE (critical, verified): OptiScaler uses Microsoft Detours and already hooks D3D12CreateDevice
  and ID3D12Device vtable slots 8, 16, 19, 21, 23, 25. Our ASI must NEVER hook those. We hook exactly:
    * ID3D12Device::CreateGraphicsPipelineState      (vtable slot 14)
    * ID3D12Device::CreateComputePipelineState       (vtable slot 13)
    * ID3D12CommandQueue::ExecuteCommandLists        (vtable slot 8)
  Because the ASI loads after OptiScaler's hooks are installed, the device/queue pointers we receive are the real
  underlying objects. Hook library: MinHook (BSD-2-Clause, single-header use or prebuilt .lib; if you vendor it,
  include the minhook source). Alternatively use OptiScaler's own MinHook? No — use plain MinHook, it's fine.
  Do NOT detour exported d3d12.dll functions; vtable swap on the above three methods only.

--------------------------------------------------------------------------------
2. GROUND TRUTH FROM GPU CAPTURE (verified; trust these, don't rediscover)
--------------------------------------------------------------------------------
Capture + full C++ replay export:
  C:\Users\Admin\Documents\Default Project\ScaleNG.Drive\research\5 2 frame capture\
  (GPU 1.wpix and the exported .cpp files + saved .cso shader dumps live there)

Frame structure (2 frames; Present at GlobalId 7673 / 15512; 107,270 D3D12 calls):
- MAIN SCENE (terrain + everything): RTV = resource 10911 (R16G16B16A16_UNORM, 1920x992, ALLOW_RENDER_TARGET,
  clear 0 — i.e. LDR, NOT HDR), DSV = 3051 (D24_UNORM_S8_UINT), stencil ref 14, viewport 1920x992.
  PSOs 12675..12716, root signature 12681 (8 root params). Native res; there is NO internal render scale today.
- CAMERA CONSTANT BUFFER: resource 12672 (1792B resource, 1616B used) = struct RenderPassConstBuffer,
  bound as cb0/space2 at root param 2. Per frame it is rewritten via ID3D12GraphicsCommandList::CopyBufferRegion
  (1616 bytes) from a CPU ring buffer (resource 9) — so in-game the bytes appear suddenly, NOT via Map on 12672.
  Verified struct layout (offsets are GROUND TRUTH from shader disassembly; field names from the struct decl):
    0        ambientSH9[9] (9 x float4)
    224      worldToCamera          (view matrix; col3@272 = camera world position, e.g. 797.6/3382.9/70.3)
    288      worldToCameraPos0      (origin-rebased view; col3 = 0,0,0,1)
    352      worldToScreenPos0      (jittered viewproj; col3@400 = jitter pixels, e.g. -0.1565/0.1354/1.0002/1.0)
    416      cameraRemainder
    432      cameraToScreen         (PROJECTION; col3@480 = jitter in last 16 bytes = translation column)
    496      viewProj               (view*proj, col3@544 = camera pos0 coords)
    560      vEye
    576      eyePosWorld            (CONTENT IS THE SUN POSITION in the capture, NOT the eye — trust offsets only)
    592      eyeMat
    672      clipPlane0
    688      projectionParams       (zFar = 6060.7)
    704      viewportParams
    768      LightDataPSSM          (cascade shadows, irrelevant to us)
    1184     viewProjPrevFrame      (ENGINE ALREADY TRACKS PREV-FRAME MATRIX — populated each frame)
    1248     worldToScreenPos0PrevFrame (prev-frame jittered VP — populated)
    1312     prevEyePosWorld
    1344     lastFrameExposure      (~0.01)
    1360     fogSunDirection
  Terrain VS (PSO 12685, saved as pso12685_VS.cso): SV_Position = cameraToScreen (bytes 432..495) * pos0,
  where pos0 = world position rebased to camera origin via worldToCameraPos0 (bytes 288..351). So:
  * To implement render scale: widen cameraToScreen (432..495) by 1/scale in X/Y while it is in flight.
  * Jitter: engine already writes its own TAA jitter into bytes 480..495 (translation column) each frame —
    we replace it with a Halton jitter in pixels (DLSS convention) and feed the same value to DLSS.
- DEPTH CONVENTION (verified via velocity compute PSO 13253, saved pso13253_CS.cso): LINEAR depth.
  It reconstructs worldPos0 = uScreenToWorldPos0 * (ndcX, ndcY, depth, 1) then divides by w.
  DLSS DepthInverted flag = 0. FSR2 DEPTH_INVERTED = off. Depth buffer 3051 contains linear Z.
- COLOR: scene color is R16G16B16A16_UNORM (LDR 0..1, gamma-ish). HDR lives in 10062 (R16G16B16A16_FLOAT)
  only AFTER the composite. So DLSS input color = UNORM16 → set IsHDR=0 and consider AutoExposure=64 flag
  (DLSS then handles exposure; engine exposure ~0.01 in lastFrameExposure@1344).
- VELOCITY (verified): compute PSO 13253 (8x8 threads) reads depth 3051 + its own CB 13233 (176B:
  uTexSize@0, uScreenToWorldPos0 @16 = INVERSE viewproj, uPrevWorldToScreenPos0 @80, uParams @144,
  uCurrMinusPrevCamPos @160) → UAV 4092 → copy → resource 5178 (R16G16_FLOAT, ALLOW_RENDER_TARGET, 1920x992).
  Output convention: screen-space [0,1] deltas (prev NDC*0.5+0.5 minus current pixel UV), magnitude clamped to 0.5.
  For DLSS: motion vectors must be in pixels/frame at render res → scale the [0,1] deltas by (renderW, renderH)
  (i.e. MV.Scale.X = 1/renderW etc. ONLY if DLSS treats raw values as pixels — verify from DLSS docs/headers;
  if DLSS accepts NDC-style, scale to NDC instead. Document the choice.)
- COMPOSITE (verified): combine PSO 13413 (RS 13414) samples 3 textures inputTex0 t0 (the scene color),
  inputTex1 t1, blendTex t2 → writes HDR 10062 (R16G16B16A16_FLOAT) → blit PSO 13397 → LDR 227
  (R8G8B8A8_UNORM) → copy → 10901 → swapchain backbuffer (13387/13873). SKY is drawn into 10062 at
  GlobalId 3452 (PSO 233/RS 234) BEFORE the combine. So the correct DLSS injection point is AFTER the last
  scene draw, BEFORE the combine: replace inputTex0 (scene) with the DLSS native-res output.
  The combine PSO's inputTex0 is bound via a descriptor heap table — we must replace the descriptor for the
  scene-color SRV with our DLSS-output SRV (both R16G16B16A16; our output should be R16G16B16A16_FLOAT and the
  combine shader likely just samples it — verify pso13413_PS.cso and adjust: if the combine expects UNORM16
  range, either create our DLSS output as UNORM16 (DLSS can output UNORM16) or add a tiny copy to a UNORM16
  buffer. Decide from the shader and note it.)
- CAUTION: in the capture's exported "initial resource data", several fields are stale/wrong (ring buffer 9
  contains NaN in the frame-2 slot; some struct fields hold sun values). NEVER trust initial data at runtime —
  the plugin must VALIDATE the camera CB it captures: orthonormal rows in worldToCamera@224, |col3@272| in
  world units (100..10000), zFar ~6000 at 688, jitter-like values in the translation column of cameraToScreen.
  If validation fails, treat the buffer as NOT the camera CB and ignore it (zero-trust).

--------------------------------------------------------------------------------
3. DLSS / NVSDK_NGX API FACTS (verified from NVIDIA DLSS SDK; latest NGX runtime 310.7.0)
--------------------------------------------------------------------------------
- Headers: nvsdk_ngx.h, nvsdk_ngx_defs.h (NVSDK_NGX_VERSION_API_MACRO 0x0000015, calling convention __cdecl).
- Load nvngx.dll dynamically via LoadLibraryW(L"nvngx.dll") (it ships with NVIDIA drivers in System32) and
  GetProcAddress for: NVSDK_NGX_D3D12_Init, NVSDK_NGX_D3D12_Init_ProjectID, NVSDK_NGX_D3D12_CreateFeature,
  NVSDK_NGX_D3D12_EvaluateFeature, NVSDK_NGX_D3D12_Shutdown, NVSDK_NGX_D3D12_GetParameters.
- nvngx_dlss.dll (the DLSS feature DLL) does NOT ship with drivers. The user must drop it next to the game exe
  (or in the game Bin64 folder). Tell the user where to get it (NVIDIA DLSS SDK release, or copy from a recent
  NVIDIA Game Ready driver's install; apps also get it via DLSS swapper). Its location must be passed in
  NVSDK_NGX_FeatureCommonInfo::PathListInfo (paths list). Handle absence gracefully: log and disable DLSS,
  fall back to native (do NOT crash).
- Init: NVSDK_NGX_D3D12_Init(appId, dataPath, ID3D12Device*, FeatureCommonInfo*, NVSDK_NGX_Version).
  appId = 0 (personal/local use; NVIDIA docs allow 0 for development). dataPath can be a temp folder.
- Create: NVSDK_NGX_D3D12_CreateFeature(device, params NVSDK_NGX_Parameter*, outInstance, scratch, NVSDK_NGX_DLSS_Create).
  Set params via NVSDK_NGX_GetParameters then NVSDK_NGX_ParameterSetI/U/I etc. (helper macros NVSDK_NGX_Parameter_*).
  DLSS create params: Width, Height = render res; OutWidth, OutHeight = display (native) res;
  PerfQualityValue: 0=MaxPerf,1=Balanced,2=MaxQuality,3=UltraPerf,4=UltraQuality,5=DLAA (we use 0..4, never 5);
  D3D12.Feature.Creat.Flags (DLSS-related flags: IsHDR=1, MVLowRes=2, MVJittered=4, DepthInverted=8,
  DoSharpening=32 deprecated, AutoExposure=64), D3D12.Feature.Creat.ScratchMemory, D3D12.Feature.Creat.Contexts.
  Output format: DLSS supports R16G16B16A16_FLOAT and R16G16B16A16_UNORM output textures.
- Evaluate each frame: NVSDK_NGX_D3D12_EvaluateFeature(g_commandList, instance, params, scratch, NVSDK_NGX_DLSS_Evaluate).
  Evaluate params: Color (input SRV), Output (UAV/RTV), MotionVectors, Depth, Jitter.Offset.X/.Y (PIXELS at
  render res), MV.Scale.X/.Y, MV.Offset.X/.Y, InvViewProjectionMatrix, ClipToPrevClipMatrix, Sharpness,
  ExposureTexture (optional; with AutoExposure flag it's unused).
  InvViewProjectionMatrix = inverse of (cameraToScreen_unjittered * worldToCamera) — i.e. from our patched
  matrices WITHOUT jitter. ClipToPrevClipMatrix = viewProjPrevFrame@1184 * inverse(viewProj_current_unjittered)
  (engine gives us the prev matrices already — use them, do NOT cache your own).
- NGX is NOT thread-safe: all NGX calls from the same thread; save/restore the current root signature, PSO,
  descriptor heaps (CBV/SRV/UAV + sampler) around EvaluateFeature; DLSS needs its own descriptor heap space
  (create a heap big enough and pass via parameters). After Evaluate, restore the bound state EXACTLY.
- The DLSS evaluate must run inside our hooked ExecuteCommandLists path: inject the Evaluate on the SAME
  command list right after the last main-scene draw and before the combine draws. The scene draws are on the
  direct/graphics queue with PSOs 12675..12716 and the combine PSO 13413 — detect the combine PSO (3 SRVs,
  1 RTV, renders into FLOAT 1920x992) as the "last scene frame" marker, or inject before the first
  SetGraphicsRootSignature that matches RS 13414. Implement marker detection that does not rely on PSO ids
  (content-based: PSO that has 3 root SRV tables + samples t0/t1/t2 into an R16G16B16A16_FLOAT RTV = combine).
- ExecuteIndirect is NOT used for the scene; draws are DrawIndexedInstanced.

--------------------------------------------------------------------------------
4. ARCHITECTURE (follow this; it is the agreed design)
--------------------------------------------------------------------------------
- IUpscaler interface (pure virtual): Init(device, renderW, renderH, displayW, displayH, flags),
  Evaluate(cmdList, colorSRV, depthSRV, motionVectorsSRV, jitterPixelsXY, invViewProj, clipToPrevClip,
  outputUAV), Shutdown(). Implement NvDlssUpscaler now. Do NOT implement FSR2 in this task (later phase) —
  but keep the interface FSR2-shaped (depth+MV+jitter only, no matrices needed by FSR2).
- Content-based discovery, never hardcoded IDs:
  * Scene color = R16G16B16A16_UNORM ALLOW_RENDER_TARGET, dims == swapchain client dims, cleared to 0,
    written by many PSOs. Track via CreateCommittedResource/CreatePlacedResource interception.
  * Depth = D24_UNORM_S8_UINT ALLOW_DEPTH_STENCIL same dims.
  * Velocity = R16G16_FLOAT, dims == render res, written by a compute then copied (accept either 4092-like
    UAV or 5178-like RT — the descriptor we sample just needs the resource).
  * Camera CB = buffer 1616..1792B whose content validates as camera struct (rules above), updated by
    CopyBufferRegion of 1616 bytes. Hook CopyBufferRegion on the command list vtable? NO — command list
    vtable slot for CopyBufferRegion is risky with OptiScaler; instead hook ExecuteCommandLists and SCAN the
    command list's recorded calls is not possible in D3D12. Correct approach: also vtable-hook
    ID3D12GraphicsCommandList::CopyBufferRegion (slot 15) — CHECK: OptiScaler hooks are on the DEVICE and
    D3D12CreateDevice, not on command lists, so command-list vtable swap of CopyBufferRegion is safe.
    Validate & copy the 1616-byte payload to a shadow buffer each time it validates as camera; this gives us
    per-frame cameraToScreen@432 with jitter@480 plus viewProjPrevFrame@1184 for ClipToPrevClip.
- Render scale implementation (the actual perf win):
  * Config ScaleNG.ini: [Main] enabled=1, upscaler=dlss, scale=0.67 (or quality preset), jitterPattern=halton,
    logLevel=2. Default scale 0.67 (= Balanced); scale values 0.5/0.67/0.75 supported.
  * When the scene color RT is created at native WxH, do NOT shrink it (engine allocates it once; shrinking
    at creation breaks the half-res composite chain assumptions). INSTEAD: allocate our own low-res
    R16G16B16A16_UNORM RT (renderW=ceil(W*scale), renderH=ceil(H*scale)) and redirect: swap the heap/descriptor
    the engine uses? NO — simplest robust approach: patch the viewport/scissor to low-res on scene draws AND
    make the engine's RTV a low-res VIEW: intercept CreateRenderTargetView for the scene color resource and
    pass a low-res description when the resource matches scene-color and the view dims == native dims
    (first such RTV). The engine re-creates RTVs rarely; verify in capture how many CreateRenderTargetView
    calls reference 10911 (inspect export). Same for DSV (3051) and velocity (5178 RT view / 4092 UAV view
    gets low-res dims too — but keep the velocity COMPUTE dispatch dims consistent: patch Dispatch dims via
    command-list? The velocity compute dispatches 240x124 for 1920x992; with low-res we must patch Dispatch
    (240*scale, 124*scale) — intercept ID3D12GraphicsCommandList::Dispatch (slot 9) and scale when the PSO is
    the velocity compute (content: 1 SRV depth + 1 UAV R16G16_FLOAT + 176B CB). Simpler alternative: keep
    velocity at FULL res (compute cost is tiny) — decide based on effort; note in DESIGN_NOTES.md.
  * Patch cameraToScreen bytes 432..495 each frame in our shadow CB: multiply the X/Y scale terms by 1/scale
    and overwrite the jitter translation column (bytes 480..495) with Halton jitter (2D, base 2/3, first few
    values off, jitter range ±0.5 pixel at RENDER res; clamp to jitterScale from DLSS query
    NVSDK_NGX_DLSS_GET_STATS / jitterScale param if exposed — DLSS returns recommended jitter scale via
    NVSDK_NGX_DLSS_GET_RECOMMENDED_JITTER_SCALE? use NVSDK_NGX_ParameterSetF params). Apply the same patch
    to worldToScreenPos0 @352 (bytes 352..407) so the engine's velocity/TA-sampling stays consistent.
    How to make the engine SEE the patched bytes: hook CopyBufferRegion, and when dst == our camera CB and
    src==ring buffer: allocate our own copy of the ring buffer region, patch it, then WRITE THE PATCHED
    BYTES to the dst instead (replace the copy with our patched data). This is the clean injection: we
    shadow the 1616 bytes, patch, and forward the patched copy. (The camera CB resource must be CPU-writable?
    No — CopyBufferRegion is GPU-to-GPU; we cannot easily write patched GPU bytes without a staging upload.
    SOLUTION: we CREATE our own small upload buffer + a command-list CopyBufferRegion from upload→camera-CB
    queued right after the engine's copy on the same list, and SKIP/let the engine's copy land first, then
    overwrite. We have an upload heap for this. Because we hook ExecuteCommandLists, we can record our copy
    on the same list after the engine's. THIS IS THE CRITICAL MECHANISM — get it right.)
  * RSSetViewports/RSSetScissorRects interception: patch to low-res ONLY when the current PSO is a main-scene
    PSO (content-based: PSO whose VS reads cb0/space2 rows 18-21 and 27-30 — i.e. RenderPassConstBuffer, AND
    whose RTV is our scene color). Intercept ID3D12GraphicsCommandList::RSSetViewports (slot 11) and
    RSSetScissorRects (slot 12) — check OptiScaler does not hook command lists (it hooks device + creation).
  * DLSS input textures at render res: color = our low-res scene RT, depth = low-res DSV view, MV = low-res
    velocity. Output = native-res R16G16B16A16 (FLOAT or UNORM per combine-shader requirement).
- Descriptor swap at combine: hook ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable (slot 17?) —
  verify slot. When the bound root sig is the combine RS (content: 3 SRV ranges + 1 RTV + uses cb?), and the
  SRV in range 0 slot 0 points at our low-res scene color — replace it with our native-res DLSS output SRV.
  Descriptor-table root params are GPU descriptor heap indexes: we copy the engine's table into OUR heap with
  the first entry swapped, and call the real SetGraphicsRootDescriptorTable with our heap's GPU handle
  (descriptor copy via CopyDescriptors). Alternatively, if the engine binds the scene color via
  SetGraphicsRootShaderResourceView/SRV root descriptor — check the capture (grep SetGraphicsRootDescriptorTable
  vs SetGraphicsRootShaderResourceView near combine PSO 13413) and use whichever exists. NOTE: SRV root
  descriptors need the GPU VA of a view — our DLSS output resource's SRV descriptor must be created and its
  GPU handle used.
- TAA: the engine's TAA jitter is in the camera CB (bytes 480..495). We REPLACE it with our Halton jitter —
  this effectively disables engine TAA's temporal accumulation inputs. ALSO the engine may have a TAA pass
  (check export for a TAA PSO near the scene; if found, document whether it must be disabled in-game).
  In ScaleNG.ini document: disable in-game TAA/AA settings. Never run engine TAA + DLSS simultaneously.
- Robustness: every hook wraps its body in __try/__except(EXCEPTION_EXECUTE_HANDLER); any validation failure
  → log + return original. Logging: ScaleNG.log in Bin64 (timestamps, decisions, matrix sanity values,
  DLSS init results, frame numbers). No crash paths: if nvngx.dll or nvngx_dlss.dll missing → log + disable.
  Handle: window resize/fullscreen toggle (recreate low-res RTs + DLSS feature), device lost not handled
  (BeamNG handles it; we just re-init DLSS on new device via hook re-entry).

--------------------------------------------------------------------------------
5. DELIVERABLES (write these files)
--------------------------------------------------------------------------------
  src/main.cpp                  - ASI entry points, init, config load, logging, MinHook setup
  src/d3d12_hooks.cpp/h         - the vtable swaps (device 13/14, queue 8, commandlist: CopyBufferRegion,
                                  RSSetViewports, RSSetScissorRects, Dispatch?, SetGraphicsRootDescriptorTable?)
                                  + resource tracking + validation logic
  src/camera_cb.cpp/h           - camera CB shadowing, validation, patching (scale + jitter), matrix helpers
  src/upscaler.h                - IUpscaler interface
  src/dlss_ngx.cpp/h            - NVSDK_NGX loading, init, feature create/recreate, per-frame evaluate,
                                  root-sig/PSO/heap save-restore around Evaluate
  src/minhook/ (vendored MinHook source) - OR instruct to clone; if vendoring, include all files
  src/build.bat                 - builds with cl.exe (VS2022): /std:c++17 /O2 /MT /EHsc /LD, links minhook,
                                  output dist\ScaleNG.asi
  DESIGN_NOTES.md               - every decision you made and why (esp. MV scaling choice, velocity res,
                                  descriptor swap method, UNORM vs FLOAT DLSS output, TAA handling)
  VERIFY.md                     - what to check in-game (log lines to expect, A/B test steps, perf)

--------------------------------------------------------------------------------
6. CONSTRAINTS / ANTI-PATTERNS (violating these = failure)
--------------------------------------------------------------------------------
- NEVER hook: D3D12CreateDevice, device slots 8/16/19/21/23/25 (OptiScaler owns them).
- NEVER rely on resource IDs / PSO ids from the capture in the plugin (capture-specific). Content-based only.
- NO frame generation. NO DLAA (PerfQualityValue 5 is forbidden). Render scale ONLY.
- Do not copy code from OptiScaler (GPL-3.0). MinHook (BSD-2-Clause) and NVSDK_NGX headers (NVIDIA EULA,
  fine for personal use) are OK. FSR2/FidelityFX (MIT) OK if ever needed.
- Do not use the initial-data content of the capture as ground truth for runtime values (stale/NaN).
- The plugin must do nothing (no-op) if: config disabled, no NGX, validation fails, non-scene workloads.
- Keep per-frame overhead minimal: no allocations per frame (preallocate), no file I/O per frame (log at most
  a line per new state change, counters for frame count).
- Match BeamNG v0.39 DX12; do not assume D3D11. Agility SDK version unknown — use plain D3D12 headers
  (d3d12.h from Windows SDK) so it links against whatever d3d12.dll BeamNG uses.

--------------------------------------------------------------------------------
7. BEFORE YOU FINISH — verify these from the export (paths above) and note results in DESIGN_NOTES.md
--------------------------------------------------------------------------------
- How many times is CreateRenderTargetView called for 10911-like resources (per-frame or once)?
- Are main-scene textures bound via SetGraphicsRootDescriptorTable or SetGraphicsRootShaderResourceView?
- Does the combine PSO 13413 expect UNORM16 or FLOAT input (pso13413_PS.cso)?
- Is there a separate TAA pass PSO? (grep export for PSOs between scene end and combine, ~GlobalId 5000-7600)
- What exact slots do we need for RSSetViewports / RSSetScissorRects / Dispatch / CopyBufferRegion /
  SetGraphicsRootDescriptorTable on ID3D12GraphicsCommandList (verify against d3d12.h from the Windows SDK)?
- Confirm nvngx.dll exports used by name with GetProcAddress (check the SDK header for exact spellings).
- Confirm whether DLSS MV input convention needs pixels (and MV.Scale = 1) or NDC (MV.Scale = 1/renderRes),
  and whether jitter must be at render res or display res (docs/headers say render res pixels).
- If the velocity buffer is used by the combine or later passes at full res, note that our low-res velocity
  must be upsampled for those passes OR those passes must sample our low-res — decide and document.

Write the complete code now. Output each file's full content in your final message (or write them to disk if
you have file tools), plus DESIGN_NOTES.md and VERIFY.md. Be precise, be complete, no stubs.
```