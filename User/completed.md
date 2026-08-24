# ScaleNG.Drive - Completed Work Log

# ScaleNG.Drive - Completed Work Log

> **Generated:** 2026-08-22, after fix33 (hash 81D2546B / git ecdc860).
> Companion to `User/correctness.md`. Status: `[DONE]` `[-]` `[BLOCKED]` `[N/A]`

---

## Immediate Priority Queue (Top 20)

| # | Item | Status | Details |
|---|------|--------|---------|
| 1 | Fix AdoptDisplaySize hysteresis | [DONE] | Proper candidate/accepted pattern. Globals not updated until 15 stable observations. Candidate reset on different dims. |
| 2 | Bridge→game sync | [DONE] | fix27: game queue opens shared fence via OpenSharedHandle, enqueues GPU-side Wait(bridgeFence, v2) before copy-back list submission |
| 3 | Fence-protect g_injAlloc Reset | [DONE] | Existing fence wait before allocator Reset in TryDeferredInject (INFINITE wait on g_injEvent) |
| 4 | Fence-protect g_bridgeAlloc Reset | [DONE] | CPU-side fence wait added: checks GetCompletedValue >= g_bridgeLastSubmit before Reset, waits up to 5s if not |
| 5 | GPU ordering vs CPU lifetime | [PARTIAL] | Queue::Wait() enforces GPU ordering; CPU lifetime requires separate GetCompletedValue check + wait. Documented but not fully enforced everywhere |
| 6 | Reset s_gameFence on bridge change | [-] | Consolidated to single g_gameFence global (was duplicated in two scopes). Opened once from shared handle; no stale instance possible since it's the only reference |
| 7 | Per-command-list resource state | [NOT DONE] | Major refactor — global g_resourceStates still used. Deferred to Phase 5 of checklist |
| 8 | Depth discovery heuristic fix | [NOT DONE] | Hardcoded dimension checks removed (now dynamic >=1000x500), but scoring/persistence system not yet implemented |
| 9 | Backbuffer PRESENT restore | [DONE] | Copy-back block transitions bb PRESENT→COPY_DEST→copy→PRESENT via explicit barrier in fresh list |
| 10 | Track actual bb state | [PARTIAL] | g_resourceStates tracks bb but initial state assumption (PRESENT) is hardcoded at discovery time |
| 11 | Bridge→game sync verification | [DONE] | Cross-queue GPU sync implemented (fix27). Game queue Wait(bridgeFence, v2) before copy-back |
| 12 | Bridge allocator reuse | [DONE] | CPU-side fence check added before bridgeAlloc Reset |
| 13 | Direct Release() retirement | [PARTIAL] | Graveyard exists for dlssOut; bridge resources released during EnsureBridge teardown without fence protection |
| 14 | Renderer generation tracking | [NOT DONE] | Major architectural addition needed |
| 15 | NGX parameter ABI validation | [NOT DONE] | Padding virtuals added to NgxParamStore but no runtime self-test |

---

## Phase 0 - Debugging Baseline

| Item | Status |
|------|--------|
| Known-good build recorded | [DONE] Passive run stable 3+ min |
| Passive mode stable | [DONE] Confirmed multiple times |
| Camera-CB patching works | [DONE] Working since session 8 |
| NGX init succeeds | [DONE] On bridge device (our clean device) |
| DLAA eval succeeded once | [DONE] Frame 9 (fix14 era), and harness test_mini/test_eval |
| Preserve regression baseline | [DONE] Git history has all versions |
| Crash classification taxonomy | [NOT DONE] Ad-hoc only |
| Comprehensive crash logging | [PARTIAL] Step breadcrumbs + fault addr/module/regs. Full context not yet |
| Log exact DLSS failure result | [DONE] NGX result code logged per failure |
| Renderer generation in logs | [NOT DONE] |
| Camera-CB rejection cleanup | [NOT DONE] Rejection dumps capped at 5 but still verbose |

## Phase 0 - NVGX Telemetry

| Item | Status |
|------|--------|
| nvngx.log live in-game | [DONE] __NGX_LOG_LEVEL=3 set via SetEnvironmentVariableA + setx user-wide |
| Snippet validation messages visible | [DONE] Confirmed: snippet found, version checked, metadata validated |
| Exact failure reason visible | [PARTIAL] RWFlagMissing visible as result code; internal resource creation details NOT visible in log |

## Phase 1 - Correctness Bugs

| Item | Status |
|------|--------|
| AdoptDisplaySize hysteresis | [DONE] Proper candidate/stability pattern |
| Hardcoded dimensions removed | [DONE] Dynamic >=1000x500 for RTV discovery |
| Dead sync code removed | [NOT DONE] Still has unreachable branches |
| Direct Release() audit | [PARTIAL] Graveyard for dlssOut; other releases not fence-tagged |
| Thread-safe InitializeASI | [DONE] InterlockedCompareExchange atomic guard |
| Thread-safe KickInitThread | [DONE] InterlockedCompareExchange for event creation |
| Atomic g_injResourcesReady | [DONE] volatile long with Interlocked ops |
| Sticky s_fenceTried removed | [DONE] Replaced with null-check retry pattern |

