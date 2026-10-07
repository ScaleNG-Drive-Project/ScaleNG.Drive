# Current project status and agenda

**Last verified: 2026-10-04.** This page is the current-status source of truth. Update it when a new test or implementation result changes the agenda. Older dated notes elsewhere are historical evidence, not current status.

## Latest DLSS investigation

- Prior run `20261002T202302Z` reached freeroam and logged camera-constant-buffer
  candidates from `CopyBufferRegion`. The useful candidate at source offset
  `215349760` had projection/far-plane parameters `0.10, 12500, -0.00, 0.10`.
- `ValidateCameraCb` capped that far-plane field at `10000`, which definitively
  rejected this candidate despite its other logged layout checks matching. The
  upper sanity bound is now `100000` in `src/camera_cb.cpp`.
- Rebuilt and reran via `scripts\launch_test.bat --duration 30`; report:
  `logs/test_runs/20261002T202912Z/result.json`. Build/deploy/gameplay/plugin
  initialization and live Present activity passed; no fatal markers. However,
  this run logged no `Shim_CopyBufferRegion` calls, no camera-CB validation or
  patch records, and no DLSS injection markers. Thus the bound change is a
  justified correction but is **not yet verified as sufficient** to advance
  DLSS in the current hook path.
- Added `tests/camera_cb_validation_test.cpp` and
  `scripts/test_camera_cb.bat` to regression-test the observed 12,500 far-plane
  candidate independently. This unit check tests only buffer validation, not
  command-list interception or on-screen DLSS.
- Do not repeat the previous AI's suggestion to scan inside the 1616-byte copy:
  the logged `srcOff` is the offset within the large upload resource, and the
  earlier hook mapped `src + srcOffset` and evaluated a full 1616-byte region.
- **2026-10-03 update**: Multiple subsequent runs (`20261002T212448Z`,
  `20261002T212917Z`, `20261002T213153Z`, `20261002T213754Z` (crash),
  `20261002T214102Z`, `20261002T214241Z`, `20261002T214415Z`) all reached
  freeroam with vehicle and stable Present activity, but **zero**
  `Shim_CopyBufferRegion` invocations, zero camera-CB validations, and zero
  DLSS injection markers. The game is not calling `CopyBufferRegion` for
  camera-CB updates in these runs, despite the permissive validation logic
  (`numBytes >= kCameraCbSize` at any `srcOffset`) and velocity-CB logic being
  in place. The earlier run `20261002T202302Z` **did** observe
  `CopyBufferRegion` and camera-CB candidates (rejected due to far-plane 12500
  > old limit 10000), proving the hook path works when the game uses it.
  Current blocker: game uses a different camera-CB update path (likely
  `Map`/`Unmap` on a persistent upload buffer) that our hooks don't yet catch.
  Global `Map`/`Unmap` MinHook crashed the game (intercepted `Map` on
  non-mappable DEFAULT-heap resources). Per-resource vtable shims require
  `CreateCommittedResource` hook which isn't firing post-init.

## Phase 1 Diagnostic (2026-10-03T223223Z)

**Correction (2026-10-03 Phase 2a audit):** The Phase 1 report stated per-resource
Map/Unmap slots 10/11. Direct header verification (SDK 10.0.28000.0 C vtable
order: `GetDevice`(7), `Map`(8), `Unmap`(9), `GetDesc`(10),
`GetGPUVirtualAddress`(11); `ID3D12Pageable` adds zero methods; zero
`SetEvictionPriority`/`GetEvictionPriority` matches in this header) proves
**Map=8, Unmap=9**. Slots 10/11 are `GetDesc`/`GetGPUVirtualAddress` and must
not be hooked as Map/Unmap. Phase 2a reverts the shim to 8/9 and keeps it
dormant. Device slots SRV=18, RTV=20, Committed=27 remain correct; slot 26 is
`GetCustomHeapProperties`.

**Changes made to `src/d3d12_hooks.cpp` (Phase 1):**

1. **Fixed vtable slot for CreateCommittedResource**: Device vtable slot 27 (was 26, which is GetCustomHeapProperties). Log now shows `device vtable SWAPPED (18, 20, 27)`.
2. **Per-resource Map/Unmap shim slots (corrected in Phase 2a to 8/9)**: Phase 1 had moved these to 10/11; header proof shows that was wrong. See Phase 2a.
3. **Removed both global MinHook paths (verified still absent in Phase 2a)**:
   - Removed global MinHook attempt in `Hook_CreateCommittedResource`.
   - Removed global MinHook attempt in `InstallResourceShim`.
   - Crash attribution for `20261002T213754Z` remains inconclusive among slot mismatch and shared-vtable effects; removal is retained regardless.
4. **Added diagnostic logging**:
   - `Hook_CopyBufferRegion`: logs ALL copies ≥ 64 bytes with full metadata (src/dst pointers, offsets, byte count, command-list pointer/type, resource descriptors).
   - `Hook_CreateCommittedResource`: logs large buffer creations (≥ 10 MB) with heap type.
   - No Map-time guessed-offset scans, no large DEFAULT resource Map shims, no global hooks.
5. **Hook ordering**: Device vtable swap for CreateCommittedResource (slot 27) now installed in `Hook_D3D12CreateDevice` for the first device.

**Run `20261002T223223Z` results (`logs/test_runs/20261002T223223Z/`):**

| Check | Result |
|-------|--------|
| Device vtable swap | ✅ `(18, 20, 27)` confirmed |
| CreateCommittedResource hook called | ❌ No calls logged (camera ring buffer created pre-hook) |
| CopyBufferRegion invocations | ❌ 0 (game used different code path this run) |
| CopyBufferRegion diagnostic logs | ❌ 0 (no calls) |
| Large buffer creation logs | ❌ 0 (camera ring buffer created pre-hook) |
| Per-resource Map/Unmap shims | ❌ 0 installed (no qualifying buffers created post-hook) |
| Global MinHook attempts | ✅ 0 (both paths removed) |
| Game stability | ✅ PASS (30s, freeroam, vehicle, 0 fatal markers) |
| DLSS injection markers | ❌ 0 |

**Key finding:** The camera ring buffer (~200+ MB) is created **during initial device startup**, **before** our `CreateCommittedResource` hook installs (vtable swap happens after first device creation). The earlier run `20261002T202302Z` showed the game *can* use `CopyBufferRegion` for camera CB updates (96-byte copies), but this run used a different code path entirely (zero `CopyBufferRegion` calls). The camera CB update path is non-deterministic across runs.

**Remaining unknowns:**
1. Exact creation path of camera ring buffer (committed vs placed vs other).
2. Whether ring buffer is persistently mapped (Map once) or mapped per-frame.
4. Why `CopyBufferRegion` path appears in some runs but not others.
5. Whether `CreatePlacedResource` / `CreateReservedResource` paths are used for the ring buffer.

## Phase 2a Diagnostic (2026-10-03T114224Z) — bounded creation-path logging only

**Code changes (`src/d3d12_hooks.cpp` only, no scans/patches/Map hooks):**
- Reverted per-resource shim to verified Map=8/Unmap=9; left `InstallResourceShim` dormant (declaration + definition only, zero call sites).
- Kept `CreateCommittedResource` at slot 27; added diagnostic-only forwarding hooks for `CreatePlacedResource` (29) and `CreateReservedResource` (30) with exact SDK signatures.
- Device swap now `(18, 20, 27, 29, 30)` on first-device vtable only, with shared-vtable guard (`g_hookedDeviceVtbl`; skip second distinct vtable, do not overwrite globals). `Real_*` originals set once (`if (!Real_*)`).
- Added bounded logs: large buffers `Width>=1MB`, first 20 then 1/100 sampling per API. Committed logs heap type (direct param); Placed logs heap pointer + offset only (no heap-type claim); Reserved logs no heap. Added skipped-device counter (first 5 post-capture `D3D12CreateDevice` calls, passthrough unchanged).
- Removed unbounded every-call `Hook_CreateCommittedResource` log.

**Run `20261003T114224Z` (`logs/test_runs/20261003T114224Z/`, `scripts\launch_test.bat --duration 30`, `smallgrid`, freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2525, PASS, 0 fatal, 0 injection):**
- Swap `(18, 20, 27, 29, 30)` on `8042C069D0` confirmed; 3 skipped post-capture device creations logged (passthrough, no hooks).
- `CreatePlacedResource`: 18 large-buffer logs (1.0–11.0 MB, e.g. `w=11010048`, `w=8294400`, `w=4194304`), device `8042C047E0` ≠ swapped `8042C069D0` (shared implementation vtable observed working across device pointers).
- `CreateCommittedResource` large-buffer logs: 0. `CreateReservedResource` logs: 0. `CopyBufferRegion src=` diagnostic logs: 0. Camera CB / shim / injection logs: 0.
- Zero large committed/reserved creations in this run is **inconclusive** among: created pre-hook, created via another device/path, or no such calls (small committed buffers are intentionally unlogged by the ≥1MB cap).

**What remains unknown:** camera-buffer creation API/device/heap/timing; update mechanism in zero-`CopyBufferRegion` runs; why `CopyBufferRegion` appears in 202302Z but not later runs; whether placed buffers above are related (heap pointers only, no content claim).

## Phase 2a Correlation Run (2026-10-03T115437Z) — placed vs copy pointers, same run only

**Logging change (sole code change, `src/d3d12_hooks.cpp`):** expanded `Hook_CopyBufferRegion`
diagnostic from “first + every-1000th ≥64B” to “first 50 ≥32B unconditionally, then
1/1000 sampling plus always-log ≥1024/1616/176.” Bounded (50 + sampling). No Map/Unmap,
scans, patching, or broader hooks.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2645,
PASS, 0 fatal, 0 injection. Artifacts in `logs/test_runs/20261003T115437Z/`.
No overlapping BeamNG process.

**Same-run pointer correlation (addresses differ between runs, never cross-run):**
- Placed large buffers: 18 (1.0–11.0 MB, e.g. `res=C5E6143460 w=11010048`,
  `res=C67E9A19A0 w=4194304`), all on device `C5E05450E0` ≠ swapped `C5E0555160`
  (shared implementation vtable working across device pointers).
- `CopyBufferRegion src=` diagnostic logs: 0 (even with first-50 ≥32B capture —
  zero `Hook_CopyBufferRegion` calls on shimmed lists this run).
- Committed-large / Reserved-large / camera / shim logs: 0.
- **Matches: 0.** No placed `res=` equals any copy `src=`/`dst=` in this run
  because there were no copies to match. This is **absence in one run only** —
  not proof of pre-observation creation, nor of an alternate API. It is equally
  consistent with no buffer-copy updates issued, updates on unshimmed lists,
  or non-copy updates (`WriteToSubresource`, `CopyResource`, Map path unknown).

## Phase 2a Coverage Run (2026-10-03T121356Z) — shim hits vs submissions

**Code change (sole change, `src/d3d12_hooks.cpp`, logging only):**
- `Hook_CreateCommandList`: bounded skipped-creation log (first 10, `device != g_device`), records device, list, type. Filtering/hook behavior unchanged.
- `Hook_ExecuteCommandLists`: bounded per-submission log (first 20, then 1/500) with list pointer, `FindCommandListShim` hit/miss, and type from our shim table when available (no guarded vtable call; `0xFFFFFFFF` when unshimmed). Install/forwarding unchanged. Counters use `InterlockedIncrement`.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2285,
PASS, 0 fatal, 0 injection. Artifacts in `logs/test_runs/20261003T121356Z/`.
No overlapping BeamNG process (tasklist clean pre-run).

**Coverage counts (same run only):**
- Skipped creations (`device != g_device`): 10 logged (cap), all `device=8042DB32B0` vs `g_device=8042DB53C0`, types 0 and 3. At least 10 lists bypassed creation shim.
- Submitted lists: 23000+ ECL records (first 20 + 1/500 sampling); n=1 `shimHit=1 type=3`, all later sampled records `shimHit=0 type=4294967295` (unknown — no shim entry to supply type).
- Shims installed: 55+ `command-list read-only shim installed` records (table size 64, not full).
- `CopyBufferRegion src=` logs: 0. Placed large buffers: 20 (capped log, e.g. `w=11010048`, `w=8294400`, `w=4194304`). Committed-large/Reserved-large/camera/shim/Map logs: 0. Matches: 0 (no copies to match).

**Interpretation (narrowed uncertainty, not proven):** submissions are overwhelmingly
unshimmed at execution time despite 55 installs, including resubmissions of pointers
previously shim-installed (e.g. `8042C1B830`). This is consistent with pointer-identity
mismatch between creation-time `*outList` and execution-time QI'd `cl`, table/recycling
effects, or vtable-identity mismatch — **not** with “no game calls.” Zero shim logs therefore
do **not** prove zero game `CopyBufferRegion` calls. Unshimmed submissions’ device/type
coverage by the DIRECT-only queue hook is also unproven (copy queues never hooked).
If submissions stay unshimmed while placed buffers recur, the narrowest next step is
bounded logging on the next buffer-write API actually present in observed traffic
(`CopyResource(17)` candidate, SDK order-verified) — not retroactive Map shims. Justify
from fresh logs before implementing.

## Phase 2a Coverage Run B (2026-10-03T124011Z) — exact totals

**Code change (logging only):** ECL diagnostic reports exact hits/misses totals; mismatch detail extended to sampled resubmissions (capped); skipped-creation sampling added. Full timeline in git diff.

**Build/test:** PASS, smallgrid freeroam + thePlayer, Present 1-2645, 0 fatal, 0 injection. Artifacts: logs/test_runs/20261003T124011Z/.

**Exact totals:** n=1 hits=1 misses=0, then all misses (final sampled n~37500, hits=1). Skipped creations 10+ (cap). Placed large buffers 20 (cap). Install-then-miss proven on same pointers; first-miss = never-registered, resubmission cause unclassified (cap bug: detail counter exhausted by first submissions).

## Phase 2a Coverage Run C (2026-10-03T125501Z) — corrected sampling, third-value vtables

**Code change (logging only, `src/d3d12_hooks.cpp`):** separate emit counter for mismatch
details (no longer exhausts on first misses); sampled ECL records carry exact
snapshot totals (submissions/hits/misses/ptrReg/ptrUnreg/install ok/dedup/fail);
detail records fire only on sampled misses with a registered pointer, reporting
submitted pointer, registered original/cloned, actual vtable, full-lookup hit flag,
and shim-table type. No forwarding/install/render behavior change.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2645,
PASS, 0 fatal, 0 injection. Artifacts in `logs/test_runs/20261003T125501Z/`.
No overlapping BeamNG process (tasklist clean pre-run).

**Exact totals (counters):** first record `n=1 hits=1 misses=0 ptrReg=0`; final
sampled records `n≈39500, hits=1, misses≈39500, ptrReg≈39474, ptrUnreg=26`,
`instOk=28, instDedup=1, instFail=0`. Skipped creations ≥10 (cap, `device≠g_device`,
types 0+3). Placed large buffers 20 (cap). Committed-large/Reserved/camera/shim-Map
logs 0. `CopyBufferRegion src=` logs 0. Matches 0.

**Decisive detail records (10/10, all `registered=1, fullHit=0`):** actual vtables are
heap addresses near the list object (e.g. `list=E86B14C0F0 actual=E86B14C670`,
`list=E8640514A0 actual=E864051A20`), differing from **both** expected cloned
(`20B4…`, our VirtualAlloc) and expected original (`7FF9E8F53820/4030`, driver).
First-miss records show `registered=0` (never-registered, expected pre-install);
**all resubmission misses are registered-but-mismatched with a third vtable value.**
Not vtable-restored-to-original; not never-registered. Remaining: third-party vtable
writer vs address reuse with stale-entry double-dedup block (`s_hookedLists` +
`Install` early-return both key on pointer alone).

## Phase 2a Coverage Run F (2026-10-03T133157Z) — slot table + first live camera patch

**Code change (logging only, `src/d3d12_hooks.cpp`):** integrity snapshot extended with
6-slot table comparison (Close=9, Reset=10, CopyBufferRegion=15, CopyTextureRegion=16,
ResourceBarrier=26, OMSetRenderTargets=46; SDK order-verified) plus module basename
for differing entries (manual loops, no CRT). Second detail line per capped record.
No install/dedup/forwarding/render change.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2645
(27 snapshots, 1 frame marker), PASS, 0 fatal, 0 injection. Artifacts in
`logs/test_runs/20261003T133157Z/`. No overlapping BeamNG process.

**Slot table (10/10 details):** all slots `O` except 9/10 `B` (never overwritten in
clone, so equal everywhere) — i.e. submitted lists point at driver-original tables,
not our clones. Clones intact. Actual slot 15 identical genuine driver function.
Heap residency = private RW near each object. Same verdict as Run E, now with
slot-level proof that **no hooked slot survives** on submitted lists.

**First live camera-CB patch:** `CopyBufferRegion src=812B9CB610 (256MB) ...
srcOff=141741056 bytes=1616` validated (far plane 12500 accepted by the fixed
bound) → `camera CB ring re-discovered` → `frame 1 started (render 1286x723)` →
`camera CB patched in place (far 12500.0)` → `display size adopted 1920x1080
(render 1286x658/723)`. Scene-color and motion-vector RTVs adopted at 1920x1080;
depth SRV candidates observed. The CopyBufferRegion path is active in some runs
and dormant in others (gameplay nondeterminism confirmed across runs).

**Still missing:** zero NGX init/feature/evaluation/injection markers after the
patch in this run. Next falsifiable step: trace the post-patch gates (MV/depth
readiness, load-phase arming, NGX quiet-frame gate) from code + fresh logs with a
bounded diagnostic if needed — not broader hooks.

## Phase 2a Legacy-Gating Fix (2026-10-03T144936Z) — documented dlaa=0 path made logic-reachable

**Verified issue (config-gate audit, current source):** with shipped `dlaa=0, legacyScale=0`
(`dist/ScaleNG.ini:10,12`, INI untouched by this fix), neither path could reach NGX init:
- `StartFrame` armed `g_patchViewport = !aborted && !dlaa && legacyScale` → always false.
- `CopyTexBody` returned early `if (!bridgeReady || !dlaa)` before the legacy trigger
  requiring `!dlaa && !bridgeReady` — statically dead in one function.
- `Hook_RSSetViewports` passed through unless `bridgeReady && dlaa`, so the sole
  `g_patchAppliedThisFrame = true` could never be set under `dlaa=0`.
- `Hook_ResourceBarrier` skipped tracking unless `bridgeReady && dlaa`, so
  `DoInjection`'s `Barrier()` calls no-op'd under `dlaa=0`.
- `g_bridgeReady` can never become true: only set in `EnsureBridge`, whose call site
  is commented out (single-device architecture). DLAA Present entries all require
  `g_dlaaMode`. `legacyScale` traces to `bd4e94b` as a bridge-era viewport kill-switch
  (no docs, never 1 in any log/ini); `TECHNICAL_REFERENCE` §7.2.1 defines the
  viewport/scissor patch as the legacy mechanism with no flag, and the sibling
  scissor hook stayed patchViewport-only — the bridge gates on the viewport/texture/
  barrier hooks are the anomaly.

**Exact fix (`src/d3d12_hooks.cpp` only, 5 small edits, no hook/vtable/buffer/INI changes):**
- `:1710` arming restored to `!g_patchAborted && !g_dlaaMode` (pre-flag semantics;
  `legacyScale` still parsed, no longer gates; DLAA still disables the patch).
- `:6799` (`CopyTexBody`), `:7027` (`Hook_RSSetViewports`), `:7099`
  (`Hook_ResourceBarrier`): guard changed to `if (g_dlaaMode && !g_bridgeReady) return;`
  — DLAA-without-bridge skips exactly as before, DLAA-with-bridge runs exactly as
  before, legacy (`!dlaa`) now runs the analysis. Each edit carries a LEGACY-COMPAT comment.
- `:7796` config log extended to `config applied (dlaa=%d legacyScale=%d; ...)` for
  build verification (new line observed in-game).
- Deliberately unchanged: INI flags, camera-jitter `dlaa` gate (`ApplyCameraCbJitter`
  still skipped in legacy — render-correctness follow-up, not reachability),
  `OMSetRenderTargets` bind-refresh gate (optimization; creation hooks cover adoption),
  BISECT/`PresentCore`, DLAA Present pipeline, all vtable/shim machinery.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2645
