# ScaleNG.Drive - Completed Work Log

> Generated 2026-08-22. Companion to `User/correctness.md`.
> Status: `[DONE]` = implemented this session | `[PARTIAL]` = started but incomplete | `[NOT DONE]` = not attempted | `[BLOCKED]` = attempted but blocked by something else

---

## Immediate Priority Queue

| # | Item | Status | Notes |
|---|------|--------|-------|
| 1 | Display-size hysteresis | [PARTIAL] | Basic stability counter exists (fix12). Missing: separate accepted/candidate variables, explicit transition logic |
| 2 | Bridge game sync | [DONE] | fix27: game queue opens shared fence + enqueues GPU-side Wait(v2) before copy-back list. This was THE missing cross-queue sync causing all post-load crashes |
| 2b | NVIDIA adapter enumeration for bridge device | [DONE] | fix28: bridge device explicitly created on NVIDIA adapter (was nullptr=maybe AMD iGPU on hybrid laptop) |
| 2c | appId corrected to proven-working value | [DONE] | Changed from appId=1 to appId=241534720 (0xE658700) matching harness test_mini which succeeded |
| 3 | Bridge allocator reuse | [NOT DONE] | bridgeAlloc still Reset without proving GPU completion. Needs allocator ring or fence-tagged reuse check |
| 4 | Direct Release() of in-flight resources | [PARTIAL] | Graveyard pattern exists for dlssOut (fix17). But g_gameOut/g_brOut/g_brColor/g_brDepth/g_brMv have no retirement protection. Full fence-tagged queue not implemented |
| 5 | Renderer-generation tracking | [NOT DONE] | No generation counter exists anywhere |

## Phase 0 - Debugging Baseline

| Item | Status | Notes |
|------|--------|-------|
| Known-good build recorded | [DONE] | passive=1 run: 3+ min stable, zero faults |
| Passive mode stable | [DONE] | Confirmed by control run |
| Camera-CB patching works | [DONE] | Working since session 8, still active in all builds |
| NGX initialization succeeds | [DONE] | On bridge device (our clean device), not wrapper |
| DLAA evaluated at least once | [DONE] | Frame 9, fix14-era run |
| Preserve as regression baseline | [DONE] | Git tag/commit available |
| Crash classification system | [NOT DONE] | Ad-hoc classification used but no formal taxonomy |
| Improved crash logging | [PARTIAL] | Step breadcrumbs + fault addr/module/RIP/RAX/RCX added (fix15). Full context (generation, fence values, all resource states) not implemented |

## Phase 1 - Obvious Correctness Bugs

| Item | Status | Notes |
|------|--------|-------|
| Display-size hysteresis | [PARTIAL] | Stability counter exists but lacks accepted/candidate separation. Still susceptible to oscillation between two sizes |
| Remove resolution hacks | [NOT DONE] | No hardcoded 1920x992 remains (dynamic discovery works) but renderScale config path still computes scaled W/H even in dlaa mode |
| Remove dead synchronization code | [NOT DONE] | COMMON→COMMON barriers, unreachable branches still present |
| Eliminate direct Release() of in-flight resources | [PARTIAL] | dlssOut uses graveyard. But shared bridge resources (g_brColor etc) released directly during EnsureBridge teardown without fence wait |

## Phase 2 - GPU Lifetime Management

| Item | Status | Notes |
|------|--------|-------|
| Fence-tagged retirement queue | [NOT DONE] | Graveyard is untagged (releases after ANY fence wait, not resource-specific fence value) |
| Track last-use fence per resource | [NOT DONE] | No per-resource fence tracking exists |
| Bridge-generation retirement | [NOT DONE] | Old bridge destroyed immediately on size change, no deferred destruction |
| Document synchronization domains | [NOT DONE] | Game queue vs bridge queue sync documented informally in comments but not formally |

## Phase 3 - Bridge Synchronization