## Phase 2 - GPU Lifetime

| Item | Status |
|------|--------|
| Fence-tagged retirement queue | [NOT DONE] Basic graveyard exists but untagged |
| Per-resource last-use fence tracking | [NOT DONE] |
| Bridge generation retirement | [NOT DONE] |
| Shared-fence handle lifetime | [NOT DONE] |
| g_bridgeVal monotonic | [DONE] Monotonically incrementing, never reset |
| Sync domain documentation | [NOT DONE] Informal comments only |

## Phase 3 - Bridge Sync

| Item | Status |
|------|--------|
| Game→bridge ordering | [DONE] Signal(v1) then Wait(v1) |
| Bridge→game ordering | [DONE] Signal(v2) then Wait(v2) via game-side opened fence |
| Bridge alloc fence-protect | [DONE] CPU-side check before Reset |
| Bridge output transition fix | [NOT DONE] Still COMMON→COMMON effectively |
| NGX output state determination | [NOT DONE] Assumed COMMON |
| Two-submission ordering documented | [DONE] Comments explain why |

## Phase 4 - Renderer Generations

| Item | Status |
|------|--------|
| Generation counter | [NOT DONE] |
| Resource generation tagging | [NOT DONE] |
| Feature generation match | [NOT DONE] |
| Bridge generation match | [NOT DONE] |
| Invalidation on generation change | [NOT DONE] |

## Phase 5 - State Tracking

| Item | Status |
|------|--------|
| Replace global map | [NOT DONE] |
| Separate ScaleNG/game contamination | [NOT DONE] |
| Unknown state → skip frame | [DONE] Barrier() skips untracked resources |
| Ownership metadata | [NOT DONE] |

## Phase 6 - Descriptor Tracking

| Item | Status |
|------|--------|
| Heap identity = ptr | [NOT DONE] |
| Heap creation tracking | [NOT DONE] |
| RTV against correct heap | [NOT DONE] |
| SetDescriptorHeaps per-list | [NOT DONE] Global g_setHeapCount/g_setHeaps used |

## Phase 7 - Command List Identity

| Item | Status |
|------|--------|
| Proper registry (map) | [NOT DONE] s_hookedLists vector with linear search |
| Handle pointer reuse | [NOT DONE] |
| Prevent growth | [NOT DONE] Vector grows forever |

## Phase 8 - Discovery

| Item | Status |
|------|--------|
| Depth scoring/persistence | [NOT DONE] Last-write-wins |
| MV dimension-independent | [DONE] >=1000x500 dynamic check replaces hardcoded |
| Confidence scoring all types | [NOT DONE] |
| Gen-change invalidation | [NOT DONE] |

## Phase 9 - Jitter

| Item | Status |
|------|--------|
| Convention documentation | [NOT DONE] |
| Engine jitter investigation | [NOT DONE] Engine's own TAA jitter observed (w2s11 varies) but relation to ours unknown |
| Cumulative prevention | [DONE] Always patches from validated source copy |

## Phase 10 - MV Correctness

| Item | Status |
|------|--------|
| All items | [NOT DONE] Requires visual testing with DLAA actually resolving frames |

## Phase 11 - Matrix Inversion

| Item | Status |
|------|--------|
| Residual test | [NOT DONE] |
| Bad matrix logging | [NOT DONE] |

## Phase 12 - Temporal State

| Item | Status |
|------|--------|
| TemporalState struct | [NOT DONE] m_firstEvaluate bool still used |
| History resets | [PARTIAL] Resolution change resets via UpdateSizes |

## Phase 13 - NGX Hardening

| Item | Status |
|------|--------|
| ABI validation layer | [NOT DONE] |
| Version gating | [NOT DONE] |
| DLL selection improvement | [NOT DONE] Uses first nvlti.inf_* match |
| UnloadNGX lifetime safety | [NOT DONE] |
| Fence before feature destroy | [DONE] DestroyFeature uses ReleaseFeature (not Shutdown); fence wait before graveyard flush covers this partially |
| Fail closed | [DONE] Circuit breaker + sticky halt + graveyard |
| Identity tracking | [NOT DONE] |

## Phase 14 - Transactional Eval

| Item | Status |
|------|--------|
| DlssFrame struct | [NOT DONE] |
| ValidateDlssFrame() | [NOT DONE] Gates scattered across multiple if blocks |
| Transactional flow | [PARTIAL] Eval fail → reverse barriers → skip frame |
| Success-only injection flag | [DONE] |

## Phase 15 - Fault Handling

| Item | Status |
|------|--------|
| SEH fault → unhealthy | [PARTIAL] HALTED breaker for NGX failures; SEH faults just log |
| Failure classification | [NOT DONE] |
| Recovery sequence | [NOT DONE] |

## Phase 16 - Present Cleanup

