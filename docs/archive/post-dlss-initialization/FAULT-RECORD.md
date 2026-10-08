# Post-DLSS initialization — fault record

> **HISTORICAL RECORD — SUPERSEDED PENDING.** Recent record of the
> "Post-DLSS initialization" era. `docs/STATUS.md` header is the
> current-status source of truth when updated; this file is frozen.

## 1. 192627Z — CreateFeature AV crash

- **Timeline:** 120s run, `realInputs=1` deployed; crash on FIRST eval.
- **Evidence (STATUS.md § "Real MV/depth inputs"):** `GAME_CRASHED_AFTER_PLUGIN_INIT`,
  exit `0xC0000005`; log sequence shows `why=mv-format` → zeros fallback
  (no engine barrier executed by own code — provable from log order);
  fault inside NGX `CreateFeature` (SEH `0xC0000005`).
- **Competing hypotheses:**
  - Pre-existing `CreateFeature` flakiness (first observed) — UNDETERMINED.
  - Run-specific driver state — UNDETERMINED.
  - Caused by the real-input hunk — REFUTED by baseline rerun 193154Z
    (flag off, current binary): `PASS_DLSS_EVAL`, 22 oks, 0 fatal, stable.
- **What it motivated:** contributed (with the two storms) to the standing
  warning "baseline NOT crash-free"; no code fix claimed from this event alone.
- **Non-claims:** no engine-state corruption by own code is possible on the
  logged path (zero fallback touches nothing engine-owned); crash cause
  UNDETERMINED, crash-freedom UNKNOWN.

## 2. 194541Z — zero-fault cluster then REAL success (FAIL token)

- **Timeline (run 20261007T194541Z, p2256, 120s):** presents 842→~2745
  faulted ~1904× (`EvaluateFeature FAULTED SEH 0xC0000005`) with ZERO
  fallbacks (`why=depth-size`: transient 359x379 fmt-28 depth candidate),
  then instant success from the first REAL eval → `ok #7200`
  (present 9945, handoff 1), normal game exit, 0 device-removal.
- **Evidence:** REAL = engine MV (`80DA99BFD0` fmt-34 1920x1080,
  placed + RTV-created, MV ALT) + engine velocity-as-depth (depth-slot
  `8045DA8860` fmt-34, second velocity buffer adopted via copy-DEST
  heuristic — NOT true depth; critical role correction stands).
  Params: mvScale 1920.0x1080.0 (UV hypothesis), jitter 0/0, HDR flags.
- **Competing hypotheses for zero→REAL transition:**
  - Upload-completion timing / driver warmup / first-feature settling —
    UNRESOLVED (correlation logged, CAUSALITY UNCLAIMED).
  - Zero-faults caused REAL success — UNCLAIMED (confounds listed above).
- **What it motivated:** zeros-recurrence rerun 195249Z (cluster did NOT
  recur); runner-note that 1904 unbounded `FAULTED` lines trip `no_fatal`
  (correctly — FAIL token even with later success); later log-bound work.
- **Non-claims:** FAIL token is from log volume, not from handoff failure;
  zero→REAL causality UNCLAIMED; same-frame MV proof weaker (stable
  pointer + mvAge 1, frozen frame clock, no per-frame MV touch logs).

## 3. 200904Z — fault storm (FAIL)

- **Timeline (run 200904Z, p13344):** 6000 healthy zero-evals (present
  842→6841), then EVERY eval faulted present 7054→run end (7390
  `EvaluateFeature FAULTED SEH 0xC0000005`, no recovery, no
  DEVICE_REMOVED, game survived, normal exit).
- **Evidence:** F9 verified working end-to-end in the same run
  (`scripts/f9_toggle.ps1`: `inputs REAL MV/depth (F9)` 23:10:50,
  `inputs zero placeholders (F9)` 23:11:26 — both directions register).
  Storm onset coincided with F9#1/focus-change to the exact second —
  BUT flag PROVEN uninvolved: validation never passed (no REAL line,
  zero engine contact), F9#2 did not stop the storm, no
  size/display/feature/reset events exist.
- **Competing hypotheses:**
  - F9 involvement — UNRESOLVED both ways (coincidence vs focus
    side-effect; testable focus-without-F9 vs F9-without-focus, n=1
    each — weak, deferred).
  - Poisoned-history (Reset only ever set on first eval — poisoned
    history persists by design) — HYPOTHESIS, mechanism UNKNOWN.
  - NGX CPU-side AVs cluster/persist with identical inputs after
    thousands of successes — OBSERVED pattern across 194541Z + 192627Z
    + 200904Z, mechanism UNKNOWN.
- **What it motivated:** circuit breaker + log bound (`src/dlss_ngx.cpp`,
  `src/d3d12_hooks.cpp`): FAULTED/failed logs bounded (first 10 +
  every 600); 30 consecutive faults → `ResetFeature`, 120 → HALT evals
  for the session (game presents unmodified). Verified no behavior
  change on healthy paths (run 201932Z: 60s PASS, 17 oks, breaker
  silent). Breaker UNFIRED live to date.
- **Non-claims:** F9 neither blamed nor exonerated; 20-min run stays off
  the table (user directive); baseline NOT crash-free (3 fault events).

## Standing warning (all three events)

One `CreateFeature` AV crash (192627Z) + two self-sustaining
`EvaluateFeature` fault storms with identical logged inputs (194541Z
zeros-era recovered; 200904Z permanent) — mechanisms undetermined. All
failures present the original frame; no device loss observed.

## Sources quoted

- `docs/STATUS.md:23-27` (known crashes/faults — all fault-storm eval
  failures present the original frame; the 192627Z CreateFeature AV is a
  process crash, not a presented frame), `:2209-2220` (192627Z +
  193154Z baseline), `:2234-2270` (194541Z), `:2307-2343` (200904Z storm,
  breaker, 201932Z verification).

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