| Item | Status | Notes |
|------|--------|-------|
| Game→bridge ordering | [DONE] | Game queue Signal(v1) → bridgeQueue Wait(v1). Implemented fix19 |
| Bridge→game ordering | [DONE] | fix27: game queue opens shared fence + injQueue->Wait(gameFence, v2) before copy-back |
| Bridge allocator reuse safety | [NOT DONE] | g_bridgeAlloc->Reset() called without proving GPU completion of prior recording |
| Bridge resource lifecycle states | [NOT DONE] | Shared textures stay in COMMON permanently; no explicit SRV/UAV transitions tracked per-frame on our device |

## Phase 4 - Renderer Generations

| Item | Status | Notes |
|------|--------|-------|
| Global renderer generation | [NOT DONE] | |
| Resource generation tagging | [NOT DONE] | |
| DLSS feature generation match | [NOT DONE] | |
| Bridge generation match | [NOT DONE] | |

## Phase 5 - Replace Global State Tracking

| Item | Status | Notes |
|------|--------|-------|
| Per-command-list resource state | [NOT DONE] | Still using global g_resourceStates map |
| CommandListState structure | [NOT DONE] | |
| Unknown state → skip frame | [PARTIAL] | Barrier() skips untracked resources but doesn't skip entire frame |

## Phase 6 - Descriptor Tracking

| Item | Status | Notes |
|------|--------|-------|
| Descriptor identity = heap + ptr | [PARTIAL] | g_rtvMap uses handle.ptr as key which includes heap implicit in pointer value |
| Heap creation tracking | [NOT DONE] | |
| SetDescriptorHeaps per-list | [NOT DONE] | g_setHeapCount/g_setHeaps are globals |

## Phase 7 - Command List Identity

| Item | Status | Notes |
|------|--------|-------|
| Replace s_hookedLists vector | [NOT DONE] | Still uses vector with linear search |
| Handle pointer reuse | [NOT DONE] | |
| Prevent indefinite growth | [NOT DONE] | Vector grows forever |

## Phase 8 - Resource Discovery Confidence

| Item | Status | Notes |
|------|--------|-------|
| Scene-color confidence scoring | [NOT DONE] | First-match adoption, no confidence system |
| Depth confidence scoring | [NOT DONE] | Last-write-wins, no scoring |
| MV confidence scoring | [NOT DONE] | First-match + ALT slot, no scoring |
| Renderer-gen invalidation of discovery | [NOT DONE] | |

## Phase 9 - Camera Jitter Correctness

| Item | Status | Notes |
|------|--------|-------|
| Determine engine jitter | [PARTIAL] | Engine has TAA jitter (w2s11 values observed varying) but full sequence/amplitude not mapped |
| Replace vs add jitter | [NOT DONE] | Currently replaces (overwrites col3 coefficients) but relation to engine TAA not proven |
| Prevent cumulative jitter | [DONE] | Always patches from validated source copy, never accumulates |
| Ring-buffer double-patch prevention | [PARTIAL] | Multiple dsts per frame patched independently; no dedup within same frame |

## Phase 10 - Motion Vector Correctness

| Item | Status | Notes |
|------|--------|-------|
| Static world + static camera → MV zero | [NOT DONE] | Not tested |
| Moving camera MV direction/magnitude | [NOT DONE] | Not tested |
| MV sign convention | [NOT DONE] | Assumed UV-space prev-current based on docs, not verified |
| MV units | [NOT DONE] | Assumed UV-space, MV.Scale=(W,H) converts to pixels |
| MV scaling verification | [NOT DONE] | Uses display dims, should use actual MV texture dims |

## Phase 11 - Matrix Inversion

| Item | Status | Notes |
|------|--------|-------|
| Pivot checks | [DONE] | ValidateCameraCb checks matrix validity |
| Inversion residual test | [NOT DONE] | |
| Bad matrix logging | [NOT DONE] | Reject logs exist but no residual computation |

## Phase 12 - Temporal State

