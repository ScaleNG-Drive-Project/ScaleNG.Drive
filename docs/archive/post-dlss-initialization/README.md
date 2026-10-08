# Post-DLSS initialization — phase index

> **HISTORICAL RECORD — SUPERSEDED PENDING.** Recent record of the
> "Post-DLSS initialization" era. `docs/STATUS.md` header is the
> current-status source of truth when updated; this file is frozen.

## Era definition

- **Label (exact):** "Post-DLSS initialization".
- **Prior era (exact):** "Pre-DLSS initialization".
- **Boundary run:** `20261007T165257Z` — first NGX `Init_Ext` SUCCEEDED +
  feature created + shadow-eval `ok #1..#1800+` consecutive, every present,
  zero failures (STATUS.md § "NGX CORE LOADER FIX").
- **Scope:** everything from the boundary run through the 2026-10-07/08
  eval+handoff streaks, HDR/LDR A/B, zeros baselines, REAL-input
  progression, fault events, and the user F9 session. No new claims beyond
  the reconciled boundary-run facts.

## Streak / milestone ledger

| Run ID | Outcome | One-line proof |
|---|---|---|
| 20261007T165257Z | PASS, 0 fatal | `Init_Ext` SUCCEEDED + feature created + `ok #1..#1800+`, first NGX evals on wrapped game device |
| 20261007T171602Z | PASS, 0 fatal | 4200+ consecutive eval+handoff frames (`ok #4200` at present 5041, 0 failures); 1000-frame criterion SATISFIED for mechanics |
| 171851Z | PASS (streak) | 6600+ oks, handoff ON screenshot healthy freeroam frame (non-destructive) |
| 172210Z | PASS (streak) | 4200+ oks, handoff OFF via INI toggle, healthy frame; log holds stale overlapping process (PID-filtered), pre-launch tasklist check stays mandatory |
| 20261007T183644Z | PASS, 0 fatal | 7× F7 HDR/LDR toggles, feature recreated each time, steady `ok #15000`; user verdict "essentially the exact same" → color/sharpness eliminated as softness causes (sharpness SDK-unsupported) |
| 190954Z | PASS_DLSS_EVAL (zeros baseline) | 22-proxy oks (19 counted), flag off — regression baseline, 0 fatal |
| 195249Z | PASS (zeros baseline) | Zeros-recurrence check after 194541Z fault cluster — cluster did NOT recur |
| 224756Z (2026-10-08) | PASS_DLSS_EVAL (zeros baseline) | 17 oks, 0 FAILED/FAULTED, zeros default (DestroyFeature hardening validation; reset path not exercised) |
| 191619Z | INVALID run | 2048x2048 fmt-10 ALT hijacked display size → every present exits at size check; fail-closed guards behaved (`why=mv-retired`, 0 REAL); proves adoption hazard |
| 192231Z | PASS_DLSS_EVAL, 0 REAL | 26 oks; guards correctly rejected all run (`why=mv-format` ×27) — safe rejection proof |
| 194541Z | FAIL (token) | 1904 zero-fallback faults then REAL `ok #7200` (engine MV + engine velocity-as-depth); FAIL token from unbounded FAULTED lines; zero→REAL causality UNCLAIMED |
| 20261007T200358Z | PASS_DLSS_EVAL | REAL `ok #1` (present 842) → `ok #9000` (present 9841), handoff 1 throughout; engine MV + engine depth-family fmt-45 (value convention UNKNOWN — never "true D24"); no depth-conversion shader needed; totals from #N counters (25 sampled lines) |
| 200904Z | FAIL (storm) | 6000 healthy zeros then 7390 consecutive faults from 1ms after F9 press; F9 involvement UNRESOLVED both ways (storm continued after toggling back); motivated log bound + 30/120 breaker (breaker UNFIRED live to date) |
| 192627Z | GAME_CRASHED_AFTER_PLUGIN_INIT (0xC0000005) | Crash on FIRST eval with ZEROS inside NGX `CreateFeature`; cause UNDETERMINED |
| 204943Z (user session) | PASS_DLSS_EVAL | 7× F9-ON, 7/7 engaged REAL (genuine MV + SELF zero depth via pre-guard SRV self-adoption) + null visual; F8 gates handoff visibly; null explained (E4 subtlety + depth-void + short windows), not a toggle failure |
| 220445Z | PASS_DLSS_EVAL | Guard fires live (`self-adopt rejected`), clean fallbacks (depth-size ×22 on transient junk); 21 sampled oks (last #6600), breaker silent |
| 120239Z | PASS_DLSS_EVAL (zeros reference) | Zeros + 8 F9 toggles (NOT pure-zeros); `ok #7800` reference with 204943Z |

Notes:
- Runner semantics: sampled ok / FAILED / FAULTED cadences; `PASS_DLSS_EVAL`
  counts zeros and REAL alike.
- 200358Z depth wording: engine depth-FAMILY fmt-45 (D24_UNORM_S8_UINT);
  value convention UNKNOWN.
- 172210Z hygiene note and 120239Z NOT-pure-zeros qualifier are load-bearing;
  do not drop them when citing streaks.

## Cross-cutting unknowns (still open at archive time)

- Same-frame color/MV/depth association UNPROVEN (frozen frame clock).
- MV sign/axis/scale + depth value conventions UNPROVEN.
- Image-quality gain UNPROVEN.
- Crash-freedom UNKNOWN (baseline NOT proven crash-free).
- Runner `PASS_DLSS_EVAL` ≠ visual success; human A–G drive test awaited driver.

## Pointers to STATUS.md (quote, don't copy)

- Current state + key runs — `docs/STATUS.md:5-42` (verified facts,
  unknowns, crashes/faults, blockers, next, key-runs line).
- Boundary run — § "NGX CORE LOADER FIX — Init_Ext SUCCEEDED + feature
  created + evals run (2026-10-07, runs 20261007T165257Z/193649Z+)".
- Streaks + F8/INI + hygiene — § "Visual A/B (2026-10-07; handoff ON vs OFF
  screenshots…)" (runs 171851Z/172210Z).
- HDR/LDR A/B — § lines ~1880-1898 (run 20261007T183644Z, 7× F7,
  `ok #15000`, user verdict, SDK sharpening quote).
- REAL path + baselines + INVALID/reject/crash — § "Real MV/depth inputs:
  fail-closed path + live findings (2026-10-07, runs 190954Z–193154Z)".
- REAL engaged — § "REAL ENGAGED (2026-10-07, run 20261007T194541Z…)".
- True depth + 9000 — § "TRUE DEPTH + 9000 REAL evals (2026-10-07, run
  20261007T200358Z…)".
- Storm + breaker — § "Fault storm + circuit breaker (2026-10-07, runs
  200904Z/201932Z)".
- User session + self-adoption fix + 220445Z — § "User F9 session
  forensics + self-adoption fix (2026-10-07/08, runs 204943Z/211024Z/220445Z)".
- Hardening + 224756Z zeros — § "Verification round + DestroyFeature
  hardening (2026-10-08, run 224756Z)".

## Sources quoted

- `docs/STATUS.md:5-42`, `:1843-1848` (171602Z milestone), `:1882-1898`
  (183644Z toggles + verdict), `:1926-1942` (171851Z/172210Z A/B + hygiene),
  `:1968-1974` (165257Z result), `:2191-2225` (190954Z–193154Z ledger +
  192627Z crash), `:2234-2270` (194541Z), `:2272-2305` (200358Z + 195249Z
  non-recurrence), `:2307-2343` (200904Z storm + breaker + 120239Z),
  `:2345-2396` (204943Z session + 220445Z), `:2398-2424` (224756Z).

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
