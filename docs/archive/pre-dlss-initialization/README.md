# Pre-DLSS initialization era — phase index (HISTORICAL, SUPERSEDED)

> **HISTORICAL / SUPERSEDED — do not use as current status.**
> Phase label (exact): **Pre-DLSS initialization**.
> Era boundary: everything before run **`20261007T165257Z`** (2026-10-07),
> the first verified `Init_Ext SUCCEEDED` + feature created + evals #1..1800+.
> Current status lives in `docs/STATUS.md`; this directory only preserves
> what the pre-success era proved.

## Era definition

"Pre-DLSS initialization" = all work before run `20261007T165257Z`
(first verified `Init_Ext SUCCEEDED` + `feature created (1286x723 → 1920x1080)` +
`shadow-eval ok #1..#1800+` consecutive, zero failures — STATUS.md:1958-1974).

Boundary evidence (quoted, not moved):

- STATUS.md:1958 — section header `NGX CORE LOADER FIX — Init_Ext
  SUCCEEDED + feature created + evals run (2026-10-07, runs
  20261007T165257Z/193649Z+, commit pending)`.
- STATUS.md:1968-1974 — `Result (run 20261007T165257Z, PASS
  freeroam+thePlayer, 0 fatal)`: `loaded driver core nvngx.dll
  (...nvltsi...)`, NGX log `Found matching adapter with NVAPI physical GPU
  handle`, `Init_Ext SUCCEEDED`, `feature created (1286x723 → 1920x1080)`,
  `shadow-eval ok #1..#1800+`.
- Commit `5e2b6c2` (`git show --stat`: 7 files — `src/dlss_ngx.cpp`,
  `src/d3d12_hooks.cpp/.h`, `src/main.cpp`, `tests/ngx_init_probe.cpp`,
  `dist/ScaleNG.ini`, `docs/STATUS.md`) is the census-loader code; the
  `nvlti.inf_*` → `nv*.inf_*` narrowing dates to `34c95d7`; the boundary run
  predates the commit timestamp, i.e. working-tree code, "commit pending".
- Loader persists in `src/dlss_ngx.cpp:234-260` (driver-store
  `nv*.inf_*` census, first `nvngx.dll` found, `GetLastError` logged on
  census failure).

Predecessor runs inside the era: `20261007T163018Z` / `20261007T163545Z` —
all-fail (classic `0xBAD00001` / Ext `0xBAD00002`, ~20x each, zero evals);
the swamp the boundary run escaped (STATUS.md:1800-1802 for 163018Z fresh-dir
identical codes; `src/dlss_ngx.cpp:220-221` for 163545Z SIMCLASS identical
`0xBAD00002`).

## What the era proved (verified facts)

1. The NGX init matrix was exhausted project-locally: every project-local
   input variant was eliminated with live one-variable evidence
   (STATUS.md:1976-1980: "Every project-local input variant is now
   exhausted; remaining Init hypotheses need either NVIDIA-side answers or
   driver work — both external"). Full ledger: `INIT-FAILURE-RECORD.md`.
2. The failure was wrong-module, not wrong-parameters: `LoadNGX` hardcoded
   ONLY `nvlti.inf_*` while this box carries the core at
   `nvltsi.inf_amd64_...\nvngx.dll` (489KB vs 74MB snippet), so every Init
   ran against the fallback feature DLL (STATUS.md:1955-1961).
3. The 5th-arg type concern and classic-vs-Ext code split are explained:
   `FeatureCommonInfo` against the loaded CORE returns success; earlier Ext
   failures were wrong-module (snippet), not wrong-struct
   (STATUS.md:1837-1842).

## Pointers to live records (do NOT move them)

- `docs/STATUS.md` — `## NGX Init failure investigation` (:1743),
  `## NGX bring-up resolution` (:1804), `## NGX CORE LOADER FIX` (:1952).
- `docs/TECHNICAL_REFERENCE.md` — §2 frame architecture / camera-CB layout,
  §7 design notes (viewport-patch-only scale, jitter placement, in-place
  camera-CB patch, injection point). Historical reference; §8 verification
  guide is stale-era install text.
- `User/completed.md` — pre-era architecture record (cross-device bridge
  ABANDONED, RC1–RC10 root causes, NGX-evaluate `0xBAD00002` analysis).
  Last updated 2026-08-25, fix142 era — historical.
- `OMNI.md` — M9-era autonomous instructions (bridge recovery); historical.
- `docs/archive/README.md` — older proposals (DXGI/ReShade plans etc.).

## What carried over

- Census loader (`src/dlss_ngx.cpp:234-260`).
- Shadow-eval own-list path, `ProbeCleanDeviceInit`, `Init_Ext` +
  logging-callback path, `ngxApiVersion` INI plumbing,
  `tests/ngx_init_probe.cpp` (STATUS.md:1986-1994 — all reversible per hunk).
- Pre-era subsystems/designs, retained as context only: DXGI-proxy
  architecture, ReShade integration notes, bridge cross-device design
  (ABANDONED, `User/completed.md` RC1), legacy engine-list injection design,
  viewport render-scale patch (§7.2.1), HDR scene path pre-verification,
  quiet-gate/camera-frame work, PIX captures (1920x992-era constants are
  capture-specific, not live spec), camera-CB validation, jitter formulas
  (unapplied to render).

## Verified facts vs hypotheses/unknowns

- VERIFIED: boundary run lines above; wrong-module root cause; exhaustion
  of the project-local matrix (each item ledgered in INIT-FAILURE-RECORD.md).
- HYPOTHESES (unbacked, NOT eliminated): SIMCLASS forum experiment (code
  comment only, `src/dlss_ngx.cpp:220-221`), D3D11 path (no mention found).
- WITHDRAWN (not a finding): driver repair/reinstallation (STATUS.md:1764-1768,
  1814-1815).
- UNKNOWN at era end: color correctness (IsHDR vs LDR input), MV/depth
  semantics, any visual benefit (STATUS.md:1967-1968, 1981-1985).

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