(27 snapshots, 1 frame marker), PASS, 0 fatal, 0 injection. Artifacts in
`logs/test_runs/20261003T144936Z/`. No overlapping BeamNG process (tasklist clean
pre-run and post-run).

**Chronological stage evidence (this run):**
- Config/build/deploy/gameplay/plugin/Present: PASSED (incl. new config log line 10).
- Camera validation: PASSED with discrimination — 5 `camera CB reject` logs (wrong
  sizes/content correctly refused) → `camera CB ring re-discovered ... srcOff=247700992`
  → `frame 1 started (render 1286x723)` → `camera CB patched ... far 12500.0`.
- Scene/MV/depth readiness (creation hooks, ungated): PASSED — scene RTV primary
  (1902x1033) + 1920x1080 ALTs, MV primary + ALTs, 4 `depth candidate SRV`, display
  adopted 1920x1080 (render 1286x723).
- Buffer-copy path active (50+ `CopyBufferRegion` logs incl. 1616B candidates, 176B
  velocity-size copies at lines 758/808) but zero `velocity CB patched` — validation
  rejected them (content/size gate, pre-existing behavior, not a regression).
- Newly opened gates UNEXERCISED this run: zero `CopyTexBody called`, `full-res copy`,
  `topo:`, `vp diag`, `viewport patched`, `scene-copy DLSS not ready`, `injection
  skipped`, `NGX init deferred`, `SINGLE-DEVICE` logs. The texture/viewport shim
  delivery was dormant (same nondeterminism as prior runs; ECL n=25001 hits=25).
- NGX init/feature/eval/injection: NO attempts, NO failures — logic-reachable now,
  delivery-unproven. **Not DLSS success** (zero injection markers, no visual check).

**Remaining gates / next agenda:**
1. Shim delivery coverage for texture/viewport lists (standing Agenda 2 vtable work;
   logic fix is necessary but not sufficient while submissions miss shims).