| Item | Status | Notes |
|------|--------|-------|
| TemporalState struct | [NOT DONE] | m_firstEvaluate bool + m_currJitter/m_prevJitter used instead |
| History reset on feature creation | [DONE] | m_firstEvaluate=true after CreateFeature |
| History reset on resolution change | [DONE] | UpdateSizes sets m_firstEvaluate=true |
| History reset on map transition | [NOT DONE] | |
| Previous-jitter first frame | [NOT DONE] | |

## Phase 13 - NGX Hardening

| Item | Status | Notes |
|------|--------|-------|
| Param object self-test | [NOT DONE] | Padding virtuals added but no read/write verification at startup |
| Deterministic DLL discovery | [PARTIAL] | DriverStore FindFirstFile + System32 fallback, but no version validation |
| Feature/device/gen identity before eval | [NOT DONE] | |
| Fence before destroying feature | [DONE] | DestroyFeature uses ReleaseFeature (not Shutdown); but no fence wait before release |
| Fail closed on NGX errors | [DONE] | Circuit breaker + sticky halt + graveyard prevents stale output use |

## Phase 14 - Transactional Evaluation

| Item | Status | Notes |
|------|--------|-------|
| DlssFrame structure | [NOT DONE] | |
| Centralized validation | [NOT DONE] | Gates scattered across multiple if blocks |
| VALIDATE→PREPARE→RECORD→EVALUATE→COMMIT flow | [NOT DONE] | Linear flow with partial rollback on fail |
| g_injectedThisFrame only after success | [DONE] | Only set after successful record+submit |

## Phase 15 - Fault Handling

| Item | Status | Notes |
|------|--------|-------|
| SEH fault → mark unhealthy | [PARTIAL] | HALTED breaker for NGX failures; but SEH faults just log and continue next frame |
| Passive fallback on failure | [PARTIAL] | g_dlaaHalted stops DLAA; HUD continues independently |
| Explicit recovery sequence | [NOT DONE] | No recovery path once halted (requires F10 or restart) |

## Phase 16 - Present-Path Cleanup

| Item | Status | Notes |
|------|--------|-------|
| Reduce work in Present | [DONE] | Present hooks now only poll hotkeys; all work moved to ECL callback |
| Move expensive prep earlier | [PARTIAL] | Init on dedicated thread; but discovery/adoption still in ECL path |

## Phase 17 - COM Correctness

| Item | Status | Notes |
|------|--------|-------|
| Audit raw pointers | [NOT DONE] | Multiple raw resource pointers without ownership documentation |
| ComPtr usage | [NOT DONE] | All raw COM pointers |
| Generation + lifecycle identity | [NOT DONE] | |

## Phase 18 - Stress Testing

| Item | Status | Notes |
|------|--------|-------|
| Launch test | [DONE] | Multiple successful launches |
| Loading-screen test | [FAIL] | Crashes during loading in active mode (the current blocker) |
| Map-transition stress | [NOT DONE] | |
| Resolution test | [NOT DONE] | |
| DLSS toggle test | [NOT DONE] | F10 toggling caused crashes previously |
| DLAA long-run (10min+) | [NOT DONE] | Never achieved sustained injection |
| Long-run camera patch regression | [DONE] | 45k+ frames confirmed working |
| PIX/D3D12 debug validation | [NOT DONE] | |

## Phase 19 - Performance

| Item | Status | Notes |
|------|--------|-------|
| All items | [BLOCKED] | Cannot optimize until correctness proven per priority rule |

## Phase 20 - Documentation

| Item | Status | Notes |
|------|--------|-------|
| Accurate DLSS status description | [PARTIAL] | CACHE.md tracks detailed history; PLANS.md has failure ledger |
| PROVEN/OBSERVED/THEORETICAL separation | [PARTIAL] | Some claims need reclassification |
| PLANS.md maintained | [DONE] | A1-A11 entries with root causes |