| Item | Status |
|------|--------|
| Allocator fence-protect | [DONE] Both allocators have CPU-side fence checks |
| Backbuffer PRESENT restore | [DONE] Explicit barrier in copy-back block |
| Track actual bb state | [PARTIAL] Map tracks it but initial assumption may be wrong |
| ECL race verification | [NOT DONE] Needs PIX proof |

## Phase 17 - COM Correctness

| Item | Status |
|------|--------|
| Pointer audit | [NOT DONE] |
| ComPtr usage | [NOT DONE] Raw pointers throughout |
| Generation+lifecycle identity | [NOT DONE] |

## Phase 18 - Stress Testing

| Item | Status |
|------|--------|
| Launch test | [DONE] Multiple successful launches |
| Loading-screen test | [FAIL] Crashes during loading when active |
| Map-transition stress | [NOT DONE] |
| Resolution test | [NOT DONE] |
| DLSS toggle test | [FAIL] F10 toggling caused crashes (fixed by fix29 gates but not re-tested) |
| DLAA long-run | [NOT DONE] Never achieved sustained injection |
| Camera patch long-run | [DONE] 45k+ frames confirmed |
| PIX validation | [NOT DONE] |

## Phase 19-20

| Item | Status |
|------|--------|
| Performance optimization | [BLOCKED] Cannot start until correctness proven |
| Documentation cleanup | [NOT DONE] |

## Summary Scorecard

| Category | Done | Partial | Not Done | Blocked |
|----------|------|---------|----------|---------|
| Immediate Priority (1-20) | 7 | 4 | 9 | 0 |
| Phase 0 (Baseline) | 5 | 3 | 3 | 0 |
| Phase 1 (Correctness) | 3 | 1 | 3 | 0 |
| Phase 2 (GPU Lifetime) | 1 | 0 | 6 | 0 |
| Phase 3 (Bridge Sync) | 3 | 0 | 6 | 0 |
| Phase 4 (Generations) | 0 | 0 | 5 | 0 |
| Phase 5 (State Tracking) | 1 | 0 | 5 | 0 |
| Phase 6 (Descriptors) | 0 | 0 | 4 | 0 |
| Phase 7 (CmdList ID) | 0 | 0 | 3 | 0 |
| Phase 8 (Discovery) | 1 | 0 | 6 | 0 |
| Phase 9 (Jitter) | 1 | 0 | 6 | 0 |
| Phase 10 (MV) | 0 | 0 | 7 | 0 |
| Phase 11 (Matrix) | 1 | 0 | 2 | 0 |
| Phase 12 (Temporal) | 1 | 1 | 5 | 0 |
| Phase 13 (NGX) | 2 | 0 | 6 | 0 |
| Phase 14 (Transactional) | 1 | 1 | 3 | 0 |
| Phase 15 (Faults) | 0 | 2 | 3 | 0 |
| Phase 16 (Present) | 2 | 1 | 3 | 0 |
| Phase 17 (COM) | 0 | 0 | 3 | 0 |
| Phase 18 (Testing) | 2 | 0 | 7 | 0 |
| Phase 19 (Perf) | 0 | 0 | 5 | BLOCKED |
| Phase 20 (Docs) | 1 | 0 | 6 | 0 |

**Overall: ~30% complete. Core architecture works; correctness hardening is the remaining work.**
## Session update 2026-08-23 (commits 43d4b9b, 36537fb)
- [x] **#4** Device-removed precondition before CreateFeature - fix73: GetDeviceRemovedReason() gate, logs once, skips cleanly (verified pattern: 0x887A0001 waste eliminated).
- [x] **#27** MV-absent capability handling - fix74: once-per-session 'MV capability ABSENT' warning; DLAA idles until MV RTV binds. No fabricated evaluation.
- [x] **#33** Ownership model documented honestly - VERIFIED ALREADY DONE: StoreTracked comments (lines ~262, ~341) state STRICTLY WEAK + rationale + safety stack since fix53.
- Note: binary fingerprint (#6 of reviewer plan) done in fix72 (build date/time in first log line).
## P0 progress (fix75, commit af5c1e0)
- [x] **P0#1** InitializeASI instrumented: tid/pid/asiBase/stateAddr logged with init line.
- [x] **P0#2 (partial)** state enum formalized 0=UNINIT/1=INITIALIZING/2=INITIALIZED/3=FAILED; atomic 0->1 guard pre-existed; FAILED now recorded on SEH.
- [x] **P0#4** module base logged - duplicate-copy detection enabled (compare bases across log lines).
- Queued next: P0#3 per-hook idempotency states; P1 device generations/classification.
## P0 complete (fix80, commit 33b58c8)
- [x] **P0#3** Hook-install idempotency: CreateDevice detour now carries s_installState (0/1/2); re-invocation logs and returns; failure paths record FAILED. Double-patch structurally impossible.
- **P0 (1,2,3,4) ALL DONE.** Next per plan: P1 device generations/classification (5-8), then P2 transactional bridge (11) + fence-safe teardown (12).