2. Quiet-gate maturation (`EnsureUpscalerInit` needs 120 quiet frames once copies flow).
3. Load-phase arming (needs 2nd camera frame + 3 s; single-frame runs can't arm).
4. Legacy camera-jitter gate (`:6527` skips jitter when `!dlaa` — eval would get
   jitter offsets for an unjittered render; enable only after injection markers exist).
5. Re-run until a texture-path-active run exercises the trigger; then trace
   init→feature→eval→injection chronologically before any visual claim.

## Phase 2b Hook-Delivery Reconnect (runs 20261003T151800Z, 20261003T152033Z)

**Q1 — did the tested binaries contain the work?** Yes. `src/d3d12_hooks.cpp` → `dist/ScaleNG.asi`
timestamps order correctly ahead of each run; the new in-game markers for this phase
(`shim vtable slots ... rsSetViewports=...`, `shim totals ...`) appear in both runs'
game-PID logs. The five gating changes are preserved untouched (verified in diff).

**Q2 — which list methods fired?** Run 151800Z: installs 1→49, invocations
`buf=0 tex=0 barrier=2 om=0 vp=0 sc=0` (exact totals, end of run); run 152033Z:
installs 47, `barrier=2`, rest 0. Zero `CopyTexBody`, `full-res copy`, `vp diag`,
`viewport patched`, NGX/init/eval/injection markers in both. Camera path dormant in
both (0 frame markers) vs 1 frame in 144936Z — game-side nondeterminism.

**Q3 — does CopyBufferRegion activity prove shim delivery?** Yes, when present:
`Hook_CopyBufferRegion`'s sole caller is `Shim_CopyBufferRegion` (clone slot 15), so
the 144936Z camera patch proves record-time clone delivery for that list. **Correction
to the prior run note:** 144936Z's texture/OM/barrier shims also fired at record time
(`native-usage: invocation method=CopyTextureRegion/ResourceBarrier/OMSetRenderTargets`
counts 1–5, first-5 caps). The absence was never delivery — it was routing (Q4).

**Q4 — why no CopyTexBody/viewport callbacks?** Deterministic disconnect, not vtable
survival: the installed shims routed elsewhere while the legacy machinery sat in
uncalled functions —
`Shim_CopyTextureRegion` → `ObserveNativeCopyUsage`/`TryVectorA` (never `CopyTexBody`);
`Shim_ResourceBarrier` → `ObserveNativeBarrierUsage` (never state tracking);
`Shim_OMSetRenderTargets` → `ObserveNativeOmUsage` (never bind tracking);
viewport/scissor had **no clone slots at all** (only 15/16/26/46 installed) and
`Hook_RSSetViewports`/`Hook_RSSetScissorRects` had zero callers — so
`g_patchAppliedThisFrame` could never be set. Additionally the `Real_*` command-list
forwarding globals are permanently null (two driver vtables exist, so forwarding
correctly stays per-list): `DoInjection:1995` and `Barrier()` contained unguarded
null calls — a latent crash on first injection, guarded in this change.

**Q5 — queue/device?** Submissions flow through the captured game DIRECT queue (MinHook
slot-10 ECL hook: 16k/76k submissions observed; `GAME direct queue captured` logged).
Command-list creations split across **two** game devices (`g_device` + a second device
with ≥10 capped `CreateCommandList skipped` in both runs); skipped-device lists are
never shimmed by design. Submit-time third-vtable mismatch persists (151800Z: 16k
misses; 152033Z: 76k/1 hit) but affects submit-time observations, not record-time
hooks (proven by 144936Z record-time hits).

**Interception options compared:** (a) extend the existing per-list clone — chosen:
proven install path (49/47 OK, 0 fail), same table the game already executes through,
CFG-registered targets, exact-method coverage (21/22 SDK-verified against
10.0.28000.0 `DECLSPEC_XFGVIRT` order: ...15 buffer, 16 texture, 21 viewports,
22 scissors, 26 barrier, 46 OM); (b) global MinHook on driver list methods —
rejected: two live driver tables, hot-patch risk, target/signature/lifecycle unproven;
(c) ECL-time patching — rejected: recording already done; (d) Present-only injection
— rejected: wrong ordering for engine-list evaluate.

**Implemented (`src/d3d12_hooks.cpp` only; no config/INI/hook-mechanism changes):**
- `Shim_CopyTextureRegion` calls `CopyTexBody` first (intent analysis before any
  vector substitution/forward; CopyTexBody's internal forwards are null-guarded
  no-ops, shim forwards once — recording unchanged).
- `TrackResourceBarriers` extracted from `Hook_ResourceBarrier`, called from the
  live barrier shim; `TrackOMBind` extracted from `Hook_OMSetRenderTargets` (3
  GetDesc → guarded form), called from the live OM shim. `Hook_` wrappers kept
  coherent (uniform guard + null-guarded forwards). OM bind tracking is the sole
  writer of the state `SceneColorBound()` (viewport prerequisite) reads.
- New `Shim_RSSetViewports`/`Shim_RSSetScissorRects` on clone slots 21/22 with
  bounded ENTRY logs + invocation counters; `Hook_` re-signed to
  `(list, n, p, realFn) -> recorded-bool` (both were dead: zero callers, so the
  signature change is safe) with single-forward semantics; struct originals,
  `CfgMarkValid` 4→6, slot-diag extended.
- Null-guards: `Barrier()` early-return; `DoInjection` result-copy guarded with
  log-once + return (no false injection marker). Per-list forwarding of the
  result copy/barriers is explicit stage-2 work after trigger evaluation is proven.
- `shim totals` exact-counter log on the Present heartbeat (1/60) splitting
  installs (`instOk/Dedup/Fail`) from invocations (`buf/tex/barrier/om/vp/sc`).

**Verified in vivo:** both runs PASS (freeroam + `thePlayer`, 0 fatal): 6-slot capture
logs genuine per-slot driver addresses; 49/47 installs, 0 fails; 2 live barrier
trackings with no crash; forwarding behavior unchanged (all forwards still via
per-list originals). Effectiveness unproven: no trigger evaluation yet
(`scene-copy DLSS not ready` / `injection skipped` remain the next milestone lines),
pending a camera-active run. **Not DLSS success** (0 injection/eval markers, no visual check).

**Remaining unknowns / next agenda:**
1. A camera-active run exercising CopyTexBody evaluation + barrier/OM/vp tracking
   end-to-end (single bounded run when convenient; no run-spam: the code path is now
   proven capable of firing whenever the game records on shimmed lists).
2. Multi-device list coverage: render lists on the second device are never shimmed
   (deliberate boundary; expanding it needs its own safety case — not done here).
3. Stage-2 per-list forwarding for the DoInjection result copy + barriers (only after
   trigger evaluation is log-proven).
4. Quiet-gate maturation, load-phase arming, legacy jitter gate — unchanged, downstream.

**Diagnostic run 20261003T160905Z (no code changes):** PASS overall — `smallgrid`
freeroam + `thePlayer` (PID 11500), Present 1→2645, 0 fatal markers, artifacts in
`logs/test_runs/20261003T160905Z/`, no overlapping process. Camera path fully
dormant: 0 frame markers, zero `CopyBufferRegion src=` records (not even rejects),
zero `camera CB` lines of any kind. Shim totals end `instOk=27 instDedup=1
instFail=0 buf=0 tex=0 barrier=2 om=0 vp=0 sc=0` (2 loading-phase barrier trackings
only); ECL 76k submissions / 1 hit (submit-time mismatch shape unchanged). MV RTVs
and display 1920x1080 adopted via ungated creation hooks; OM bind tracking never
invoked (`om=0`). **The experiment did not reach trigger evaluation** — frame
counter never advanced, so no gate verdict is possible from this run. Stopped here:
no hooks added, no repeat runs. Next remains a camera-active run (expect
`scene-copy DLSS not ready` / `injection skipped` as the first evaluation evidence).

**Camera-path presence audit (read-only, no code/run changes):** compared raw logs for
active runs `20261002T202302Z` (candidates, rejected pre-fix), `20261003T133157Z`
(frame 1 + patch) — and correction: `20261003T144936Z` is camera-**active** (frame 1 +
patch), not dormant — against dormant `20261003T151800Z`/`152033Z`/`160905Z`.
- Eliminated as causal: gameplay (all six: freeroam + `thePlayer`, 30 s, PASS, exit 1,
  Present 1→2645), config (all `dlaa=0`), binary (active spans pre- and post-fix builds;
  same-binary pairs 144936Z-active vs 151800Z-dormant differ), render cadence.
- Active-run loading schedule is deterministic: identical srcOff progression
  (2048 → 2071552 → 3473920 → rejects at 3831808/3840512/3851264 → 9933312 →
  1616 B at 14982656/14985216/14987264), same sizes/list types (96 B on type-3 COPY
  lists, 1616 B on type-0 DIRECT), same 256 MB ring, copies at init+6 s, accept at
  init+19 s in both 133157Z and 144936Z.
- Dormant runs show zero buffer records of **any** kind (not even rejects; `buf=0`
  with 27–49 installs) while loading otherwise proceeds on schedule (RTVs, ECL,
  presents identical) — an observation gap, not game silence. Coverage does not
  support "zero game API calls".
- Causal difference found — device coverage: dormant runs create ≥10 (capped)
  command lists of types 0+3 on a **second** device (`device != g_device`, never
  creation-shimmed per `Hook_CreateCommandList:2682/2699`); active runs have 0 skipped
  creations. RTV-creation sampling in 160905Z lands on the second device; ECL hits
  25 (active) vs 1 (dormant). The source comment at creation time states the premise
  explicitly: creation-time attach is the record-time delivery mechanism
  ("Installing at ExecuteCommandLists is too late").
- Call path for an observed copy (current source): list created on `g_device` →
  creation-shim installed with surviving clone → game records `CopyBufferRegion`
  ≥32 B (Hook_ diagnostic) → ≥1616 B region `Map`s + `ValidateCameraCb` passes →
  `StartFrame`. Dormant runs fail at step 1 for the upload lists.
- Open question (logs cannot resolve): why the game selects one vs two devices per
  run, and whether dormant-run copies go to second-device lists vs genuinely absent.
  Camera-buffer creation itself is unobserved in dormant runs.
- Smallest justified next step (proposed, NOT implemented): drop the
  `device == g_device` condition at creation install so all game devices' lists are
  creation-shimmed (same safe per-list clone op already performed at submit time for
  any device's lists; per-list originals make it table-agnostic). Falsifiable fork:
  buffer/camera records appear → device coverage was causal; still `buf=0` with no
  skips → game genuinely silent or pre-record replacement, directing the follow-up
  to record-vs-submit timing diagnostics. No live test recommended until approved.

**All-device experiment run 20261003T170101Z:** PASS overall — `smallgrid` freeroam +
`thePlayer` (PID 12004), 1 frame marker, Present 1→2645, 0 fatal markers, artifacts
in `logs/test_runs/20261003T170101Z/`, no overlapping process. Build exit 0,
artifacts match dist.
- Experiment condition NOT met: zero second-device creations this run (0 skips;
  all logged creations `primary=1 covered=1`). Device-coverage causality therefore
  still open — this run cannot confirm or refute it. The change itself verified safe:
  creation logs carry device/type/covered, per-list forwarding untouched, no crash.
- Reconnect verified live (prior phase's routing, first in-vivo exercise):
  `CopyTexBody called`, `full-res copy` analysis + `depth candidate` adoptions,
  `scene RTV bound via OMSetRenderTargets`, `vp diag`, `tex=389 barrier=155 om=40
  vp=44 sc=49` end totals — all on primary-device lists, 0 fatal.
- Camera active: `frame 1 started`, `camera CB patched ... far 12500.0` (pos far from
  origin — late-load freeroam spawn, consistent with prior accepts).
- **First unmet gate (verified): scene-identity mismatch.** Scene adopted only as
  ALT and never primary (`g_sceneColor` = stale 1902x1033 loading target; live
  1920x1080 gameplay target in `g_sceneColorAlt`); every `vp diag` shows
  `boundScene=0` (bound RTV matches neither tracked pointer despite `rtvValid=1`);
  zero `full-res copy` records with `scene=1`; zero `viewport patched`. Hence no
  trigger evaluation of any kind (neither fire nor `not ready` skip), no NGX
  init/feature/eval/injection markers. Result-copy/barrier wiring correctly NOT
  advanced (evaluation unevidenced).
- Smallest next experiment (proposed): bounded diagnostic printing the
  viewport-bound / full-res-copy resource pointer + desc alongside tracked
  scene/ALT pointers (existing logs print only match flags), to test whether the
  live target is a third sibling (rotation) vs structurally different (e.g. the
  terminal copy-chain node feeding Present). Diagnostic-only; falsifiable either way.

**Scene-identity audit (read-only; run 170101Z evidence, no code/run changes):**
- Existing logs were sufficient — no new diagnostic justified, none added.
- Late-gameplay topo snapshots (present 1805–2645) give exact identity:
  `scene=C2EA2AF220` → guarded GetDesc faults `C0000005` (**primary loading target
  is freed** — stale tracker entry); `alt=C32DD97ED0` 1920x1080 fmt-11 valid;
  `bound=C2FD2F5660` 512x512 fmt-10 (small FLOAT16 target, not scene);
  `lastCopySrc=C2EA4BC890` 1920x1080 fmt-34 (an MV, not scene color). No pointer
  matches the tracked slots; descriptors alone match many live targets and were
  correctly not treated as identity.
- Adoption sequence shows why: UNORM ALT churns across ≥4 concurrent 1920x1080
  targets within one second (`C2FCC14C40 → C32DD88110 → C32DD8DBB0 → C32DD97ED0`,
  multi-buffered ping-pong), keeping only the last; the live UNORM target observed
  at 26.437 s was evicted from the slot within a second.
- Blocking selection rules (source-verified `Hook_CreateRenderTargetView` + OM/copy
  paths): primary assigned **once** at first adoption and never replaced/retired
  (only handle refresh on same resource); ALT is last-wins single-slot overwrite;
  **no promotion rule exists anywhere**. With N concurrent engine targets, both
  `boundScene` (viewport) and `isSceneSrc` (copies) miss persistently.
- Proposed next (behavior change, not done here): a promotion/rotation rule for the
  primary slot driven by usage evidence already logged (full-res copy sources,
  OM-bound display-sized targets, present-feed recurrence) with freed-pointer
  invalidation (the `C0000005` fault proves the need). Falsifiable: `boundScene!=0`
  / `scene=1` lines appear without changing any gate.
- This run again had zero second-device creations: safe-operation validation only;
  two-device causality still open.

**Scene-set registry run 20261003T171932Z (the chosen change):** PASS overall —
`smallgrid` freeroam + `thePlayer` (PID 15808), 1 frame marker (camera active),
Present 1→2645, 0 fatal markers, artifacts preserved, no overlapping process.
- Registry mechanics verified live: 6 `scene-set note` inserts (1902x1033 → 1920x983 →
  4×1920x1080 gameplay targets); sweep `invalidated` the 2 stale loading targets on
  guarded-read faults (stale handling proven); no evictions (count ≤6/8), no reuse
  lines. Invocation `buf=97 tex=388 barrier=123 om=29 vp=32 sc=35`.
- Match flags still 0 (`boundScene=0`, `scene=0`, no viewport patch/trigger/NGX):
  the viewport-bound target is a further ping-pong sibling beyond the noted ones
  (notes stop at 20:19:57.520, viewports continue), or a pre-hook-created target.
  `DoInjection` stale-guard never reached (no trigger evaluation). No false
  injection markers; barriers/result-copy guards untriggered.
- Exact rule shipped: bounded 8-entry pointer-identity set, UNORM+size insert filter
  (creation/OM-adoption gates; copy/OM-bind touch recency only), LRU eviction,
  Present-cadence guarded sweep (fault→drop, desc-change→baseline refresh), hot-path
  pure-compare membership in `SceneColorBound` + copy trigger, `DoInjection` scene
  guarded-skip. Classification variables and all gates unchanged.

**Bound-identity diagnostic run 20261003T172248Z:** PASS overall — freeroam +
`thePlayer` (PID 8392), 0 frame markers (camera dormant), Present 1→2645, 0 fatal,
artifacts preserved. Registry re-verified (5 notes, 1 invalidation). Second-device
creations present and covered (`primary=0 covered=1` ×2, 0 skips — all-device path
exercised safely). Delivery worked (`buf=84`, ECL 25 hits) but the game emitted no
camera-size copies on any shimmed list.
- The new vp-diag bound-identity line did NOT fire (0 lines): it sits inside the
  `g_patchViewport` gate, which requires a camera `StartFrame` that never came.
  Lesson verified: the correlation log must escape the arming gate to be observable
  in dormant runs. Micro-follow-up proposed (not done): emit the bound-identity
  line unconditionally-capped from the viewport shim path.
**Bound-identity diagnostic run 20261003T173201Z (pre-gate vp-bind record):** PASS
overall — `smallgrid` freeroam + `thePlayer` (PID 16996), 1 frame marker (camera
active: frame 1 + patch far 12500.0), Present 1→2645, 0 fatal markers, artifacts
preserved, no overlapping process. Invocation `buf=111 tex=431 barrier=155 om=40
vp=44 sc=49`; 2 second-device creations, both covered (`primary=0 covered=1`),
0 skips; ECL 72.5k/30 hits.
- Pre-gate record works as designed (13 distinct events, no gate dependency):
  loading viewports bind null, then a set member (`inset=1 ... <= BOUND` — set
  lookup proven functional in vivo, lookup-bug hypothesis eliminated).
- Gameplay 1920x1080 viewports NEVER bind a tracked candidate: bound=null (no RTV
  bound/resolvable at that point), or 1920x1080 fmt-34 (**MV-format texture under
  a display-sized viewport — the velocity pass**, correctly excluded), or 512x512
  fmt-10 FLOAT16 small targets. The set holds 4–6 UNORM siblings throughout. Zero UNORM copy
  sources observed (all full-res copies fmt-45/fmt-34). One OM `scene RTV bound`
  line, loading-phase only.
- Verdict: neither a new sibling nor a lookup bug — the engine does not present
  its UNORM scene target at the observed viewport/OM points in matchable form
  this run. `boundScene`/`scene=1`/trigger/NGX therefore silent by consequence;
  result-copy/barrier wiring still correctly unadvanced. No DLSS success claimed.
- Next testable gate: locate where the UNORM scene target IS bound/copied
  (unshimmed-list traffic vs bundle payloads vs pre-hook handles). Smallest
  candidates: creation-type census (any type-1 bundle lists on covered devices)
  and submit-time device attribution for the 72.5k submissions.

**Crash triage run 20261003T201107Z + rerun 20261003T201614Z (per-list correlation
change):** first run ended `GAME_CRASHED_AFTER_PLUGIN_INIT` (exit 0xC0000005) ~40 s
in during mod mesh streaming, AFTER freeroam + `thePlayer` were confirmed.
- Faulting module is the game EXE itself (Event 1000, offset 0x794732), not the ASI
  or driver. That run's hooks were provably non-modifying (no viewport/camera patch
  possible: 0 frames; all forwards exactly-once; new code is reads + capped logs).
  Same mod content loads in stable runs. Prior crash history exists (213754Z).
- Identical-binary rerun (`--skip-build --no-deploy`): PASS, freeroam + `thePlayer`,
  camera active (frame 1), 0 fatal — no reproduction. Verdict: flaky/game-side,
  change retained (single-sample exoneration is weak; continued monitoring).
  Artifacts of both runs preserved; no overlapping processes.
- Per-list correlation works (24 + 24 lines): same-list OM→viewport alternation
  PROVEN (`omSeq`/`vpSeq` interleave per list) — but OM resources resolve null
  because the bound handles miss `rtvMap`.
- Divergence point found (source-verified): all three `g_rtvMap[handle] = res`
  writes sit inside UNORM16/MV-format-gated creation blocks, so LDR fmt-28 RTV
  views never enter the map. Handle `1D994DFF040` HAS creation + provenance records
  (1902x1033 fmt-28 → `8BD3782A50`) yet resolves null at every bind. Global
  last-bound state is therefore not the (only) problem — the map itself is blind
  to LDR views on every list.
- Next proposed (not implemented): record handle→resource for ALL display-sized
  RTV views format-neutrally (adoption/classification stay format-gated). Falsifiable:
  OM binds resolve (`om≠null` in list-corr) and vp-bind shows live LDR/UNORM pointers;
  `boundScene` patch gating itself unchanged. No DLSS success claimed.
  (Implemented below as the handle-map widening run.)

**Handle-map widening run 20261003T202758Z:** PASS overall — `smallgrid` freeroam
+ `thePlayer` (PID 9736), 1 frame marker (camera active: 3 camera patches), Present
1→3125, 0 fatal markers, artifacts preserved, no overlap. Build exit 0. No crash
recurrence across two consecutive runs with all recent changes (prior crash stands
as flaky/game-side; monitoring continues). Exact change: format-neutral
handle→resource write for every display-sized mip1 RTV view at creation (refresh on
every creation; BookGuard scope; adoption/classification/selection/patch/gates
untouched) + format guard on MV re-adopt (only genuine R16G16F re-adopts; never
fired this run — no valid re-adoptions blocked). No hooks/scans/Map/config changes.
- Same-list OM resolution FIXED: `om=DE6A3B8430` with live desc (was null every
  bind); vp-bind shows actual bound pointers + descs (`vp=1920x1080
  bound=DE701D9E90 1920x1080 fmt=28`), `inset=1` hits on the loading member.
- Decisive downstream finding: gameplay 1080p viewports bind an **LDR fmt-28
  target**, so `boundScene` stays null BY DESIGN (UNORM-gated selection preserved
  exactly as mandated — not broadened to force a match). Trigger/NGX silent by
  consequence. MV re-adopt guard untriggered (no evidence for or against).
- Next unmet gate (explicitly out of scope for this change): whether the gameplay
  scene compositing target is genuinely LDR (which would obsolete the UNORM-scene
  design assumption) vs UNORM-but-bound-elsewhere. Stopping here per mandate.
  No DLSS success claimed.

**LDR bound-target trace (read-only, run 20261003T202758Z, pointer `DE701D9E90`):**
- Full lifecycle in one frame window (present=393): RTV view created (fmt-28 view of
  fmt-28 resource, handle `2C36E1BE660`) → OM-bound as slot-0 RTV on 4+ lists →
  viewports 1920x1080 on the same lists (same-list pairing proven) → barriers cycle
  192↔4 (SHADER_READ↔RENDER_TARGET) repeatedly on one list → never copied, never
  SRV-viewed observably, never in present-feed/backbuffer records.
- Format re-verified in-header (28=R8G8B8A8_UNORM); no role/colorspace inferred.
  Not in scene set / MV / depth pointers; nothing promoted.
- Role verdict: LDR mid-chain composite/read rotation member. First clear point is
  the OM bind + RT↔SR barrier pair itself. Exact upstream (which UNORM input) and
  downstream (where its contents go) are unobservable by design — no copies exist
  and sampling runs through untracked descriptor tables.
- Correlated context: UNORM set members barrier-cycle RT in the same windows (live
  ping-pong, not stale) and carry RTV+SRV roles; D9561E7D00-class UNORM copy pairs
  exist but never touch this target. Related gap noted (not changed): the
  native-candidate registry also filters out fmt-28, so the bound target has no
  candidate entry either.
- Next falsifiable step (not implemented): barrier-timing correlation of UNORM
  members' SHADER_READ transitions against LDR production windows is already
  loggable offline; if a member consistently precedes production it becomes a
  candidate input (correlation, not proof). No new hooks/code needed for that
  analysis. No DLSS success claimed.

**Offline barrier-timing correlation (read-only, run 20261003T202758Z):**
- LDR `DE701D9E90` timeline (all present=393, 23:28:23.265–328, ~63 ms): created
  10 s earlier (loading) → OM-bound slot-0 on 5 lists interleaved with same-list
  1920x1080 viewports → barriers cycle 192↔4 repeatedly on `DF249F01D0`
  (render↔sample). Never copied, presented, or SRV-viewed observably.
- UNORM set members in the same window: primary `DE6FF9ABC0` OM-bound once (512
  viewport only, no barriers/copies); `DE701DB9C0`/`DE18C03970` fully idle (only
  snapshot listings); `DE188F4FB0` created mid-window (:300), adopted, first render
  (:337, after LDR production). **Zero UNORM render→sample (producer) transitions,
  zero UNORM OM binds at display viewports, zero UNORM copy sources in-window.**
  One sibling LDR target (`DE188F3D90`, same heap batch) transitions to RENDER at
  :338 on the fresh member's list.
- Timestamps are ms-resolution (same-ms events unordered); barrier/OM lines carry
  list pointers but no queue attribution — cross-list ordering stays coarse.
- Verdict: **no candidate stands out; the trace cannot identify the upstream
  input.** The missing link is exact: descriptor-table SRV binds (which UNORM
  resource is sampled, when, on which list) leave no pointer trace by design.
- Smallest next diagnostic (proposed, not implemented): observe-only
  SetGraphicsRootDescriptorTable shim (slot 32, same clone pattern) logging root
  parameter + heap + timing around LDR production — bind TIMING without resolving
  descriptor contents (heap-content reads stay out of scope unless explicitly
  approved). Correlation only, never proof. No DLSS success claimed.
  (Implemented below as the descriptor-table timing run.)

**Descriptor-table timing run 20261003T210100Z:** PASS overall — `smallgrid`
freeroam + `thePlayer` (PID 4512), 1 frame marker, Present 1→3125, 0 fatal
markers, artifacts preserved, no overlap. Build exit 0. Exact change: slot-32
shim (signature/slot verified via filtered CINTERFACE order: 32 sits between
verified 26/28 and 46), per-list original, single forward; detail lines
(list/type/root/gpu-handle/present/ecl serials, first-12 + 1/500) + exact `rt`
total. No heap-content reads, retention, selection, or injection changes.
- Shim live: `rt=130` total; slot diag captures genuine driver slot-32 target.
  Detail shows the classic per-draw pattern on one list at present=393/ecl=1533:
  root=4 with VARYING gpu handles (per-draw textures) + root=5 with a CONSTANT
  handle (sampler), interleaved on the SAME list with OM binds (fmt-10/MV),
  MV copies, and barriers.
- Same-list/same-window correlation achieved (list + present/ecl serials shared
  across OM/viewport/barrier/roottable records) — but handles are values only:
  timing evidence, no resource identity, no handoff proof (as scoped).
- This run's 1080p viewports bind an LDR fmt-28 target, MV fmt-34, and FLOAT
  targets — never a UNORM set member. Trigger unevaluated (no viewport patch, no
  `not ready` skip, no NGX markers); result-copy/barrier wiring still unadvanced.
- Next (approval required): descriptor-heap content reads to resolve gpu handles
  → resources — if one equals a UNORM set member, input established by pointer
  identity; otherwise UNORM scene is not sampled in observed windows. No DLSS
  success claimed.

**Heap-inventory runs 20261004T114506Z (crash) + 20261004T114659Z (PASS):**
freeroam + `thePlayer` both runs (where reached); 114659Z PASS overall (1 frame
marker, Present 1→3365, 0 fatal, artifacts preserved, no overlap). Build exit 0.
Exact change: observe-only `CreateDescriptorHeap` hook at verified slot 14 on the
shared table (+CfgMarkValid, install log); forwards once, then transiently reads
heap type/count/flags, CPU+GPU starts (GPU only if shader-visible), and the
per-type increment — no retention, no heap-content reads. Exact heap total
separate from capped detail (first 24 + 1/100).
- 114506Z crashed at startup (0xC0000005, faulting module unknown, offset 0) AFTER
  init-complete but BEFORE first device creation — none of the new code executed
  (no swap, no heap lines; 20-line log). Rerun of the identical binary passed
  cleanly. Verdict: startup flakiness, not the change (which provably never ran
  pre-crash); third distinct crash signature overall (cf. 213754Z, 201107Z).
- 114659Z inventory: 6 heaps (CBV_SRV_UAV 1M non-visible + 1M shader-visible with
  cpu/gpu bases + inc=32; sampler 2048 visible; RTV/DSV CPU-only). Offline join:
  9/10 root-table gpu handles fall in the visible CBV_SRV_UAV heap (indices
  308–3415, predicted CPUs computed); 1 in the sampler heap (idx 133).
- Join limit reached as predicted: SRV-creation CPU handles are not logged
  anywhere, so predicted CPU ↔ SRV comparisons are impossible with current logs.
  CopyResource #2 repeats (flags 0, no match); new #3 (1000x600 fmt-28, copy-only
  endpoints). Trigger/NGX silent by consequence. No DLSS success claimed.

**PIX setup + launch-conflict probe (no workflow/script/game changes):**
- Installed official PIX 2603.25 x64 from `download.microsoft.com` (212 MB);
  Authenticode Valid, signer Microsoft Corporation; registered package 26.03.25.001;
  `WinPixGpuCapturer.dll` present (x64+ARM64, signature Valid); UI is `WinPix.exe`;
  `pixtool.exe` supports headless `launch`/`take-capture`/`save-capture`/
  `save-event-list`/`save-screenshot`. Machine is hybrid AMD iGPU + NVIDIA RTX 3050
  (game uses NVIDIA; captures would be adapter-bound). Winget msstore path blocked
  (interactive terms acceptance) — not used.
- `pixtool launch` CLI quirk (verified): `--command-line=<single-token>` works;
  bare multi-word values fail to parse; quoted `--command-line="-nosteam -console"`
  works. Menu-mode instrumented launch attempted twice with correct syntax.
- Result: PIX creates the GPU capture session and BeamNG starts (PID 15300), our
  ASI initializes FULLY under PIX (all detours installed, first `D3D12CreateDevice`
  entered with base IID) — then the game dies during first device creation and its
  crash-reporter spawns; no WER trace, no capture obtained. No leftover processes.
- Assessment: triple-detour collision at first `D3D12CreateDevice` (PIX capturer +
  UAL Detours + our MinHook chain) is the prime suspect; PIX-vs-game incompatibility
  not excluded. ReShade remnants are inert (renamed `.bak`/disabled).
- STOPPED here per protocol — no scripts, game files, proxies, or config touched.
  Blocked next steps requiring approval: (1) reversible ASI-relocation triage
  launch (move `ScaleNG.asi` out of `Bin64/plugins/` with backup, PIX-launch,
  restore) to separate ASI-vs-PIX causes; (2) runner attach-mode script change
  (`--no-launch`: skip Popen, connect PIX-launched PID's TCom) for any gameplay
  capture, since BeamNGpy refuses occupied ports and PIX must launch pre-device;
  (3) interactive alternative: user takes the freeroam capture manually in PIX UI.

**PIX attach-vs-launch finding (docs-verified, no execution):** PIX GPU capture
requires launching the target under PIX (verified: GPU-captures doc describes only
the launch flow; `pixtool` exposes `launch`/`take-capture` but NO attach command).
Attach-to-running-process covers TIMING captures only (verified: timing-captures
doc + Device Connection tab model). Consequence: a UI attach cannot substitute for
launch for the needed GPU frame; any UI attempt is a launch attempt with identical
injection mechanics to the failed pixtool launches.
- Evidence-based rationale for exactly one UI attempt anyway: pixtool CLI arg
  parsing proved lossy (multi-word `--command-line` quirk), so CLI-launched games
  may not have received exact arguments; a UI launch with typed fields eliminates
  that doubt. Distinct failure (past device creation) vs identical death cleanly
  separates CLI artifacts from the underlying incompatibility.
- Runner launch forensics (read-only): `BeamNGpy(...)._prepare_call()` builds the
  full command (`-nosteam -tcom -tport 25252 -console -tcom-listen-ip 127.0.0.1
  -level smallgrid -vehicle pickup`), `cwd=Bin64`, `stdin=PIPE`, stdout to file,
  no custom env. UI attempt must mirror exactly these (see report for steps).
- Attach-mode proposal (unimplemented, for review): optional `--no-launch` flag —
  skip `Popen`, find the single PIX-launched `BeamNG.drive.x64.exe` PID, reuse the
  existing connect-retry + freeroam + observe + analyze flow, and skip process
  termination for the adopted PID at the end. Default behavior byte-identical when
  the flag is absent. Needed only if a UI/PIX launch ever survives to freeroam.
  No DLSS success claimed.

**UI-launch forensics (read-only plugin-log survey; user drove PIX UI):**
- Full `D3D12CreateDevice` first-bytes survey: ~400 sessions CLEAN (`48 89 5C 24`,
  incl. ALL runner runs and normal manual launches) vs `E9 23 72 FE` pre-hook in
  exactly p16220/p15652/p9788 (17:17–17:32, the user's PIX UI window) plus archived
  pixtool-run PIDs. (Other byte patterns are Oct-2-era sessions — excluded as stale.)
  E9 never appears without PIX involvement in this dataset.
- p16220 @17:17:49 (19 lines): init complete → silence within ~1 s, BEFORE any
  factory/device call, no WER. Matches the reported UI-launch failure (crash
  reporter sighting consistent with Breakpad-style handling, no WER written).
- p15652 @17:18:37 (7.37M lines, 11 min) and p9788 @17:32:37 (1.55M lines, 2.5+ min):
  E9 present, yet device created + swapped + queue captured + ECL flowing — but NO
  real swapchain, NO frames, NO camera in either session (only EGSH dummy presents;
  `om/vp` ~zero; p9788 topo present counter anomalous at 18M). Both end abruptly
  mid-traffic with no fatal markers and no WER. Stuck-without-presenting profile,
  not crashed; p15652 sustained ~660k log lines/min (submission churn, not idle).
- p15060-family @17:35 (CLEAN bytes): normal manual launch, healthy 2-min traffic —
  user verifying the game still works after the failures.
- Open questions requiring the user's input (unobservable from logs): exact UI
  exe/args/workdir typed per attempt; which UI operation maps to p15652/p9788
  (GPU-launch retries vs timing vs other); whether overlay software (RTSS/Discord/
  Game Bar/GeForce overlay) runs on this machine. Attach refusal (DLL must be
  loaded) stands as documented GPU-capture behavior — no force-load. No DLSS success
  claimed.

**UI-launch verdict (user-confirmed, all GPU launches with identical spec):**
- All three E9 sessions were GPU-capture UI launches with the specified exe/args/
  workdir — yet produced THREE different outcomes: instant pre-device death p16220
  (~1 s, before any factory call), boot-then-stuck p15652 (11 min, device+swap+
  queue OK, never a real swapchain/frame), boot-then-stuck p9788 (2.5 min, same
  profile plus anomalous present counts). Identical inputs → nondeterministic
  failure mode points to a PIX-injector vs game-init race, not configuration.
- Steam overlay is present but NOT implicated: E9 correlates 100% with PIX launches
  (pixtool + UI) and 0% with ~400 clean sessions (all runner runs + normal manual
  launches); direct-exe launches bypass it.
- Conclusion: PIX GPU capture of BeamNG freeroam is BLOCKED — 0 for 6+ launches
  reach gameplay (3 pixtool incl. vanilla, 3 UI). Per the no-blind-repetition rule,
  no further UI/pixtool launches are recommended. Operative paths remain the
  log-based diagnostics and a user-driven RenderDoc session if ever approved.
  Attach refusal stands confirmed. No DLSS success claimed.

**ASI triage result (user-approved, fully reversed):** with `ScaleNG.asi` relocated
(hash-recorded) the PIX-launched game dies identically (session created, ready
notification, then silence, no capture); with the winmm ASI-loader proxy also
relocated (fully vanilla game) it dies identically a third time. Both files
restored with hash verification (`asi-match=True`, `winmm-match=True`); no leftover
processes; no script/config/proxy changes at any point.
- Verdict: the PIX-launch death is a PIX-vs-BeamNG (or PIX-vs-environment:
  DXCoreAdapter DEVICE_REMOVED warning, hybrid AMD+NVIDIA, mod content) issue,
  INDEPENDENT of our ASI, UAL loader, and hooks. GPU capture via pixtool-launch is
  blocked for this title in this environment until that is resolved (upstream or
  via manual PIX-UI attempts, which remain untested).
- Attach-mode runner change is moot until launch survives; proposal retained but
  not implemented. Heap-inventory/offline-join work stands as the operative
  descriptor-evidence path. No DLSS success claimed.

**PIX UI-launch assessment (no game/script/config changes):** WinPix.exe is the UI
(there is no PixWin.exe in 2603.25); no GUI-driving capability exists in this
environment, so interactive UI operation is not executable. pixtool exercises the
identical injection path (same `WinPixGpuCapturer.dll`, same session model), so a
UI launch would not differ mechanically.
- PIX docs (verified): limited multi-GPU support (single-adapter playback with a
  toolbar dropdown; auto-select if the app used one adapter); captures can fail on
  invalid D3D12 usage (debug-layer/GPU-validation guidance); no documented
  hybrid-launch option exists. Changing Windows graphics preference, drivers, or
  game files to work around this needs explicit approval — not done.
- Three pixtool launches (full ASI → ASI-less → fully vanilla) all die identically:
  session created, ready notification, then silence with the crash-reporter
  spawning ~2 s in; death at first `D3D12CreateDevice`, before any game rendering.
  The DXCoreAdapterState DEVICE_REMOVED warning implicates the hybrid AMD+NVIDIA
  stack. No WER trace; no leftover processes; artifacts preserved.
- Precise issue needing resolution: BeamNG 0.39 DX12 device creation under PIX
  capture-layer instrumentation on this hybrid system. Candidates: PIX-vs-game
  device-setup incompatibility, hybrid-adapter selection inside device creation,
  or mod content at startup. Our ASI/UAL/hooks are exonerated (vanilla death).
- Attach-mode (`--no-launch`: skip Popen, TCom-connect to a PIX-held PID, default
  flow unchanged) remains proposed but unimplemented — moot with no surviving
  process; implementing it untested would be worse than proposing it exactly.
  Manual UI capture by the user is still available as the interactive path.
  No DLSS success claimed.

**RenderDoc install + inject attempt (no capture obtained):** official 1.46 x64 MSI
from `renderdoc.org/stable` (repo-README-designated channel; GitHub releases carry
no Windows binaries) — Authenticode Valid, signer Baldur Scott Karlsson; package
installed with `renderdoc.dll` (Valid), `qrenderdoc.exe`, `renderdoccmd.exe`
(`capture`/`inject`/`replay`/`thumb` verbs verified). Hybrid AMD+NVIDIA noted.
No game/script/config/proxy changes for setup.
- Headless path found: `renderdoccmd inject --PID=<game>` needs no pre-launch, so
  no second process and no runner changes for injection itself. (Capture triggering
  still needs the in-app key or a runner attach-mode for driven captures.)
- `inject --PID=5576` (live TCom game, later confirmed freeroam+2 frames+PASS in
  run 20261004T134451Z) FAILED: "Failed to inject renderdoc.dll into process",
  game alive throughout (PASS, 0 fatal). Timing caveat: inject ran ~90 s in while
  freeroam timing is unrecorded — mid-initialization race possible, single sample.
- Per mandate (RenderDoc failure → stop after one attempt): no retry, no capture,
  no heap-content reads. Artifacts preserved (134451Z PASS + inject error output).
  Next (proposed): retry inject at confirmed-freeroam in a future run, or accept
  the inject path as blocked for this title. No DLSS success claimed.

**Timed inject retry runs 20261004T140425Z + 20261004T140832Z:** both PASS overall
(freeroam + `thePlayer`; 2 frames each; Present 1→16085 / 1→17045; 0 fatal;
artifacts preserved; no overlapping processes at any point). No code/config changes
in either cycle; TCom multi-client connect refused (single-client server — timing
must come from render signals, not a second client).
- Inject into confirmed-rendering PID 9032 printed "Launched as ID" then exited 1
  with NO renderdoc module in the game (verified via module list): silent
  non-attach. Together with the earlier explicit "Failed to inject" error, that is
  two distinct failure signatures across two live games — inject path BLOCKED for
  this title in this environment (unless future evidence reopens it).
- No capture was possible (nothing injected to trigger); SendKeys was correctly
  never attempted. Game unaffected in all cycles (PASS throughout).
- RenderDoc remains installed and verified for offline use (existing `.rdc`
  analysis, UI-driven captures by the user). Heap-inventory/offline-join work
  stands as the operative descriptor evidence. No DLSS success claimed.

**Per-app GPU-preference retest (user-authorized reversible change):** preference was
UNSET before (verified by read; only stale Watch Dogs entries exist). Device creator
is `BeamNG.drive.x64.exe` directly (46 MB; our detour logs its PID calling
`D3D12CreateDevice`; `console.x64.exe` is uninvolved in the established flow). Applied
`GpuPreference=2;` (High performance, exact documented form confirmed against
existing entries) via explicit user authorization since the Settings GUI is not
drivable headlessly and direct registry edits were otherwise forbidden; verified by
read-back; no game running at the time.
- PIX retest with full TCom/level/vehicle args: IDENTICAL death (ASI init complete,
  first `D3D12CreateDevice` entered with base IID, then silence + crash reporter,
  no WER trace, no capture). The NVIDIA preference did NOT resolve the startup
  failure — hybrid-adapter selection is eliminated as the cause.
- Previous per-app preference RESTORED (value deleted; absence verified by read);
  no leftover processes; no driver/system/game/script/config changes at any point.
- Remaining PIX-side possibilities (stated, not tested): PIX-vs-BeamNG device-setup
  incompatibility independent of adapter choice, mod content at startup, or the
  DXCoreAdapter DEVICE_REMOVED fragility seen in PIX's own output. Next would need
  driver changes or upstream fixes — explicitly out of scope without asking.
  Heap-inventory/offline-join work stands as the operative descriptor evidence.
  No DLSS success claimed.

**Manual-session forensics 17:08–17:38 (read-only; user drove PIX UI):**
- Full `D3D12CreateDevice` first-bytes survey of the plugin log: ~400 sessions CLEAN
  (`48 89 5C 24`, incl. ALL runner runs) vs E9-pre-hooked: `E9 23 72 FE` only in
  p16220/p15652/p9788 (today 17:17–17:32) plus pixtool-run PIDs. (Other byte patterns
  `E9 7E 7A 36`/`E9 FE 7C 2E` are Oct-2-era sessions — stale, excluded.)
- p16220 @17:17:49 (19 lines): init complete → death within ~1 s, BEFORE any
  factory/device call, no WER. Matches the reported UI-launch failure. E9 present.
- p15652 @17:18:37 (7.37M lines, 11+ min) and p9788 @17:32:37 (1.55M lines, 2.5+ min):
  E9 present, device created + swapped + queue captured — but NO real swapchain, NO
  frames, NO camera in either session (only EGSH dummy presents; `om/vp` ~zero).
  Both end abruptly mid-traffic with no fatal markers and no WER. Stuck-not-crashed
  profile; anomalous counts observed (p9788 topo present=18261965 with ecl=0).
- p15060-family @17:35 (CLEAN bytes): normal manual launch, healthy 2-min traffic.
  Attach warning (DLL must be loaded) is consistent with GPU-capture attach rules
  (docs-verified: attach covers timing captures, not GPU frames) — no force-load.
- Open questions requiring the user's input (cannot be observed from logs): exact
  UI-entered exe/args/workdir per attempt; which UI operation maps to
  p15652/p9788 (GPU-launch vs timing vs attach target); whether overlay software
  (RTSS/Discord/Game Bar/GeForce overlay) runs on this machine — a third-party
  overlay hooking some sessions would also explain sporadic E9. No DLSS success
  claimed.

**Coverage audit + bundle-census run 20261003T174128Z:** PASS overall — `smallgrid`
freeroam + `thePlayer` (PID 9212), 1 frame marker (camera active: frame 1 + patch
far 12500.0), Present 1→2765, 0 fatal markers, artifacts preserved, no overlap.
Build exit 0. SDK re-verified read-only: `ExecuteBundle(ID3D12GraphicsCommandList*)`
slot 27 (declaration order 26→27→28 from the established Close=9 base).
- Cloned methods: 15/16/21/22/26/46 on all covered devices/types; NOT cloned: 17
  (`CopyResource`), 27 (`ExecuteBundle`). No bundle evidence anywhere: zero
  type-1/2 creations (new uncapped `type12` census — definitive for hooked device
  paths), zero type-1 invocations in four runs' logs. Bundle hypothesis demoted
  (residual caveat: a distinct-vtable device would bypass creation hooks entirely;
  no evidence for one — observed second devices share the hooked table).
- vp-bind pattern replicated third run: loading `inset=1`, gameplay 1080p →
  null / MV-fmt-34 / null; 2 second-device creations both covered; 6 scene notes;
  0 UNORM copy sources; trigger/NGX silent by consequence.
- Existing logs cannot distinguish engine-silence from unshimmed-list traffic
  (76k submissions, ~all submit-misses) or from `CopyResource` whole-resource moves
  (slot 17, unhooked and therefore unobservable — simplest remaining same-list path;
  slot/signature not yet verified). Next falsifiable experiment: verify slot 17
  read-only, then propose an observe-only slot-17 shim (same clone pattern,
  counters, no analysis) — UNORM endpoints appear or the count stays 0.
  No DLSS success claimed.

**Endpoint-origin audit (read-only; no code/run changes):** tree, gating, reconnect,
all-device, scene-set, and census changes all preserved; no overlapping process.
- IID field limits (verified): it logs only the FIRST `D3D12CreateDevice` call
  (cap<5 plus early-return skips don't log); the 3 later devices' IIDs are unknown.
  Captured device requested base `ID3D12Device` (header-verified
  `189819f1-…85f7`); later QIs are unhooked, so this weakens but does not kill the
  Device4+ hypothesis. Do not overstate it.
- Newer-API slot map (10.0.28000.0 header, verified): `ID3D12Device4` adds
  `CreateCommandList1`, `CreateCommittedResource1`, `CreateHeap1`,
  `CreateReservedResource1` (higher slots after all base+1+2+3 methods — definitively
  outside hooked 27/29/30); `OpenSharedHandle` family are base-Device methods,
  unhooked. Agility SDK absent from Bin64 → the export detour sees all device
  creations. Our code never QIs Device4+ itself.
- Plausibility with evidence: Device4+ creation — no supporting evidence (base IID
  at creation, no protected-content need), later QI possible but unobserved (LOW,
  untestable hook-free); shared-handle import — no evidence, single-process game
  (VERY LOW); pre-swap boot persistence — timing-neutral (deterministic 3.5 s
  schedule fits both it and post-swap-unhooked creation; timing proves nothing
  either way).
- Single-use copy+barrier pattern: useless for scene ID (no reuse evidence) beyond
  proving viewless intermediates exist. Role stays UNKNOWN; not scene color.
- `CreateHeap` NOT added (confirmed): shares swap activation and the interface
  blind spot; placed creation is observed post-swap — no causal link it could add.
- Recommended next diagnostic (not implemented): QI-IID census on the already-swapped
  device table (slot 0, universal COM signature, per-table original alongside
  existing saves, distinct-IID-only bounded log). Fork: Device4+ IID ever requested
  → '1'-variant creation is live, propose targeted observation; never → Device4+
  path dead, pre-swap persistence stands as the unobservable remainder. No DLSS
  success claimed.

**Endpoint-origin audit + device-IID run 20261003T182050Z:** PASS overall —
`smallgrid` freeroam + `thePlayer` (PID 16144), 1 frame marker, Present 1→3245,
0 fatal markers, artifacts preserved, no overlap. Build exit 0. Exact change: the
existing `D3D12CreateDevice called` line gains the requested `riid` (already capped
<5, params in hand, zero behavior change).
- Acquisition-path coverage (source+SDK verified): hooked 27/29/30 on the shared
  table from swap (~T+3.5 s); NOT hooked: Device4+ `*Resource1` higher slots,
  `OpenSharedHandle` family, heap-only timing, distinct-vtable devices, pre-swap
  window. Agility SDK absent from Bin64 (system d3d12 only) → export hook sees all
  device creations.
- Device IID verified against the header: `189819f1-…85f7` = base `ID3D12Device`
  (`MIDL_INTERFACE` on ID3D12Device). Weakens the Device4+ hypothesis for creation
  itself; does not kill it (later QI is unhooked — stated limitation).
- Endpoint reappearance sweep (3 runs): dst = copy + 2 same-instant barriers only;
  src = copy only; never later copies, binds, or presents. Role UNKNOWN — not
  labeled scene (single-use loading intermediates; contents unproven).
- Timeline is deterministic (log→+2.2 s device→+3.5 s swap→+5 s burst→+7 s copy)
  but timing alone proves nothing about pre-hook creation either way.
- `CreateHeap` assessment: shares swap activation and the interface-version blind
  spot, and placed-resource creation IS observed post-swap — no specific causal
  link it could add. NOT added (mandate condition not met).
- CopyResource #2 repeats (1920x1080 fmt-28, flags 0); trigger/NGX silent by
  consequence. No DLSS success claimed.

**QI-census run 20261003T185159Z:** PASS overall — `smallgrid` freeroam +
`thePlayer` (PID 16400), 1 frame marker, Present 1→3125, 0 fatal markers, artifacts
preserved, no overlap. Build exit 0. Exact change: `Shim_DeviceQI` (watchlist
Device..Device4 with header-verified IIDs, exact per-IID atomics, first+1/1000
records, single forward via per-table original, no extra QIs/retention) installed
at device-vtable slot 0 in the existing swap block (+CfgMarkValid, separate install
log); skip-branch `D3D12CreateDevice` lines gain the requested riid. Distinct
tables still skipped (reported limitation, none observed).
- Census result: game QIs **Device1 and Device4** on its device (both S_OK, same
  object) ~1 s post-swap; all 4 device creations request base Device; Device2/3
  never requested. Forwarding verified by stability (identical results, PASS).
- Interpretation (limited): a newer-IID request proves the interface was requested
  on the observed call — not that endpoints came from its APIs. Pre-swap QIs and
  distinct-vtable devices remain uncovered (none observed). Device4+ '1'-variant
  creation is now a LIVE hypothesis (was: untestable).
- Slot-map correction: `CreatePlacedResource1` belongs to **Device8**, not Device4
  (Device8 also adds `CreateCommittedResource2`); no Device8 QI observed, so
  placed-resource creation stays fully covered by hooked slot 29. Live surface is
  Device4 slots Committed1/Heap1/ReservedResource1 (+List1 for lists).
- Next justified experiment (not implemented): observe-only shims for Device4
  slots 56/57/58 on the already-swapped table (same direct-swap pattern, per-table
  originals, counters, no analysis) — UNORM creations with endpoint pointers appear,
  or counts stay 0 while endpoints stay unexplained. No DLSS success claimed.

**Filter-comparison run 20261003T181205Z:** PASS overall — `smallgrid` freeroam +
`thePlayer` (PID 17252), 1 frame marker (camera active), Present 1→3125, 0 fatal
markers, artifacts preserved, no overlap. Build exit 0. Exact change: CopyResource
detail line gains a `fulldesc` companion (dimension, depth/array, mips,
samples/quality, layout, flags; same guarded reads, same 10+1/500 cap). Filter,
hooks, forwarding, selection unchanged.
- Criterion-by-criterion for both #2 endpoints (1920x1080 fmt-28, dim=3/TEXTURE2D,
  depth=1, mips=1, samp=1/0): Dimension ✓, MipLevels ✓, size ✓, format ✓ —
  **filter exclusion REFUTED**. Flags differ (dst 0x21 / src 0x1), recorded without
  interpretation.
- Endpoints still absent from all creation/view/OM/copy/present records (dst: copy
  + 2 barriers; src: copy only), third consecutive run with this shape. Remaining,
  unstated as proven cause: pre-hook creation (deterministic 3.5 s boot window
  log-start→swap in all three runs; copies fire ~3.5 s post-swap) or an unhooked
  creation path. Nothing promoted; descriptors never treated as identity.
- Proposed next discriminator (not done): heap-timing correlation for placed
  targets (heap sizes bound resource creation timing) — requires verifying a heap
  hook slot first — or accepting viewless-copy-intermediate as a standing
  classification. No DLSS success claimed.

**Blind-window closure run 20261003T180616Z:** PASS overall — `smallgrid` freeroam
+ `thePlayer` (PID 296), **2 frame markers** (frames 43 ms apart — load-phase arming
  still needs a >3 s span), Present 1→3245, 0 fatal markers, artifacts preserved, no
  overlap. Build exit 0. Exact change: texture first-N cap 20→128 in all three
  creation hooks (128 > old 101 sample point with headroom; 1/100 sampling and exact
  counters retained; filter/fields/APIs/forwarding unchanged).
- Totals vs emitted: 53 placed + 0 committed + 0 reserved, max n=53, cap unreached —
  the creation record is COMPLETE (blind window closed), all placed, single shared
  device table (no distinct-vtable lines). 2 second-device creations, both covered.
- CopyResource #2 repeats (1920x1080 fmt-28, flags 0): endpoints absent even from
  the complete creation record. Remaining possibilities, no guessing: pre-hook
  creation (3.5 s boot window: log start → vtable swap), creation-filter rejection
  (mips/multisample unknown — copy log lacks those fields), or an unhooked path.
  Pointer match proves involvement only; nothing promoted.
- vp-bind: loading `inset=1` again; gameplay 1080p/4096/2048 → null. `boundScene` /
  `scene=1` / trigger / NGX silent by consequence. Next micro-step proposed (not
  done): full-desc fields (mips/samples) on CopyResource endpoints to test the
  filter-reject hypothesis. No DLSS success claimed.

**Observe-only CopyResource shim run 20261003T175001Z:** PASS overall — `smallgrid`
freeroam + `thePlayer` (PID 10244), 1 frame marker (camera active: frame 1 + patch
far 12500.0), Present 1→2765, 0 fatal markers, artifacts preserved, no overlap.
Build exit 0. SDK verified read-only: `CopyResource(pDstResource, pSrcResource)`,
slot 17 (order 16→17 from the established base). Exact change: typedef + per-list
struct original + slot-17 clone entry + observe-only shim (exact total, ENTRY 1/600,
detail 10+1/500 with pointer-only scene/set flags + guarded descs, single forward
via stored original) + CfgMarkValid 6→7 + install/fail accounting + totals `res`
field. No ExecuteBundle/Map/global/scan/selection/trigger changes.
- Installed and fired: slot diag captures genuine driver `copyResource=7FF9E8E822B0`
  (distinct neighbor of slots 16/26); `res=2` exact total (instOk=27). Forwarding
  verified by stability (PASS, 0 fatal).
- #1: 8x8 fmt-28 pair (loading, non-scene). #2: **1920x1080 fmt-28 UNORM
  whole-resource move** (loading phase) with `dstScene=srcScene=dstSet=srcSet=0` —
  endpoints match no tracked candidate, so nothing promoted (descriptors alone
  correctly not identity; a pointer match would establish involvement, not contents).
- Cross-reference: #2's dst has same-list barrier traffic, but neither endpoint
  appears in any RTV/creation/OM log while same-batch neighbors do — their views were
  created outside observed paths (pre-hook, unhooked table, or none). Zero further
  invocations is inconclusive about game silence (submit-miss gaps stand); the exact
  `res=2` counter proves delivery works when the game calls it.
- Next falsifiable experiment: attribute the untracked endpoints' views, or extend
  observation to submit-miss lists' record-time traffic. No DLSS success claimed.

**Device4 '1'-variant run 20261003T192340Z:** PASS overall — `smallgrid` freeroam +
`thePlayer` (PID 16516), 0 frame markers (camera dormant), Present 1→3245, 0 fatal
markers, artifacts preserved, no overlap. Build exit 0.
- Slot correction (verified, mandate premise fixed): filtered CINTERFACE enumeration
  gives CommittedResource1=**53**, Heap1=**54**, ReservedResource1=**55** (not
  56/57/58 — that misread came from unfiltered header text with non-Windows
  `#if/#else` duplicate declarations); consistent with live-verified 27/29/30.
  Signatures verified verbatim. `CreatePlacedResource1` belongs to Device8
  (with `CreateCommittedResource2`); no Device8 QI observed.
- Exact change: typedefs + per-table originals + three observe-only shims (texture
  branches mirror the base filter/caps; Heap1 logged as heap creation with
  directly-available heap fields) + one-time install inside `Shim_DeviceQI` on
  Device4 success ONLY when the returned table equals the hooked table (else
  skip+reset per the distinct-table rule) + CfgMarkValid. No scans/Map/retention/
  promotion/patching/config. Inherited 27/29/30 need no new hooks: install-time
  table-equality check proves the Device4 interface shares the hooked table.
- In vivo: install logged on the shared table; **20 Heap1 heaps observed** (64 MB
  DEFAULT backing stores among them); **zero CommittedResource1/ReservedResource1
  calls all run**. CopyResource #2 repeats identically (flags 0, no match).
  Forwarding verified by stability.
- Standing gaps (stated, not guessed): QI watchlist covers Device..Device4 only —
  a Device8 QI would be invisible (watchlist extension proposed as a data-only
  micro-step); heap log cap hit exactly 20 (blind window beyond); endpoints still
  absent from the now-complete base + '1' creation record → pre-swap persistence
  vs Device8-path creation remain open. Next: extend watchlist (Device5–8) and/or
  observe-only Device8 PlacedResource1/CommittedResource2 IF a Device8 QI ever
  appears. No DLSS success claimed.

**Texture-creation trace run 20261003T175927Z:** PASS overall — `smallgrid` freeroam
+ `thePlayer` (PID 15584), 1 frame marker (camera active: frame 1 + patch), Present
1→2525, 0 fatal markers, artifacts preserved, no overlap. Build exit 0. Exact change:
shared `CreateTexFilterMatch` (TEX2D/mip1/size + 6-format candidate list) plus a
bounded `Create*Texture` log branch in each already-hooked creation API (device,
resource, dims, format, flags, state, heap data where directly available; separate
20+1/100 counters). No new APIs/hooks/retention/promotion/patching; CopyResource
forwarding untouched.
- Pre-existing gap verified in source: all three creation hooks logged buffers only
  (`Dimension == BUFFER && >=1MB`), so no texture could ever appear. Single device
  table this run (no distinct-vtable lines) → hooked APIs cover all sharing devices.
- New logs work: 20 placed-texture creations (all `CreatePlacedTexture` — the engine
  places render targets; no committed/reserved candidates), then cap reached.
- CopyResource #2 repeats the prior pattern (1920x1080 fmt-28, all flags 0): dst has
  same-list barrier traffic; neither endpoint appears in creation/view/OM/copy/present
  records. With single-table coverage, the remaining possibilities are pre-hook
  creation or the 21–100 sampling blind window (n=20 cap hit during the loading
  burst) — views observed for same-batch neighbors favor the blind window, stated
  as hypothesis, not fact. Pointer match proves involvement only; nothing promoted.
- vp-bind (16 distinct events, cap hit): gameplay 1080p → null; new sizes 4096x4096 /
  2048x2048 (bound=null, shadow/atlas-class passes). `boundScene`/`scene=1`/trigger /
  NGX silent by consequence. Next micro-step proposed (not done): raise the texture
  first-N cap to close the 21–100 window. No DLSS success claimed.

**Backfill: runs 180616Z / 181205Z / 182050Z / 185159Z (verified from artifacts):**
all PASS, freeroam + `thePlayer`, 0 fatal/injection markers; frames 2/1/1/1;
CopyResource #2 present in each with the same untracked-UNORM shape.
- 180616Z (cap 20→128 all three creation hooks): texture totals 53/0/0 placed/
  committed/reserved, max n=53, cap unreached — complete creation record, yet
  endpoints absent. 2 frames 43 ms apart (load-phase arming still needs >3 s).
- 181205Z (CopyResource `fulldesc` companion): endpoints pass every filter
  criterion (dim 3, depth 1, mips 1, samp 1/0) — filter exclusion REFUTED. Flags
  dst 0x21 / src 0x1 recorded without interpretation.
- 182050Z (origin audit + requested-IID log field): base-device IID verified in-header
  (`189819f1-…85f7` = `ID3D12Device`); deterministic boot timeline (log→+2.2 s
  device→+3.5 s swap→+5 s burst→+7 s copy) proves nothing about pre-hook creation
  either way; `CreateHeap` rejected (shares activation/interface blind spots).
- 185159Z (QI census: watchlist Device..Device4, slot-0 direct swap, per-table
  original): game QIs Device1 + Device4 (both S_OK, same object); all creations
  request base Device; Device2/3 never requested. Device4+ '1'-variant creation
  upgraded from untestable to live hypothesis.

**Device8 run 20261003T193647Z:** PASS overall — `smallgrid` freeroam +
`thePlayer` (PID 16156), 1 frame marker, Present 1→3125, 0 fatal markers, artifacts
preserved, no overlap. Build exit 0.
- Census extended to Device5–Device8 (header-verified IIDs; Device5-7 add no
  resource creation). **Device8 QI observed** (first ever, S_OK, same object) and
  Device8 resource hooks installed (69/70) on the shared table. Forwarding verified
  by stability.
- In vivo: **20 Heap1 heaps observed** (64 MB DEFAULT backing stores); **zero
  CommittedResource1/ReservedResource1/CommittedResource2/PlacedResource1 calls**.
  CopyResource #2 repeats identically with no match. `res=2` exact total again.
- Endpoint liveness cross-links (193647Z): #2 src reappears as a barrier resource
  ~10 s later alongside a DIFFERENT UNORM copy pair (`copySrc=D9561E7D00 →
  copyDst=D9792598D0`, fmt 28/28) — persistent engine textures, still single-use
  each. D9561E7D00 has an RTV-view creation record + 46 present-feed hits
  (recurring terminal copy source) yet no scene adoption (its view format evidently
  differs — adoption requires a UNORM view); D9792598D0 was adopted as a *depth*
  candidate by the copy heuristic (pre-existing heuristic imprecision, no behavior
  changed). Roles stay UNKNOWN; nothing promoted.
- Format ground truth (header-verified, corrects "fmt-10 UI" labels used earlier):
  10=R16G16B16A16_FLOAT, 11=R16G16B16A16_UNORM, 28=R8G8B8A8_UNORM, 34=R16G16_FLOAT.
  Scene-set notes therefore record UNORM view + UNORM resource (no contradiction).
- Standing gaps: watchlist covered Device..Device4 at census time for Device8's
  absence claim — now closed by observation; pre-swap persistence remains the
  unobservable remainder for endpoint origin. Next: scene-side progress (boundScene
  at viewport) rather than more creation coverage — the creation surface
  (base+'1'+Heap1+Device8-resource) is now fully instrumented and silent. No DLSS
  success claimed.

## NGX init deadlock fix (2026-10-04, runs 20261004T164454Z/164741Z)

**What changed and why (`src/d3d12_hooks.cpp`, camera-accept block):** source audit
proved a circular deadlock in dlaa=0 mode — viewport patch requires upscaler-ready,
trigger requires patch-applied, `EnsureUpscalerInit` was reachable in legacy mode
only from `DoInjection` (which itself requires ready). Added `EnsureUpscalerInit(false)`
gated on `!g_dlaaMode` at validated camera acceptance (earliest proof of live
gameplay rendering). Supersedes the fix89-era NOTE (ECL/Present race fear is stale:
atomic single-attempt guard + zero Present-thread init paths in legacy mode);
DLAA sequencing untouched.
**Evidence:** build OK; run 164454Z PASS (freeroam+`thePlayer`, 0 frames — fix
correctly dormant on 5 rejects); run 164741Z PASS (freeroam+`thePlayer`, 1 frame)
logged the FIRST-EVER init-path evidence: `NGX init deferred - chain quiet 0f/120f`
x3, one per camera accept (19:48:05). No fatal markers either run; artifacts preserved.
**Known risks/limits:** quiet gate (120 camera-frame units) likely unpassable at
observed accept rates (frameCounter ~1/run) — deferral counts will measure this;
init has not yet touched the driver (zero added GPU risk so far).
**Reversal:** delete the 12-line block (comment + `if (!g_dlaaMode)
EnsureUpscalerInit(false);`) after `StartFrame();` in the camera-accept path,
rebuild via `src\build_asi.bat`, rerun. No config/artifact changes to revert.
**Remaining blocker:** quiet-frame maturation in camera-frame units.
**Next:** longer run to measure deferral trajectory; redesign gate units only with
measured evidence. No DLSS success claimed (no init attempt yet, no eval/handoff).

## Quiet-gate trajectory (2026-10-04, run 20261004T170532Z, 120 s)

**Evidence:** PASS (freeroam+`thePlayer`, 0 fatal, artifacts preserved). 2 frames +
3 accepts + 3 deferrals, ALL within 3 ms of each other (20:05:55.787–790, a loading
burst); zero further accepts in the remaining ~110 s. Every deferral reads quiet
`0f/120f`. No `gameplay settled`, no `SINGLE-DEVICE` attempt.
**Phase 2 answer (measurability):** detection/delivery/acceptance all work when the
game copies; the ~1-frame/run limit is game-side issuance cadence (bursts at load,
then dormant — Map-path alternative still unobserved by design) compounded by
counter semantics (`frameCounter` advances only on accepts).
**Phase 3 verdict (correctness, threshold untouched):** the counter is correctly
implemented per its definition but measures the WRONG quantity for its purpose —
camera-accept count, not render-graph stability. A chain that produced 14405
presents without new nodes still reads `0f/120f`; 120 accepts at observed rates is
effectively unpassable (needs ~30-60 bursts). The 120 number itself is not under
question; its clock is.
**Proposed redesign (NOT implemented — safety tradeoff needs approval):** track a
present-serial stamp alongside `g_lastNewChainFrame` (set at the same CopyTexBody
site) and compute quiet as presents-since-churn; keep the 120 count. Rationale:
presents advance ~90/s through stable gameplay, so 120 presents ≈ 1.3 s of proven
stability — same protective intent, reachable clock. Risk: init would touch the
driver ~1.3 s after churn stops instead of (effectively) never; fix89-era load-window
crashes motivate keeping a conservative margin (e.g., 600 presents ≈ 7 s) — exact
value is the approval decision. Reversal if implemented: revert the two stamp lines
+ `HooksGetQuietFrames`, rebuild, rerun. No DLSS success claimed.

## Quiet sentinel fix (2026-10-04, runs 20261004T171957Z/172116Z)

**What changed and why (`src/d3d12_hooks.cpp`, 3 lines):** `HooksGetQuietFrames`
used `g_lastNewChainFrame==0` as never-observed, but 0 is also a valid
`frameCounter` (chain first seen pre-first-camera). Added `g_chainObserved`
(set with `g_lastNewChainFrame` on new chain node); quiet now checks the flag.
Threshold (120) and units (camera-frames) UNCHANGED — pure correctness, zero
driver-contact change (still defers, just reports the true count).
**Evidence:** build OK (`[OK] Built` both artifacts); 171957Z PASS (0 frames/5
rejects — fix correctly dormant, 0 defers, 0 fatal); 172116Z PASS
(freeroam+`thePlayer`, 1 frame, 0 fatal) logged `chain quiet 1f/120f` x3 (one per
accept) vs prior `0f/120f` — copy at 20:21:40.685 precedes camera burst at
20:21:41.069, so quiet=1-0=1 proves the flag works. No `SINGLE-DEVICE` attempt
yet (gate still defers by design). Artifacts preserved in both run dirs.
**Reversal:** revert `g_chainObserved` decl + set + `HooksGetQuietFrames` ternary
to `g_lastNewChainFrame ? ... : 0`, rebuild, rerun. No config/artifact changes.
**Format ground truth (header-verified `shared/dxgiformat.h`):** fmt-45 =
`DXGI_FORMAT_D24_UNORM_S8_UINT` (DEPTH, not scene). The recurring 1920x1080
f45->f45 topo copy + `present-feed: last full-res src ... fmt 45` (every 60
presents) is the depth chain; scene terminal node remains unidentified in the
`CopyTextureRegion` path. Gameplay RTV binds are 1920x1080 fmt-28 LDR, but all
scene adoption requires fmt-11 UNORM → `boundScene=0` on every vp-diag.
**Remaining blockers to 1000 injections (all need approval — see agenda 3/5/6):**
(a) quiet clock (camera-frames effectively unpassable), (b) scene validity gap
(fmt-28/45 live vs fmt-11 gate vs NGX HDR expectations), (c) `DoInjection`
result copy unwired (`Real_CopyTextureRegion` never assigned). No DLSS success
claimed.

## Shadow Present-quiet diagnostic (2026-10-04, runs 20261004T173233Z/173336Z)

**Exact change (`src/d3d12_hooks.cpp`, additive only; committed partial, completed here):**
prior commit `9383609` added `g_lastNewChainPresent` (`volatile LONG64`) +
`g_chainPresentObserved` (`volatile LONG`) + `HooksGetPresentQuietFrames()`,
stamped in the same `newNode` block as `g_lastNewChainFrame`. This step wired
the bounded defer log (same 5-log cap):
`chain quiet %uf/120f (present-quiet %llup)`. Gate condition
(`HooksGetQuietFrames() < 120`), threshold, units, NGX call sites,
scene-selection, and injection path are byte-for-byte unchanged.
**Thread-safety basis (verified from call graph):** `CopyTexBody` runs on
engine recording threads (`Shim_CopyTextureRegion` → `CopyTexBody`; concurrent
ECL workers); `Hook_Present` increments `g_presentSerial` on the Present
thread; `EnsureUpscalerInit` readers run on both (camera-accept/DoInjection on
engine threads; `InjectAtPresentImpl` paths on Present thread). Plain fields
would race, so shadow storage copies the established serial pattern —
`LONG64` + `InterlockedExchange64` writes / `InterlockedCompareExchange64`
loads (same as `g_presentSerial`/`g_eclSerial`); flag is `LONG` +
`InterlockedExchange`/`CompareExchange` (same as `g_loadPhase`/`g_settledOnce`).
Width is 64-bit like `g_presentSerial`; quiet is `(now - last)` mod 2^64
(wrap-safe, practically nowrap). `%llu` matches existing `present=%llu` logs.
**Build/test evidence (verified facts):** build OK (`[OK] Built` both
artifacts, 20:32:20). 173233Z: freeroam+`thePlayer`, 1 frame, 3 accepts,
`1f/120f (present-quiet 1p)` x3 (paired clocks agree: single chain observation
preceded the burst; neither clock advanced after). Game exited 0xC0000005
mid-run after the measurement (Present 1→125, 0 fatal markers in our log);
prior identical-code runs show this flake is game-side (cf. 213754Z/201107Z
triages), but causation is unproven from one run — evidence preserved in the
run dir. 173336Z: clean PASS (exit 0, 0 fatal), 0 frames / 5 rejects / 0 defers
— camera-dormant, correctly NOT counted as a quiet-clock result.
**Limitations (hypotheses kept separate):** paired `1p`/`1f` do NOT prove any
Present interval is safe — both clocks share the same single `newNode`
observation, so agreement here reflects one shared event, not coverage of the
fix87/89/90 churn (CopyResource, submit-time lists, address reuse, and
creation churn still invisible to both clocks). No NGX contact occurred; no
threshold/unit/gating change.
**Reversal:** revert the defer-log line to `chain quiet %uf/120f` +
remove the two shadow fields, getter, and the two stamp lines in the `newNode`
block; `src\build_asi.bat`; rerun `--duration 30`. No config/artifact changes.
**Next blocker:** scene-validity gap is now the binding constraint to measure
an init attempt against (fmt-28 live binds vs fmt-11 adoption gate); the
unwired `DoInjection` result copy stays last. No DLSS success claimed.

## Scene-validity audit (2026-10-04, read-only; runs 173233Z/172116Z/170532Z)

**Verified facts (source + logs):**
- Gameplay binds (all camera-active runs): 1920x1080 fmt-28 `R8G8B8A8_UNORM`
 LDR (`vp diag bound=... bdesc=1920x1080 fmt=28`, `boundScene=0`, setcount 5–6).
 Display-sized copies seen: 1920x1080 f45→f45 (fmt-45 = `D24_UNORM_S8_UINT`
 depth, header-verified) and one 1920x1080 fmt-28→fmt-28 `CopyResource`
 (`#2`, dim=3=TEXTURE2D); one 1920x1080 fmt-87 (`B8G8R8A8_UNORM`) copy src
 (`native-usage copy #3`) on an unlinked side branch.
 **Correction (2026-10-04 recheck):** the swapchain itself is fmt-28
 (`present on real swapchain ... (format 28)`, backbuffer candidate fmt-28),
 so the Present path is format-consistent fmt-28→fmt-28 — but the
 pointer connection is UNPROVEN (CopyResource `#2` dst `80CD43B8B0` vs
 backbuffer `8042AD48B0` are different pointers; no GetBuffer probe by
 design). fmt-87 feeds no observed Present input; its consumer is unknown —
 do NOT cite it as the backbuffer. No pointer-connected fmt-28→fmt-87→Present
 path exists in the logs.
- Adoption that ran: 173233Z created scene RTV `80CD3EE760` (1902x1033
 fmt-11) + ALT `80CD5AB5A0` (1920x983 fmt-11) + ALT `80CD5B0860` (1920x1080
 fmt-11). So a gameplay-sized UNORM target EXISTS, but the gameplay bind is
 fmt-28, hence `SceneColorBound()` stays null.
- `native-candidate` already names better candidates with no new hooks:
 1920x1080 fmt-10 `R16G16B16A16_FLOAT` pair (`80CD5E99C0`/`80CD5E90B0`,
 roles=3=RTV+SRV views created, rtv=1 srv=2).
 **Correction:** roles/srv counts prove view creation only — NOT that a draw
 sampled them. Zero `hooks: barrier` lines name any candidate (legacy
 `Barrier()` helper is state-map-gated and `Real_ResourceBarrier` is null on
 this path); shim-side `TrackResourceBarriers` logs invocation counts, never
 per-resource PSR transitions; `roottable` logs carry GPU addresses with no
 resource linkage. So "rendered AND shader-sampled" is WITHDRAWN: view
 footprints only. fmt-34 (`R16G16_FLOAT`, MV-shaped) and fmt-10 siblings at
 1902x1033/1920x983 also present (same caveat).
- fmt-11 requirement origin: project assumption since the fix22-era rewrite
 (`34c95d7` introduced UNORM adoption + `scene color RTV` logs); no NGX/doc
 citation in code. NGX integration (`src/dlss_ngx.cpp`) sets NO
 format/colorspace parameter — only resources (color/depth/MV/output),
 render/display sizes, jitter, sharpness, MV-scale, and flags
 (MVJittered/AutoExposure, perf quality). Format is inferred by NGX from the
 resource desc. MV `R16G16_FLOAT` adoption already matches NGX convention;
 depth is format-agnostic in code (records real fmt, gates MSAA/unknown).
 Elsewhere the project already accepts FLOAT as scene (`ObservePersistentSceneColor`
 :1544-1545, batch filter :1180-1181, f10-pair adoption) — only creation /
 RTV-bind / copy-source adoption + `SceneSetNote` (:663) are UNORM-only.
- Output constraint (verified): `g_dlssOutFormat` is fmt-11 at display size
 (`:911`, `CreateDlssOut`); `DoInjection` copies dlssOut→scene, and D3D12
 copies require identical formats. Adopting a fmt-28 scene under the current
 output format would make the result copy illegal — a second reason fmt-28
 needs more than a gate flip.
**Correction from NGX contract review (2026-10-04):** fmt-28 is not
automatically invalid as DLSS color input just because it is 8-bit UNORM/LDR.
NVIDIA's DLSS Programming Guide permits LDR processing when the input values
are in range and perceptually encoded (not linear); it says to use
`NVSDK_NGX_DLSS_Feature_Flags_IsHDR=0` for that case, and HDR processing when
the input is linear or otherwise fails the LDR conditions. The DXGI format
alone does not reveal the transfer function or pixel-value semantics. The
current `NvDlssUpscaler::CreateFeature` starts create flags at zero and adds
MV-jittered/auto-exposure flags only; it does not explicitly set IsHDR. Thus
the present implementation is effectively relying on the LDR/default flag
path, but has not established that the game's fmt-28 values meet NVIDIA's LDR
conditions. Citation: [NVIDIA DLSS Programming Guide](https://github.com/NVIDIA/DLSS/blob/main/doc/DLSS_Programming_Guide_Release.pdf).

**Inferences (not proof):** fmt-28 may be a later LDR composite, and the
fmt-10 1920x1080 RTV+SRV pair may be a better pre-tonemap HDR candidate. The
existing observations prove bind/view roles only: RTV+SRV creation counts do
not prove which draw samples a descriptor, which candidate is the primary
scene, or whether fmt-28 is encoded correctly for LDR DLSS. Do not label the
fmt-28 target unsuitable solely from its format, and do not call fmt-10 the
scene solely from format/size.
**Unknowns:** which fmt-10 target (if any) is the terminal pre-tonemap scene
vs. a transient composite; the actual transfer function/value range of fmt-28;
which descriptor resources are sampled by the relevant draw calls; and the
resource/encoding that NGX would accept for the live scene. No game NGX init
or driver verdict has occurred.
**Proposed next experiment (NOT implemented):** do not add the previously
proposed Present-only role-count sampler as a decisive test: cumulative RTV,
SRV, or copy counts cannot identify the descriptor actually consumed by a
draw or establish fmt-28's encoding. The smallest decisive evidence source is
a successful GPU-frame capture of an ordinary freeroam frame, if a capture
path becomes available; inspect the candidate resources' actual contents,
resource states, descriptor bindings, and pass ordering into Present. PIX
launch and RenderDoc attach have failed in this environment, so do not repeat
those workflows unchanged. If no capture route is available, the next
project-local experiment should be a bounded descriptor shadow-map for only
the observed scene candidates: inventory shader-visible heap bases/increments,
track relevant SRV writes/copies and heap binds, then join observed root-table
handles to exact resource pointers on the same command list/draw window. Before
implementing, audit hook coverage and descriptor lifetime/copy paths; a
partial join must be labeled inconclusive, not a negative result. Neither
option alone proves fmt-28's color encoding; that requires capture/content
evidence or a separately justified NGX validation experiment. Files/risk/
rollback for the code option must be scoped after that read-only API audit.
Other gates remain separate: scene identification proves nothing about quiet
interval safety and does not wire the `DoInjection` result copy — verified
2026-10-04 (no source/config changes).

**Assignment follow-up (verified):**
- *fmt-28 viability:* format alone is inconclusive both ways. Official docs
 (Streamline `ProgrammingGuideDLSS.md` §4–5; DLSS Guide): DLSS takes
 render-res color + final-res output + depth + MV; `colorBuffersHDR = eTrue`
 is the assumed default; LDR mode (`IsHDR=0`, our implicit setting — create
 flags start 0, only MVJittered+AutoExposure added) needs non-linear in-range
 input, else banding/shift artifacts. Our fmt-28 is non-sRGB UNORM with
 unmeasured transfer/values — viability UNPROVEN either way.
- *fmt-10 draw use:* UNPROVEN (correction above). View counts ≠ draw use; no
 barrier/PSR/root linkage in logs.
- *Pointer path fmt-28→Present:* format-consistent, pointer-UNPROVEN. No chain
 may be cited.
- *fmt-11 gate + change consequences:* project assumption (fix22-era), not
 NGX. UNORM-only sites: `SceneSetNote` (:663), creation (:3074,:3107),
 copy-source (:7854), RTV-bind (:8132). A candidate change also forces
 `g_dlssOutFormat`/`CreateDlssOut` (identical-format copy law), display-size
 hysteresis revalidation, viewport/trigger pointer match, `IsHDR` decision,
 MV-range validation (`mvScale=mvW/mvH` assumes [0,1]-UV deltas — BeamNG MV
 range NEVER measured), depth re-gating. Multi-site + driver-contact: needs
 approval.
**Shadow-map coverage audit (negative stays inconclusive):** record-time-only
 shims (submit resubmissions bypass); `CopyResource` observe-only (2 calls);
 SRV counting without heap-bind join; handle-reuse aliasing; Map-path camera
 unobserved. Success = same-list root-handle→candidate join + PSR footprint
 on fmt-10 pair with fmt-28 as sink; failure = fmt-28 as upstream SRV into
 HDR, or fmt-10 pair with no joinable use after coverage-corrected sampling.
 No NGX contact in plan.

**Descriptor-correlation coverage audit (2026-10-04, read-only, no runs).**
Rollback note (correction): never revert doc work with whole-file checkout —
 the tree holds unrelated doc edits. Roll back any section below by deleting
 only its added paragraphs (exact hunks), leaving all other edits intact.
*Per-link mapping chain (root-table GPU handle → resource used by a draw):*
- L1 SRV write (`CreateShaderResourceView`, device slot 18, verified):
 OBSERVED for discovery (`ObserveNativeCandidate` role=2) but handle→resource
 is NEVER stored (unlike RTV: `g_rtvMap`+refresh). Same-handle overwrites,
 lifetime/reuse, and pre-hook creations are untracked. Devices: hooked
 post-init only.
- L2 descriptor copies (`CopyDescriptors`/`CopyDescriptorsSimple`): NOT
 hooked (slots unverified — fresh header count required before any proposal).
 Copies/overwrites silently stale any shadow map. Critical gap.
- L3 heap inventory (`CreateDescriptorHeap`, slot 14): OBSERVED (base
 CPU/GPU, increment via `GetDescriptorHandleIncrementSize`, count, flags).
 Pre-hook heaps missed; destroy/reuse unobserved.
- L4 heap bind (`SetDescriptorHeaps`, list slot 28): UNOBSERVED —
 `Hook_SetDescriptorHeaps` is defined-but-never-installed (no `cloned[]`
 entry, `Real_` never assigned) and `g_setHeaps` has no writer, so
 `HooksGetDescriptorHeaps` always returns 0. GPU handles cannot be attributed
 to a heap. Critical gap.
- L5 root-table bind (`SetGraphicsRootDescriptorTable`, list slot 32):
 OBSERVED record-time-only (submit resubmissions bypass via third-heap
 tables). Logs GPU handle+rootParam+list, but no heap and no root-signature
 range info — table span unknown, so consumed handles unenumerable.
 Critical gap.
- L6 draw timing (`Draw*`/`Dispatch`/`ExecuteBundle`): NOT hooked. Binds
 without a following draw marker prove nothing. Critical gap.
- L7 queues/devices: several direct queues exist (first-captured kept);
 copy/compute queues and pre-hook objects uncovered.
*Positive vs negative:* a same-list join (handle inside a bound heap range +
 recorded SRV write + draw marker) proves the draw COULD sample the
 candidate — never pixel-content proof. A no-match proves NOTHING (any of
 L2/L4/L5/L6 explains absence).
*Boundedness (state, not just logs):* SRV slot map cap 64 + heap table cap 16
 + per-list last-bind cap 8; window/present-stamp expiry with fault-guarded
 reads (no refs — weak pointers per teardown rules); O(1) inserts, no
 `GetDesc` on hot paths, sampled join at Present 1/60 under shared locks
 (exclusive only on insert, never held across guarded reads); log cap 1
 line/60 presents + distinct-event caps.
*Staged plan (proposals, NOT implemented):* Stage 0 = this audit (done).
 Stage 1 (logging-only, no selection/gate/render change): SRV
 handle→resource writer inside existing `Hook_CreateShaderResourceView` +
 last-heap recorder requires FIRST installing a `SetDescriptorHeaps` shim
 (slot 28, header re-verified) + sampled join at Present. No
 `CopyDescriptors` hook, no root-signature hook, no gating change. If Stage 1
 joins never resolve, STOP — coverage dominates; do not escalate.
 (Wording superseded by the Stage 1 revision below: a no-match is
 inconclusive and answers nothing about the scene.)
 Failure modes: handle-reuse aliasing (generation/present-stamp
 invalidation), heap destroy (dangling bases — guarded + expiry), own-heap
 pollution (exclude HUD/injection heaps by pointer), submit-time bypass
 (label inconclusive), log I/O (off hot path). Files: `src/d3d12_hooks.cpp`
 only. Targeted rollback: revert the SRV-writer hunk, the heap-shim hunk,
 and the join-sampler hunk independently; rebuild; rerun. Scene-audit
 conclusions stand: fmt-28 encoding unproven, fmt-10 views ≠ draw use,
 fmt-28→Present pointer path unproven. No NGX contact.

**Stage 1 revision (2026-10-04, read-only; supersedes the "STOP if unresolved"
 wording above, which is withdrawn — a no-match Stage 1 answers NOTHING about
 the scene and must not be read as one).**
*Heap clarification:* `Hook_CreateDescriptorHeap` IS installed (device slot
 14, verified) and already inventories heap base CPU/GPU + increment + count +
 flags. No new heap-creation work is needed. What is missing is (a) an
 SRV-slot map (writes observed, never stored) and (b) per-list heap-bind state
 (`Hook_SetDescriptorHeaps` defined-but-never-installed; `g_setHeaps`
 unwritten). Those two are the actual Stage 1 additions.
*Full link map GPU-handle → resource-at-draw (slot = SDK 10.0.28000.0
 DECLSPEC_XFGVIRT order; coverage = current hooks):*
- A. SRV write (`CreateShaderResourceView`, device slot 18): COVERED for
 discovery, stores no handle. Missed ⇒ slots unknown, join impossible.
- B. Descriptor copies (`CopyDescriptors*`, device slots UNVERIFIED — fresh
 header count required; currently unhooked): NOT covered. Missed ⇒ silent
 staleness; any match may name a dead occupant.
- C. Heap bind (`SetDescriptorHeaps`, list slot 28 per prior verification —
 must be re-confirmed by header count + log-check before install; currently
 uninstalled): NOT covered. Missed ⇒ handle unattributable to any heap.
- D. Root-table bind (`SetGraphicsRootDescriptorTable`, list slot 32):
 COVERED record-time-only (submit bypass). Logs handle+rootParam, no heap,
 no root-signature ranges (root signatures entirely unobserved). Missed
 layout ⇒ table span unknown; consumed entries unenumerable.
- E. Draw/dispatch (`Draw*`/`Dispatch`/`ExecuteIndirect`, list slots —
 unhooked, slots unverified): NOT covered. Missed ⇒ binds are not draws; NO
 join may be called draw-correlated.
- F. Queues/devices/lists: several direct queues (first kept), pre-hook
 objects and copy/compute streams uncovered. Missed ⇒ whole submission
 streams invisible.
*Narrow evidence rule:* an exact pointer match proves ONLY that a candidate
 is represented in an observed bound table — never that the shader sampled
 that entry, never that it is the DLSS scene input. A no-match is
 inconclusive wherever any of B/C/D/E/F gaps remain (i.e., everywhere in
 Stage 1).
*Staged experiment:* Stage 1a (source-only, no hooks, no run): add the slot-28
 re-verification + map-cap/static scaffolding behind a disabled flag and
 review the diff only. Stage 1b (logging-only run): SRV-writer (cap 64, weak
 ptrs, present-stamped) + heap-bind recorder (cap 8/list) + sampled
 same-list join at Present 1/60 (shared locks, exclusive-only inserts, 1
 line/60). Positive = same-list handle∈bound-range + recorded SRV write for
 that slot (representation only). Negative = no-match ⇒ INCONCLUSIVE by
 construction (coverage audit above); it does NOT eliminate any candidate.
 Stage 1 therefore cannot, even in principle, deliver a meaningful negative
 for the scene question — stated explicitly so it is never misread.
 Overhead: O(1) inserts, no hot-path `GetDesc`, off-draw work only; failure
 modes: reuse aliasing (stamp invalidation), heap destroy (guarded+expiry),
 own-heap pollution (pointer-excluded), submit bypass (inconclusive label).
 Files: `src/d3d12_hooks.cpp` only. Targeted rollback: revert the three
 hunks independently; rebuild; rerun. All prior conclusions and tree changes
 intact; no NGX contact; gates untouched.

## Route assessment + bind-survey experiment (2026-10-04, runs 180251Z/180445Z)

**Route decision (verified reasoning, committed before implementing):**
- *GPU capture:* no executable route — PIX/RenderDoc failures stand and may
 not be repeated unchanged; Nsight would be third-party software needing
 approval. Documented, not executed.
- *Full descriptor join (Stage 1 as scoped):* admitted in advance it cannot
 deliver a meaningful negative (four critical gaps need four new hooks on hot
 paths with artifact history). Deferred, not deleted — requires its own
 approval given hot-path expansion.
- *Chosen — Route C (bind survey on existing hooks):* extends `TrackOMBind`
 (OM shims fire ~58/run, proven coverage) with format-agnostic logging. No new
 installs, no selection/gate/render change. Per-class budget (first 3 per
 w×h×fmt, 32 classes) after the first build proved a global cap is consumed
 by loading binds. Plain-statics budget counters follow the established
 racy-benign log-counter practice.
**Evidence (180445Z PASS, freeroam+`thePlayer`, 1 frame, 0 fatal):**
 gameplay-window binds ARE format-mixed: 1920x1080 fmt-28 at present=52 AND
 1920x1080 fmt-10 `80DA595E30` at present=192 (scene=0, UNORM-set exclusion
 working as designed). The HDR scene target is LIVE during gameplay, not a
 stale load-time allocation. Loading binds (1902x1033 fmt-28 + fmt-11 with
 scene=1) confirm adoption still works where formats match. 180251Z (same
 code v1, global cap) showed only loading binds — a cap-design negative,
 correctly re-run rather than misread. Artifacts preserved in both run dirs.
**Interpretation (narrow):** fmt-10-at-gameplay proves the HDR candidate is
 bound, NOT that a draw samples it and NOT that it is the DLSS scene input —
 draw/root-signature gaps stand. fmt-28-at-gameplay keeps the LDR question
 open (encoding still unmeasured). What changed: "scene elsewhere" now has a
 live pointer (`80DA595E30`-class), not just view-creation counts.
**Reversal:** delete the survey block in `TrackOMBind`; rebuild; rerun. No
 config/artifact changes. Stage 1 descriptor-join stays proposed-only.
**Next (SUPERSEDED — authorization granted, implementation below):**
 MV-range via readback stays rejected; fmt-10 adoption + `IsHDR` + output
 format + forwarding + present-clock gate + Present retry were all authorized
 and implemented — see next section. No DLSS success claimed.

## MV-readback safety audit + fmt-10 use-trace (2026-10-04, read-only, no runs)

**MV pixel readback: NOT safe with existing paths — rejected, never
 "read-only".** `GetDesc` is metadata only. Pixel values need: (1) READBACK
 heap + staging texture creation (creation-burst crash class, fix20-22);
 (2) MV→COPY_SOURCE barrier on the engine list (state-track desync risk —
 the 6s-crash killer); (3) copy into staging on a list (submission-order
 hazard; own-queue = proven cross-queue race); (4) fence + CPU wait (stalls
 the calling thread — hitch/stall); (5) Map staging (legal) + min/max;
 (6) barrier back. Assumptions: weak MV pointer still alive (freed-pointer
 GPU copy = device removal), opaque placed-resource layout, single-reader
 locking. It perturbs timing/states of exactly what it measures, and range
 alone still would not prove direction, jitter inclusion, or temporal
 mapping. Verdict: do NOT build this.
**Use-trace instead (zero new code — existing 180445Z logs already hold it):**
 fmt-10 `80DA595E30` at present=192: placed-texture create (initState 0xC0) →
 SRV+RTV views → barrier 192(PSR)→4(RT) on list `80CD440BE0` → OM binds
 #22–25 slot=0 + 1920x1080 viewport both on list `80CD448430`
 (`list-corr` omSeq=vpSeq=1,2; barrier is same present/ecl window,
 different list). All observations are RECORD-time (shim callbacks during
 list recording), never execution proof — replay, queue order, list reuse,
 and intervening barriers can reorder actual GPU execution. Positive proves
 a raster pass was RECORDED into the HDR candidate at display size during
 gameplay — the strongest scene-role evidence to date. It does NOT prove
 draw sampling, final-scene role, or encoding. MV class check (same run): 1920x1080 `R16G16_FLOAT`
 targets re-adopted as ALT through present~192 (`80DA5939F0`-class) —
 format+resolution match NGX MV shape; value range/direction/jitter stay
 UNKNOWN by construction (no safe read path).
**Recommendation (staged, no approval needed for Stage 1):** Stage 1 =
 nothing to build — forensics above close the "is HDR live" question.
 Value-range validation is DEFERRED to post-handoff visual analysis
 (ghosting/smear direction reveals sign/scale errors with zero GPU risk),
 not to readback. Next code (logging-only, existing hooks): MV/depth
 freshness correlation from existing stamps (barrier/copy/bind sightings per
 present window) — same read-only class as the bind survey, per-class
 budgets, `src/d3d12_hooks.cpp` only; rollback = delete block, rebuild,
 rerun. Gates/selection/NGX/rendering untouched; no driver contact.
 Prerequisite order stands: color role → MV/depth freshness → output compat
 → init/feature/eval with return codes → handoff + visual → 20-min stability
 → second clean run (1000 consecutive confirmed frames, loading excluded).

## Authorized implementation stack (2026-10-04, runs 181934Z/182222Z/182403Z/183119Z/183426Z/183813Z)

**Authorization:** user authorized reversible project-local changes incl.
 gates, scene-format selection, NGX `IsHDR`, output format/copy path.
 System/install/third-party changes still need approval (none made).
**Hypothesis:** the live HDR scene is 1920x1080 FLOAT; accepting it plus HDR
 semantics plus per-list forwarding plus a reachable quiet clock completes the
 legacy path to an init attempt. Success per stage = new expected log markers
 with 0 fatals; failure = wrong-target adoption, crash, or device removal —
 stop and diagnose, do not bypass.
**Changes (`src/d3d12_hooks.cpp` + `src/dlss_ngx.cpp`, each hunk independently
 revertible; rebuild + `--duration 30` rerun; no config changes):**
- `IsSceneColorFormat()` (UNORM|FLOAT) at 6 sites: `SceneSetNote`,
 creation x2, handle-refresh map, copy-source fallback, RTV-bind adoption;
 scene RTV logs now print `fmt=%u`; `SceneSetNote` bind call passes real fmt.
- `g_dlssOutFormat` default → FLOAT (identical-format copy law); DLAA
 per-present backbuffer override untouched.
- `CreateFeature` flags start with `IsHDR` (linear-HDR input under LDR flags
 = banding per NVIDIA).
- `Barrier()` + `DoInjection` result copy forward via per-list shim
 originals (`FindCommandListShim`, shared-lock+SEH); unshimmed lists skip
 with the established diagnostic. Dormant-safe (unreachable until trigger).
- Quiet gate: present-quiet `< 600` (~6s, restores fix89's 600 on the
 reachable clock, past the +5s death window); defer log prints both clocks.
- Present-time init retry (both `Hook_Present`/`Hook_Present1`, 1/60,
 legacy-only, not-ready-only); camera path alone never re-fires post-burst.
 Atomic single-attempt guard covers all threads.
- `upscaler ready (render/display)` success log; set-notation on bind
 (gameplay fmt-10 enters rotation set without touching classification).
- `AllocateParameters` made load-optional (resolved, never called anywhere;
 recent redistributables omit the export) — unblocked first driver contact.
**Evidence (all PASS freeroam+`thePlayer`, 0 fatal; artifacts preserved):**
 181934Z: fmt-10 adopted (ALT chain, `fmt=%u` logs), display converged
 1920x1080→render 1286x723, fmt-10 bind scene=1 (set-membership fixed).
 182222Z: dormant-clean (0 frames). 182403Z: first quiet trajectory
 59p→119p→179p→churn→attempt; 182725Z: FIRST-EVER init attempt
 (SINGLE-DEVICE, QI blocked E_NOINTERFACE as predicted, proceeded) →
 alloc=0 failure diagnosed (never-called export) → fixed → 183119Z: load
 passes, `upscaler ready (1286x723→1920x1080)`. 183426Z: ready + scene=1
 confirmed. 183813Z (120s, 13325 presents): init OK; shim counters FROZEN
 post-load (instOk=34, buf=380…) while presents advance — see verdict.
**Replay-model verdict (verified facts):** record-time clone shims go blind
 in steady gameplay (engine replays pre-recorded lists/bundles: OM/vp/copy
 invocation totals freeze; only Present/ECL-queue/camera-burst hooks fire).
 Viewport patch (needs viewport shim + boundScene) and trigger (needs
 display-sized scene copy on record lists) cannot align post-load: 120s of
 gameplay produced 0 patch + 0 trigger legs. Chicken-egg noted: init needs
 post-load stability, but record-time activity lives only at load.
**Remaining blockers (in dependency order):** (1) patch/trigger placement
 incompatible with replay (needs Present/ECL-time mechanism or loading-window
 luck — luck rejected as strategy); (2) `CreateFeature` vtable-repair probe
 untested live (runs at first Evaluate); (3) MV range/direction/jitter +
 depth convention still UNKNOWN (readback rejected; defer to post-handoff
 ghosting analysis); (4) DLAA-flow veteran gates use stalled camera units
 (DLAA path only). No DLSS success claimed (no eval/handoff/visuals).

## Fence-timeline verdict + insertion-path audit (2026-10-04, read-only, no runs)

**Fence polling: no engine fences OBSERVED (corrected 2026-10-04 — earlier
 "none exist" wording was overstated).** Source inventory: every fence
 object in code is project-owned (bridge shared fences, b2 helper fences,
 `g_injFence`); there is no `CreateFence` hook and no queue `Signal`/`Wait`
 hook, so zero engine fence pointers are visible to poll. The game may well
 use fences — that is UNOBSERVED, not disproven. No polling code was added:
 it would have nothing to read. Deciding a bounded Signal/Wait observation
 hook (queue vtable, per-submit rate, not per-draw) is deferred to the
 ordering design, with hot-path risk explicitly weighed then.
**Replay composition is already directly measured (not inferred):** ECL
 detail records carry exact totals — early submissions `shimHit=1`
 (n=1..3), then `misses=22976 ptrReg=22976` (known list objects, live
 vtables not ours). The engine reuses the same list objects through
 non-shim tables at submit. Frozen record-time counters + advancing ECL
 serials are therefore measurement-backed. No ECL-composition sampler was
 added: it would duplicate evidence already in hand.
**Trigger/patch sizing incoherence (static, verified):** the legacy trigger
 requires a DISPLAY-sized scene copy, but a patched scene pass renders at
 RENDER size (1286x723) — post-patch copies can never satisfy it, and a
 pre-patch display-sized scene evaluated with render-sized NGX params is a
 dimension mismatch. The legacy render-scale trigger/patch pair is
 self-contradictory as designed; DLAA (native-size, no patch) semantics are
 the coherent first-eval target. Depth side-note: full-res depth candidates
 adopt from fmt-34 (`R16G16_FLOAT`) copy sources — MV-shaped, while NGX
 depth is single-channel; depth convention stays UNKNOWN.
**Synthetic smoke test:** present but DISABLED (`main.cpp:222` —
 concurrency caused DEVICE_RESET). Not re-enabled: proven device-reset
 history outweighs its isolation value while live init already succeeds.
**Consequence for insertion design:** any own-list eval needs engine state
 visibility it does not have (stale-frozen `g_resourceStates`,
 unattributable barriers) plus submit-order proof it cannot get without
 engine fences. Next safe work is the Present-time native-size eval DESIGN
 (states/sync/handoff spelled out before any code), not more hooks. No
 source/config/gate changes this turn; no NGX contact; tree and artifacts
 intact.

**Present-time native-size eval design (proposal, NOT implemented —
 first half solved, second half open):**
- *Solved half (verified reasoning):* intercept in `Hook_Present` BEFORE
 `Real_Present`: current backbuffer is predictably in PRESENT state (flip
 model requires it). Own list on the GAME queue (FIFO order, no cross-queue
 race): PRESENT→PSR(color) + evaluate LDR-mode DLAA at native size into an
 own fmt-28 dlssOut (known COMMON→UAV, self-consistent) + PSR→COPY_DEST,
 copy, COPY_DEST→PRESENT, then `Real_Present`. Every backbuffer/dlssOut
 transition is self-owned from a known entry state. LDR `IsHDR=0` matches a
 post-tonemap input (mode decision: HDR `IsHDR=1` work stays parked until an
 HDR input path exists).
- *Open half (precise blocker):* MV/depth inputs are engine-owned with
 frozen-unknown states — no legal transition exists without StateBefore,
 and no read API reveals it. Options ranked: (a) PRESENT-predictability for
 MV/depth (none — unlike the backbuffer, no contract constrains them);
 (b) state inference from last-observed + staleness (unsound across replay);
 (c) dedicated decoy resources (copies need source states — same wall).
 Until MV/depth states are knowable, own-list eval cannot legally bind them.
- *Open half (precise blocker):* MV/depth inputs are engine-owned with
 frozen-unknown states — no legal transition exists without StateBefore,
 and no read API reveals it. Options ranked: (a) PRESENT-predictability for
 MV/depth (none — unlike the backbuffer, no contract constrains them);
 (b) state inference from last-observed + staleness (unsound across replay);
 (c) dedicated decoy resources (copies need source states — same wall).
 Until MV/depth states are knowable, own-list eval cannot legally bind them.
- *Not attempted:* IsHDR/mode flip, dlssOut fmt-28 variant, own-list submit
 code, queue/fence additions. Design first, code after the state gap has an
 answer. Scene-audit and fence verdicts stand.

## NGX Init failure investigation (2026-10-04, runs 182725Z–202913Z + out-of-game probe)

**Verified facts (exact codes, no guessing):** first-ever init attempt
 182725Z (present-quiet trajectory 59p→119p→179p→churn→600p→attempt).
 `NVSDK_NGX_D3D12_Init` fails `0xBAD00001`; `Init_Ext` fails `0xBAD00002`
 (current-header enum: `FeatureNotSupported`|`Fail+1` vs `PlatformError`|`Fail+2`).
 Driver's own verdict via `GetFeatureRequirements`: rr=1 SUCCESS,
 supported=0, minHW=0x160, minOS='10.0.0' — SuperSampling SUPPORTED on the
 RTX 3050. DLL reports `GetAPIVersion=0x13`, snippet `0x1360600` (=310.6.0);
 still exports no `AllocateParameters` (made load-optional: never called).
**Eliminated with live evidence (each one variable, all PASS/0-fatal runs):**
 wrapper/QI (pristine clean device fails identically; QI failure is normal
 D3D12 behavior — vtable-repair theory FALSIFIED, write DEFUSED to log-only);
 AppId (241534720, 0, OptiScaler-generic 608174073 — identical);
 API version (0x13,0x15–0x1B — official macro still 0x15, sweep negative);
 entry point (classic vs Ext — different codes, both fail);
 NvAPI (`NvAPI_Initialize`=0); data path (models dir writable);
 GPU identity (game AND clean devices LUID-match 0x10DE RTX 3050);
 updater env (removal changed nothing); deny-list (unrelated entry);
 in-process interference (standalone `tests/ngx_init_probe.cpp` on explicit
 NVIDIA device, out-of-game: identical `0xBAD00002`).
**Correction (2026-10-07 — driver hypothesis withdrawn as stated):**
 driver repair/reinstallation was presented as the standing hypothesis. That
 was overstated and is hereby corrected: the driver is known-stable/current
 by user selection, and no evidence shows the driver itself is the cause.
 Proven-vs-hypothesis accounting follows.
**Proven module evidence (verified, read-only):** every NGX call ran code
 inside ONE module: `...\Bin64\plugins\nvngx_dlss.dll`, FileVersion
 310.6.0, NVIDIA-signed (DigiCert G4 code-signing, valid to 2028-07-06).
 `LoadNGX` resolves Init, Init_Ext, CreateFeature, EvaluateFeature,
 Shutdown, GetParameters, GetAPIVersion, GetSnippetVersion and
 GetFeatureRequirements from that single `m_ngxDll` handle (driver-store /
 System32 attempts fail first — logged). The DLL is self-contained
 (imports only system DLLs). `nvngx.dll` (classic driver-core module) is
 absent from System32/SysWOW64/DriverStore/NGXCore — reconciled: that does
 NOT mean no NGX code ran; the feature-snippet Init executed and returned
 real codes. Whether 310.x still needs a separate core is UNKNOWN.
**Proven results (exact codes; vendored 1.5.0 AND current DLSS-repo headers
 agree — |1=FeatureNotSupported, |2=PlatformError):** classic Init →
 `0xBAD00001` FeatureNotSupported ("SDK/feature not supported by
 system/hardware/API"); `Init_Ext` → `0xBAD00002` PlatformError
 ("underlying platform: graphics API, OS, system libs"); same-DLL
 `GetFeatureRequirements` → rr=1 Supported (minHW TU100-class, minOS
 10.0.0) on the enumerated RTX 3050; snippet self-reports API 0x13.
 Discovery-supported yet bring-up-refused is the fact to explain.
**Eliminated (live, one variable each, all PASS/0-fatal):** wrapper/QI,
 AppId ×3, API version 0x13+0x15–0x1B (official macro still 0x15),
 NvAPI (init=0), data-path writability, GPU identity (both devices
 LUID-match 0x10DE), updater env, deny-list, in-process factors
 (out-of-game probe identical). ProjectID route: UNAVAILABLE (no export
 in this DLL — eliminated without a run).
**Open, untested input variants (no driver/system changes):** dedicated
 fresh data subdir (stale-state interference); dual search paths; Init
 timing (load-time rejected: fix89 churn-death history).
**Standing unknown (NOT a finding):** why snippet bring-up fails on a
 supported adapter. Driver repair is one UNPROVEN hypothesis among others;
 no driver/installer/system change without explicit approval.
**Update (2026-10-07, run 20261007T163018Z):** dedicated fresh data subdir
 (`models\ScaleNG_ngx`, created OK, cleaned up after) → identical codes.
 Data-path staleness ELIMINATED.

## NGX bring-up resolution (2026-10-07; subagent-assisted audit + loader fix)

**Module reconciliation (verified, read-only):** ALL NGX calls resolve from
 ONE module handle: `Bin64\plugins\nvngx_dlss.dll`, FileVersion 310.6.0,
 NVIDIA-signed (DigiCert G4, valid to 2028), self-contained imports
 (system DLLs only). `nvngx.dll` (classic driver core) is absent from
 System32/SysWOW64/DriverStore/NGXCore — reconciled: that does NOT mean no
 NGX code ran; the feature-snippet Init executed and returned real codes
 (classic `0xBAD00001` FeatureNotSupported, Ext `0xBAD00002` PlatformError
 per matching-generation headers, vendored + current agreeing). Driver
 repair was proposed, then WITHDRAWN as unproven (driver is
 known-stable/current by user selection; no evidence implicates it).
**Correction from researcher subagent:** public SDK declares
 `Init(AppId,path,device,FeatureCommonInfo*=nullptr,version)` — classic
 4-arg form is valid; version macro still `0x15` TODAY (sweep 0x13–0x1B
 correctly negative). AppId: docs say use 0 until assigned; `Denied` (not
 observed) is the restriction code — AppId sweeps were vacuous.
**Decisive fix (reviewer hypothesis B, filesystem-CONFIRMED):** loader
 hardcoded only `nvlti.inf_*`; the real core sits at
 `nvltsi.inf_amd64_...\nvngx.dll` (489KB). Census-enumerated loader
 (`nv*.inf_*`) loads the CORE first; Init_Ext + NGX logging then SUCCEED:
 `Found matching adapter`, `feature created (1286x723→1920x1080)`,
 `shadow-eval ok #1..#1800+` consecutive, 0 failures.
**Also fixed along the way:** `AllocateParameters`-optional (never called);
 `g_upscalerInitAttempted` stuck-flag resets (auditor find);
 `IDXGIDevice`-QI failure is normal D3D12 behavior (vtable-repair theory
 FALSIFIED, write defused to log-only); UPLOAD-float-texture restriction
 (DEFAULT+staging design); backbuffer index 0-hardcode → current index
 (4-deep swapchain; prime suspect in the 165729Z GPU crash, exit code 1
 shown to be the runner's normal termination); Present1 gameplay path
 (Hook_Present goes quiet after loading — explains frozen Present-path
 diagnostics, shadow eval now wired into both); silent-return stage
 counters (desc-fault/size-mismatch/reset-fail, first-3 each).
**Reconciliation (reviewer hole #3):** the `Init_Ext` 5th-arg type concern
 is FALSIFIED by success — `FeatureCommonInfo` against the loaded CORE
 returns success, so the earlier Ext failures were wrong-module (snippet),
 not wrong-struct. Classic-vs-Ext code split likewise explained (snippet
 answers each entry differently). Ext-with-`FeatureCommonInfo` stands as
 the correct core call shape.
**Milestone (run 20261007T171602Z, PASS, 0 fatal):** 4200+ consecutive
 eval+handoff frames (ok #4200 at present 5041, 0 failures) — the
 1000-consecutive-frame criterion is SATISFIED for evaluation+handoff
 mechanics. INI `shadowHandoff` toggle added (default 1) for rigorous A/B.
 Remaining for completion: VISUAL verification (screenshot under test),
 20-minute stability, second clean reproduction. No visual claim yet.
**Correction (2026-10-07 — dimension honesty):** the "1286x723 → 1920x1080
 feature" log described NGX parameters, NOT reality: the engine renders
 full-res (viewport patch is replay-blocked) while eval received a
 1920x1080 backbuffer against 1286x723-declared params — a mismatch the
 API tolerated silently. The prior "upscaling" framing is WITHDRAWN. The
 path is honest native-resolution DLAA: shadow eval now re-targets
 render==display per display size (`UpdateSizes`, once per size, feature
 lazily recreates). Render-scale upscaling needs an engine-rendered
 sub-native input that does not exist while the viewport patch is
 replay-blocked — tracked separately, not conflated.
**Same-viewpoint A/B (implemented, under test):** 600-present windows
 alternate handoff ON/OFF in-run (INI+F8 still gate; transitions logged;
 per-eval handoff bit in ok log for exact ON-frame counting). Human
 override: `abWindow=0` (default) disables alternation for steady manual
 testing (F8/INI only); bot captures set `abWindow=60+.
**Human-test findings (2026-10-07, user driving):** captures 55/62/69 show
 healthy frames with HUD intact (handoff non-destructive in both states);
 shot 76 caught the user's own Start-menu keypress (discarded). Camera
 auto-orbits when idle and the user changed cars/drove (damage, viewpoints
 vary) — cross-run pixel A/B invalid; in-run alternation or F8 is the
 valid comparison. Display/graphics settings untouched by user.
**User observations as test inputs (2026-10-07, NOT diagnoses):** no
 heat-wave/warp (consistent with zero — not wrong-signed — MVs); still
 scene stable but soft (consistent with frozen jitter + zero MVs + no
 sharpening); motion blur on lettering clearing after stop + FPS-counter
 trails (consistent with zero-MV temporal smear + UI pixels inside the
 DLSS input, since our color is the post-HUD backbuffer). Verified in
 code/logs: every eval gets identical zero MV/depth/jitter (shadow params
 constant; camera-frame counter frozen so jitter never varies);
 `sharpness=0` with deprecated SDK sharpening; `MVJittered` flag set while
 inputs are unjittered zeros (moot: NGX subtracts jitter 0).
**Falsifiable test (built + exercised live):** live HDR/LDR A/B — F7 flips
 `IsHDR` and recreates the feature (`feature created ... HDR/LDR`).
 Run 20261007T183644Z (200s, PASS, 0 fatal, PID 2468): F7 pressed 7×
 (HDR→LDR→HDR→LDR→HDR→LDR→HDR), feature recreated each time without
 failure; steady `shadow-eval ok #15000 (present 17399 handoff 1)` with
 0 shadow-eval failures — ~16.5k consecutive presented frames evaluated
 with the output written into the presented backbuffer. This is the
 strongest streak so far (supersedes 6600) and proves the toggle is
 robust under repeated use, but it does NOT measure image quality:
 the HDR-vs-LDR visual difference is a human observation still pending.
**HDR/LDR A/B verdict (user, run 20261007T183644Z):** both modes
 "essentially the exact same with extremely minor differences" — color-mode
 mismatch is NOT a contributor to the softness. Corroborated in source:
 this SDK declares sharpening unsupported
 (`vendor/nvngx/nvsdk_ngx_defs.h:296` "Sharpness is not supported") and
 exposes no MaxDetail knob, so `sharpness=0` is not the cause either.
 Remaining candidates are temporal: jitter frozen at 0, and MV + depth both
 all-zero buffers (`memset` in the shadow-eval infra) → DLSS has no subpixel
 samples and no motion information.
**Objective sharpness probe (built + validated):**
 `scripts/sharpness_ab.ps1` (Laplacian variance + mean |gradient| on a
 central crop, F8 via keybd_event, scene guard on mean luma, discards
 scene-mismatch/blank pairs) and `scripts/diff_pair.ps1` (exact
 differing-pixel count). Metric validated against synthetic sharp/blurred
 images by `scripts/ab_selftest.ps1` — PASS (sharp lapvar 48910 vs soft
 21483, correct ordering).
**A/B sharpness measurement is BLOCKED — proven quantitatively:** run
 p2908, 5 ON/OFF pairs → 100.3% lapvar ratio ("no measurable delta"), but
 the NULL CONTROL (two captures 600ms apart with NO toggle,
 `scripts/sharpness_control.ps1`) differs on 87–96% of pixels
 (meanDelta 11–24 of 255). Camera drift alone swamps any plausible DLSS
 effect, so the earlier "45.5% softening" reading was a drift artifact and
 is WITHDRAWN. ON/OFF pairs showed 9–57% differing pixels — entirely
 inside the control band. Screenshot differencing cannot measure this while
 the scene moves; needs a frozen camera (viewport patch replay-blocked) or
 an in-engine freeze.
**Test-runner honesty fix:** `dlss_injection_recorded` FAILed forever because
 it only matched the retired legacy `hooks: DLSS injection recorded` marker.
 Replaced with `dlss_frames_evaluated`, which accepts either legacy markers
 or `shadow-eval ok` markers (counting `handoff 1` separately) and names the
 evidence path; outcome token is now `PASS_DLSS_EVAL` (`PASS_DLSS_INJECTION`
 still accepted for older logs). `--require-dlss` accepts either path.
 Reversal: revert the `autonomous_test.py` hunk; no runtime code touched.

## Visual A/B (2026-10-07; handoff ON vs OFF screenshots, no 20-min run per user)

**Evidence:** `%TEMP%\dlss_handoff_on.png` (run 171851Z, 6600+ oks) shows a
 healthy freeroam frame (chase view, coherent sky/grid/truck, no black,
 no garbage); `%TEMP%\dlss_handoff_off.png` (run 172210Z, INI toggle,
 4200+ oks p14764-only, 0 fails) shows a healthy orbit view with HUD.
 Pixel stats (80×60 grid): mean ON 134,135,140 vs OFF 120,120,121;
 6.6% strongly-differing samples — consistent with different viewpoints,
 NOT a corruption signal (viewpoints uncontrolled: runner has no camera
 control; chase vs orbit + differing vehicle state).
**Verdict (narrow, honest):** handoff is NON-DESTRUCTIVE — the presented
 image stays a coherent game frame with eval+handoff active every frame.
 A DLSS-effect delta is NOT measurable here: uncontrolled viewpoints plus
 DLAA-at-native with zero MV inputs is inherently subtle. No visual
 success claimed beyond non-destruction.
**Hygiene note:** OFF run log contains a stale overlapping process
 (p4172, ASI config-load only, 0 evals) from the prior run's shutdown
 window; TCom PID was single (14764); all eval evidence above is
 PID-filtered to 14764. Pre-launch tasklist check stays mandatory.
**Reversal:** INI `shadowHandoff` (dist + deployed, both at 1) or F8 live;
 code: handoff block + toggle as documented. Artifacts: both PNGs in
 `%TEMP%`, run dirs preserved.
 handoff writes shOut into the presented backbuffer (F8 toggles, default
 ON). Color-correctness (IsHDR=1 vs LDR input), MV/depth semantics (zero
 dummies in shadow path), and VISUAL verification all still open.
 Reversal: per-hunk revert as documented in each section; full list in the
 implementation-stack section. No DLSS visual claim yet.

## NGX CORE LOADER FIX — Init_Ext SUCCEEDED + feature created + evals run
 (2026-10-07, runs 20261007T165257Z/193649Z+, commit pending)

**Root cause (verified):** `LoadNGX` hardcoded ONLY `nvlti.inf_*` for the
 driver-store core search; this box carries the core at
 `nvltsi.inf_amd64_f0bf3af178442601\nvngx.dll` (489KB, vs 74MB snippet), so
 every Init ran against the fallback feature DLL. Filesystem census by the
 reviewer subagent found it; the fix enumerates every `nv*.inf_*` dir
 (other drivers: `nv_dispi.inf_*`), loads the first `nvngx.dll` found,
 logs `GetLastError` when the census itself fails.
**Result (run 20261007T165257Z, PASS freeroam+`thePlayer`, 0 fatal):**
 `loaded driver core nvngx.dll (...\nvltsi...)`; NGX's own log lines flow
 (`Found matching adapter with NVAPI physical GPU handle`);
 `Init_Ext SUCCEEDED`; `feature created (1286x723 → 1920x1080)`;
 `shadow-eval ok #1..#1800+` consecutive, every present, zero failures —
 first NGX evaluations on the wrapped game device. Mechanics proven; color
 correctness (IsHDR=1 vs LDR backbuffer input) explicitly NOT proven.
**Visible handoff (implemented, under test):** on eval success the own-list
 path now writes `shOut` back into the backbuffer (self-owned
 PSR→COPY_DEST / UAV→COPY_SOURCE, copy, restore to PRESENT) before
 `Real_Present`; failures still present the original. F8 toggles live
 (`handoff ON/OFF` logged) for A/B. States/queue/fences all self-managed;
 engine lists untouched. Reversal: delete the handoff block + F8 block +
 `g_shadowHandoff` (keep/shadow-eval untouched); rebuild; rerun.
 No DLSS visual-verification claim yet — pending screenshot evidence. Init scoreboard: classic `0xBAD00001` ×
 (AppId 241534720/0/608174073, versions 0x13+0x15–0x1B, both devices,
 in+out of game); Ext `0xBAD00002` × (same matrix + fresh dir). Every
 project-local input variant is now exhausted; remaining Init hypotheses
 need either NVIDIA-side answers or driver work — both external.
**Next safe in-project work (no driver changes):** (1) visual-baseline
 readiness — screenshot path in the test workflow for future A/B (test
 scripts only, zero game impact); (2) depth-format logging (fmt-34 vs
 D24-family for NGX depth convention); (3) legacy trigger/patch replay
 analysis stays parked behind eval success. No DLSS success claimed.
**Built along the way (all reversible, zero game impact observed):**
 shadow-eval own-list path (infra per-step HRESULTs; UPLOAD-float-texture
 E_INVALIDARG found → DEFAULT+staging-buffer upload design; 2-frame
 fence rotation, non-blocking skips); `ProbeCleanDeviceInit` (clean-device
 Init + LUID vendor + requirements query); `Init_Ext`+logging-callback path
 with classic fallback; `ngxApiVersion` INI plumbing (default 0x15);
 `tests/ngx_init_probe.cpp` standalone discriminator. Reversal per hunk:
 revert the named block, rebuild, rerun. No DLSS success claimed — init,
 feature, eval, handoff, visuals ALL still unverified live.

## Phase 2a Coverage Run D (2026-10-03T130805Z) — clone-integrity snapshot

**Code change (logging only, `src/d3d12_hooks.cpp`):** new `ShimIntegritySnapshot` +
`CaptureShimIntegrity` helper (forward-declared shim; registry fields copied under
shared lock, lock released, then guarded reads only). ECL detail records now carry
clone readability, slot-15 equality, actual-vtable region (`VirtualQuery`
base/size/protect/state/type), actual slot-15 target + module (`FROM_ADDRESS`,
refcount unchanged). Separate emit counter (cap 10); exact totals unchanged in
meaning. No install/dedup/forwarding/render change.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2525,
PASS, 0 fatal, 0 injection. Artifacts in `logs/test_runs/20261003T130805Z/`.
No overlapping BeamNG process (tasklist clean pre-run).

**Exact totals:** `hits=1`, `misses≈39500`, `ptrReg≈39476`, `ptrUnreg=26`,
`instOk=26, instDedup=1, instFail=0`. Skipped creations 10+ (cap). Placed large
buffers 20 (cap). `CopyBufferRegion src=` / camera / injection logs 0.

**Integrity records (10/10):** clones intact in every case (`cloneReadable=1`,
`cloneSlot15IsShim=1`); actual vtables are heap-private RW (`Protect=0x4`,
`State=0x1000`, `Type=0x20000`, sizes 0.8–3.2 MB) near each list object;
actual slot 15 is identical everywhere (`7FF9E8E81ED0`, module `7FF9E8C50000`) —
a structurally valid table forwarding to the genuine driver function, not our
shim and not the captured driver original. Our clone is intact but unreferenced.
Not vtable-restored-to-original; not never-registered. Remaining: third-party
vtable writer (e.g. game-side wrapper re-clone per object) vs address reuse with
stale-entry double-dedup block. QI-offset weakened (identical addresses, correct
driver vtable read at install) but not eliminated.

## Phase 2a Coverage Run E (2026-10-03T132110Z) — base-vs-derived pointer comparison

**Code change (logging only, `src/d3d12_hooks.cpp`):** `ShimIntegritySnapshot` extended
with submitted base-interface pointer fields (no extra QI, no refcount change, never
retained); snapshot reads base vtable + region guarded. Detail records now include
`baseList/baseVtbl/baseSamePtr/baseVtblIsActual/baseVtblIsOriginal/baseVtblIsCloned`
plus base region. Install/dedup/forwarding untouched. Relevant interfaces verified in
SDK header: `ID3D12CommandList` (base, `GetType`) → `ID3D12GraphicsCommandList`
(derived, `CopyBufferRegion`); creation `riid` logged as GraphicsCommandList.

**Build/test:** `src\build_asi.bat` PASS; `scripts\launch_test.bat --duration 30` →
`smallgrid` freeroam + `thePlayer`, BeamNGpy 1.35.1/TCom v1.26, Present 1→2525,
PASS, 0 fatal, 0 injection. Artifacts in `logs/test_runs/20261003T132110Z/`.
No overlapping BeamNG process (tasklist clean pre-run).

**Exact totals:** `hits=1`, `misses≈39500`, `ptrReg≈39455`, `ptrUnreg=45`,
`instOk=47, instDedup=1, instFail=0`. Skipped creations 10+ (cap, other-device).
Placed large buffers 20 (cap). `CopyBufferRegion src=` / camera / injection logs 0.

**Decisive detail records (10/10):** `baseSamePtr=1` everywhere — submitted base
pointer EQUALS the QI'd derived pointer; `baseVtblIsActual=1`,
`baseVtblIsOriginal=0`, `baseVtblIsCloned=0`. Single shared vtable per object;
the live table is the heap third value, not ours, not the driver's. QI-offset
hypothesis **eliminated** for pointer identity (same addresses, correct driver
vtable read at install). Remaining: third-party per-object vtable writer vs
address reuse with stale-entry double-dedup block. Actual slot-15 identical
everywhere (genuine driver function, same module) and stable per pointer across
resubmissions — consistent with a live wrapped object, but reuse not excluded.

## Verified

- `scripts\launch_test.bat --duration 30` completed a true live BeamNG.drive render/Present integration run. Detailed report: `logs/test_runs/20261002T182709Z/result.json`.
- Build, required artifacts, deployment, BeamNGpy/TCom connection, `smallgrid` and vehicle startup, plugin load and initialization all passed.
- The fresh log contained 42 current-process D3D12/Present signals; the Present counter advanced from 1 to 3605 across 35 snapshots; there were no fatal markers.
- Outcome `PASS` means the game rendered/presented and ScaleNG initialized. This does not prove DLSS output.
- Camera-CB far-plane validation limit increased to 100000 (fixes 12500 rejection); unit test `tests/camera_cb_validation_test.cpp` passes.
- Permissive `CopyBufferRegion` validation (`numBytes >= 1616` at any `srcOffset`) and permissive velocity-CB validation implemented in `Hook_CopyBufferRegion`.
- **Slot fixes verified (Phase 2a):** Device vtable uses SRV=18, RTV=20, Committed=27, Placed=29, Reserved=30; per-resource shim uses Map=8, Unmap=9 (dormant).
- **Global MinHook paths removed (verified absent):** No `GLOBAL`, `s_global`, or MinHook-on-Map paths remain.

## Not yet verified (updated 2026-10-04; supersedes stale entries below as noted)

- DLSS injection and visual quality remain unverified; do not report a DLSS-success result. End-goal criterion (not yet met): eval + handoff confirmed for >=1000 consecutive presented frames, no device loss/fatal/missing-output, with A/B visual check over >=2 clean runs.
- No captured-frame A/B comparison has been produced by the runner.
- BeamNG 0.39.3 is tested with BeamNGpy 1.35.1 (TCom v1.26). The isolated `.venv-test` is intentional; BeamNGpy 1.36 uses protocol v1.27.
- The existing game `Bin64\dxgi.dll` was reversibly renamed to `dxgi.dll.scaleng-disabled-20261002` to avoid conflict with UAL ASI loading. This is machine state, not a tracked project file.
- Camera issuance is game-side bursty (~3 accepts in one 3 ms loading burst per 120 s run, then dormant); `CopyBufferRegion` record-time delivery is proven when the game issues (buf=108/tex=288 shims fire). `frameCounter` = camera-frames only (2 vs 14410 presents in 170532Z) — correct per definition, wrong clock for stability.
- NGX init has never proceeded past quiet-gate deferral (first deferral 164741Z, still 0f/120f in 170532Z); feature creation/evaluation/injection unreached. Quiet 0f has two stacked causes: (a) sentinel bug (`g_lastNewChainFrame==0` reads as never-observed even when observed at frame 0), (b) clock stall (no camera frames after burst to mature even a corrected count).
- Scene discovery mismatched to live game data (170532Z): gameplay binds 1920x1080 fmt-28 LDR + copies 1920x1080 fmt-45, but all scene adoption requires fmt-11 UNORM; `boundScene=0` on all 12 vp-diags despite setcount=6. Viewport patch (needs boundScene + upscaler-ready) and trigger (needs patch + mv + depth + ready) therefore unreachable even after init.
- `DoInjection` result copy is unwired by design (`Real_CopyTextureRegion` never assigned; logs "no per-list forward wired" and returns before the injection marker). Injection marker is currently unreachable even if all upstream gates passed.
- (Stale, retained for history: earlier notes claiming zero frame markers / zero validation ever observed are superseded — frames, validation, and patches are logged in camera-active runs; see run sections.)

## Agenda (updated 2026-10-04 — end-goal order)

1. Baseline green: keep `launch_test.bat --duration 30` (or justified longer variant) PASS with freeroam + `thePlayer`, 0 fatal, artifacts preserved.
2. Frame measurability: DONE for detection/delivery/acceptance (game-issue-limited); counter-semantics discrepancy diagnosed (camera-frames vs presents). Next: sentinel correctness fix only (no threshold/unit change), re-measure quiet trajectory.
3. Quiet gate: establish correctness + meaningfulness BEFORE any unit/threshold change. Sentinel fix is step 1; present-clock redesign stays PROPOSED-NOT-IMPLEMENTED pending approval (driver-contact timing risk).
4. NGX init: after quiet legitimately clears, record attempted/deferred/failed/initialized distinctly; address first concrete failure before moving on.
5. Feature + inputs: verify resolutions/MV/depth/exposure/jitter/camera/lifetimes against live data (fmt-28/45 vs UNORM-11 mismatch is the known first validity gap).
6. Eval + handoff: trace one eval to the presented resource (sync/states/queue coverage, no overwrite/bypass); wire per-list forwarding only with evidence + approval if it touches GPU submission.
7. Visual verify + repeat: A/B at same scene/settings, coherent + repeatable over >=2 clean runs, captures/logs preserved.

## Run the current test

```bat
scripts\setup_test_env.bat
scripts\launch_test.bat --duration 30
```

Use `--require-dlss` to require a DLSS injection marker. Absence yields
`INCONCLUSIVE_DLSS`, not DLSS success. Refer to [the test guide](../scripts/README.md)
for all options and outcomes.

## Submit-queue latch + queue-identity correction (2026-10-04, run 190017Z)

**Verified facts:** `g_graphicsQueue` has five writers (ECL-first,
 CreateCommandQueue capture, `InjectAtPresentImpl`, IDENTITY probe,
 swapchain hook). Run 180445Z proved capture-time ≠ steady-state
 (captured `804289D940` vs 13,200 ECLs on `80005B3040`); run 190017Z
 reproduces the pattern (captured `E2E8B9DB70`, ECLs + latch on
 `E2E89A5500`). Own-list submit ordering against the first-captured pointer
 would risk the proven cross-queue crash class.
**Change (`src/d3d12_hooks.cpp`, additive, no consumers yet):**
 `g_gameSubmitQueue` single-writer latch (pointer-CAS, once-log) inside the
 validated GAME ECL branch. Build OK; 190017Z PASS (0 frames dormant, 0
 fatal), latch logged first observation, init + `upscaler ready` unaffected.
**Reversal:** delete the declaration + the 3 latch lines; rebuild; rerun.
**Agenda:** next is the Present-time native-size eval design's open half
 (MV/depth entry states) OR a bounded queue Signal/Wait observation hook
 proposal with hot-path risk analysis — whichever first yields ordering
 proof. No DLSS success claimed (init only; no feature/eval/handoff).

## Machine-specific setup caveat

For the verified ASI path, `C:\Games\BeamNG.drive\Bin64\dxgi.dll` had been
reversibly renamed to `dxgi.dll.scaleng-disabled-20261002`. Keep the proxy
disabled for this ASI test unless testing coexistence specifically. Do not
blindly copy this workaround to another installation without checking its files.

## Real MV/depth inputs: fail-closed path + live findings (2026-10-07, runs 190954Z–193154Z)

**Verified: what each successful eval receives TODAY.** The only eval path
 that succeeds is shadow-eval (`ShadowEvalAtPresent`); the legacy
 `DoInjection` path records 0 injections in every recent run (its
 viewport-patch + veteran gates never all pass). Shadow eval feeds NGX:
 color = current presented backbuffer (fmt-28 LDR, post-HUD), depth +
 MV = OWNED zero-filled placeholders (`g_shMv` fmt-34 R16G16_FLOAT,
 `g_shDepth` fmt-41 R32_FLOAT, `memset 0` once, parked in PSR forever),
 output = owned fmt-28 UAV, jitter 0/0, mvScale 1.0x1.0 (zeros make scale
 moot). Proven by source (`d3d12_hooks.cpp` infra + ep block) and
 PID-filtered logs (e.g. run 190954Z p3668: owned placeholders
 1920x1080 fmt-34/41). Zero buffers are NOT valid DLSS inputs (NVIDIA
 treats depth+MVs as mandatory); they are why motion smears and stills
 stay soft. HDR/LDR + sharpness already eliminated (STATUS visual-A/B
 sections); remaining cause is temporal (frozen jitter + zero MV/depth).

**Verified: engine resources that COULD supply real inputs.** MV:
 `g_mvResource`/`g_mvResourceAlt` adopted at `CreateRenderTargetView`
 (fmt-34 R16G16_FLOAT) + silent re-adopt at `OMSetRenderTargets` bind
 (track-by-bind, no log) + registry re-adopt; sizes vary by phase
 (1902x1033/1902x983/1920x983 loading → 1920x1080 gameplay; some runs
 never reach display size). Depth: `g_depthResource` adopted at SRV
 creation + full-res-copy DEST heuristic; **live-measured format fmt-45
 = R24G8_TYPELESS** (run 192627Z p3844: `candDepth=...(45 1920x1080)`) —
 NOT directly NGX-compatible (NGX depth is single-channel; typeless
 depth+stencil needs an R32F copy or an R24X8/R32F view). States:
 barrier traffic IS tracked live on the shim path
 (`TrackResourceBarriers` refreshes `g_resourceStates` + MV/depth stamps;
 the old "frozen-unknown" blocker note is outdated) but shim coverage of
 engine lists is unproven, so untracked = fail closed. Freshness gates:
 MV age <=10 presents, depth age <=20000 frames (legacy thresholds).
 Jitter: `g_currJitter` (Halton 2/3) computed per camera frame but the
 render is UNJITTERED (`ApplyCameraCbJitter` gated on `g_dlaaMode`,
 shipped `dlaa=0`), so shadow eval correctly passes 0/0; `mvJittered=1`
 create flag with jitter 0 is moot. MV value range/direction/jitter
 inclusion: UNKNOWN (no readback by policy); assumed UV [0,1]
 prev-minus-cur per code comment + legacy `mvScale=W/H` (hypothesis,
 untested). Depth convention (inverted?): UNKNOWN, `DepthInverted` NOT
 set. Scene-color correspondence: shadow color is the PRESENTED frame
 (post-composite + HUD) while MV/depth describe the pre-composite scene
 render — approximately pixel-aligned at 1920x1080 but UI pixels carry
 scene MVs (FPS-trail mechanism already observed).

**Implemented (fail-closed, default OFF, known-good preserved):**
 `g_shadowRealInputs` (INI `realInputs=0`, F9 toggles live, logged).
 When ON, each eval validates: liveness (`SafeGetDesc`), MV fmt-34 +
 display size (primary, else ALT — engine rotates MV textures every run),
 depth known non-MSAA fmt + display size, freshness gates, tracked entry
 states via `LookupTrackedStates` (lock lives outside the `__try` frame —
 C2712 forbids unwinding objects with `__try`; first build caught this).
 Pass → engine MV/depth via tracked `Barrier()` to PSR + `mvScale=W/H`,
 restored to entry states in the same list (FIFO order = engine safety).
 ANY failure → owned zeros (byte-identical proven path). Diagnostics
 (bounded, never per-frame): source transitions (`REAL`/`ZERO` +
 reason + candidate identity + mvScale) and a periodic line piggybacked
 on the 600-ok cadence (pointers, mvScale, frame, mv/depth ages). The ok
 line format is UNCHANGED (runner parser intact). `IUpscaler` untouched.
 Files: `src/d3d12_hooks.cpp` (F9, selection, barriers, logs),
 `src/d3d12_hooks.h` (`realInputs`), `src/main.cpp` (INI parse),
 `dist/ScaleNG.ini` (`realInputs=0`).
**Reversal:** set INI `realInputs=0` (or F9); full revert = delete the
 F9 block + real-input selection/diagnostic hunks + `LookupTrackedStates`
 + config plumbing; rebuild; rerun. Zero-path behavior is untouched.

**Live results (same scene/vehicle/settings: smallgrid + pickup).**
 - 190954Z (90s, flag off): PASS_DLSS_EVAL, 22-proxy oks (19 counted),
   0 fatal — regression baseline for the new binary hunk (inactive).
 - 191239Z (90s, flag on but INI NOT deployed — deployed INI stale):
   `why=off` all run — process finding: deployment copies ASI/helper but
   NOT ScaleNG.ini; dist↔deployed INI must be synced by hand (copied;
   `.pre-realinputs-bak` kept in game dir).
 - 191619Z (120s, flag on): 0 evals — run invalid for the experiment:
   transient allocator-reset failures at startup + a 2048x2048 fmt-10
   scene ALT adopted → `AdoptDisplaySize(2048,2048)` while bb stays
   1920x1080 → every present exits at size check. Pre-existing adoption
   hazard (any >=1000x500 scene-format RTV redefines display), untouched
   by this work; run A saw 37 2048-lines without adoption taking hold.
   Fail-closed guards behaved (`why=mv-retired`, 0 REAL, 0 crashes here).
 - 192231Z (120s, flag on): PASS_DLSS_EVAL, 26 oks, 0 fatal, 0 REAL —
   guards correctly rejected all run (`why=mv-format` x27: candidate
   alive but not fmt-34). Stable: real-path validation adds no
   instability when it rejects.
 - 192627Z (120s, flag on): GAME_CRASHED_AFTER_PLUGIN_INIT, exit
   0xC0000005. Crash on FIRST eval with ZEROS (`why=mv-format` →
   fallback, no engine barrier executed by my code — provable from the
   log sequence), fault inside NGX `CreateFeature` (SEH 0xC0000005).
   Baseline rerun below shows this is not reproducible via my path.
 - 193154Z (90s, flag off, current binary): PASS_DLSS_EVAL, 22 oks,
   0 fatal — baseline STABLE; the 192627Z crash is not caused by the
   real-input hunk (inactive here; delta vs proven build is F9 check +
   two bounded logs). Crash cause UNDETERMINED: pre-existing
   CreateFeature flakiness (first observed) vs run-specific driver
   state; no engine-state corruption by my code is possible on the
   logged path (zero fallback touches nothing engine-owned).
**Bonus verified facts from the new diagnostics:** (1) MV UAF race is
 live and microsecond-scale: a candidate passing validation read dead
 (fmt 34→0) one log line later; (2) owned-placeholder identity prints
 misled once — fixed to print selected candidates; (3) depth fmt-45
 finding above blocks direct depth use.
**Limitations / NOT claimed:** REAL never engaged — no image-quality
 comparison exists yet; MV direction/scale/jitter-inclusion, depth
 convention/conversion (R24G8→R32F copy or view), jittered rendering,
 and scene-vs-presented alignment are all still open. Next: R32F depth
 conversion + MV TOCTOU hardening (validation-to-bind race), then
 re-attempt REAL on a stable baseline. No image-quality improvement
 claimed; no 20-min run (user directive).

## REAL ENGAGED (2026-10-07, run 20261007T194541Z, p2256, 120s)

**What happened:** with `realInputs=1` deployed (dist stayed 0), the
 fail-closed path SELECTED real inputs at 22:46:29 and NGX evaluated
 them successfully **7200+ consecutive times with handoff=1**
 (`ok #7200 (present 9945 handoff 1)`), 0 device-removal, normal game
 exit. This is the first non-placeholder evaluation. Deployed INI
 restored to `realInputs=0` immediately after.
**Proof points (PID-filtered):** MV `80DA99BFD0` fmt-34 R16G16_FLOAT
 1920x1080, placed texture + RTV-created (`rtv-provenance`) + adopted as
 MV ALT; depth-slot `8045DA8860` fmt-34 1920x1080, likewise RTV-created.
 NO conversion/copy (direct resources). States via tracked `Barrier()`
 to PSR + restore in the same list. Params: mvScale 1920.0x1080.0
 (UV hypothesis), jitter 0/0, HDR create flags, sharpness 0.
 Same-present engine-render proof for the depth-slot resource at present
 2745 (barrier 192→1024→RT + RTV bind survey, then our eval). MV
 same-frame proof is weaker (stable pointer + mvAge 1, but the frame
 clock is frozen and no per-frame MV touch logs exist in the REAL era).
**CRITICAL role correction:** the depth-slot resource is a SECOND
 VELOCITY BUFFER (`motion vector RTV 8045DA8860 ... (ALT)`), adopted via
 the copy-DEST heuristic — NOT true depth. So REAL = real MV +
 velocity-as-depth. True R24G8_TYPELESS depth conversion is still open
 (step 4); depth-convention questions are untouched.
**Zero-era anomaly in the same run:** presents 842→~2745 faulted
 ~1904x (`EvaluateFeature FAULTED SEH 0xC0000005`) with ZERO fallbacks
 (`why=depth-size`: transient 359x379 fmt-28 depth candidate), then
 instant success from the first REAL eval. Correlation zero-fault →
 REAL-success is logged but CAUSALITY IS UNCLAIMED: confounds include
 upload-completion timing, driver warmup, first-feature settling. The
 192627Z CreateFeature fault + this zero-fault cluster mean the baseline
 is NOT proven crash-free. Next: rerun zeros to test recurrence.
**Runner note:** 1904 unbounded `FAULTED` lines tripped `no_fatal`
 (correctly — that many driver-internal AVs, all SEH-caught, deserve a
 FAIL token even with later success). Consider bounding the FAULTED log
 only after the zero-fault cause is understood; not changed now.
**Reversal:** unchanged (INI flag; hunk revert as above). No source
 changed this turn.
