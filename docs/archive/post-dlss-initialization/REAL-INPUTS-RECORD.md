# Post-DLSS initialization — REAL inputs record

> **HISTORICAL RECORD — SUPERSEDED PENDING.** Recent record of the
> "Post-DLSS initialization" era. `docs/STATUS.md` header is the
> current-status source of truth when updated; this file is frozen.

## Progression: zeros → velocity-as-depth → genuine-depth

All evals are shadow-eval (`ShadowEvalAtPresent`); legacy `DoInjection`
records 0 injections in every run in this era.

### Stage 0 — owned zeros (default fallback)

- **What:** color = current presented backbuffer (fmt-28 LDR, post-HUD);
  depth + MV = OWNED zero-filled placeholders (`g_shMv` fmt-34
  R16G16_FLOAT, `g_shDepth` fmt-41 R32_FLOAT, `memset 0` once, parked
  in PSR forever); output = owned fmt-28 UAV; jitter 0/0; mvScale
  1.0x1.0 (zeros make scale moot).
- **Runs:** 190954Z (22-proxy oks, regression baseline), 195249Z
  (zeros-recurrence check — 194541Z cluster did NOT recur), 224756Z
  (17 oks, zeros default), 201932Z (17 oks, breaker silent), 120239Z
  reference (`ok #7800`; zeros + 8 F9 toggles — NOT pure-zeros).
- **Validation reasons on this stage:** `why=off` (flag off), `why=mv-format`
  (candidate alive but not fmt-34), `why=mv-retired`, `why=depth-size`
  (transient 359x379 fmt-28 junk correctly rejected).

### Stage 1 — engine MV + engine velocity-as-depth (run 194541Z)

- **Run 20261007T194541Z:** fail-closed path SELECTED real inputs;
  `ok #7200` (present 9945, handoff 1), 0 device-removal, normal exit.
- **Proof:** MV `80DA99BFD0` fmt-34 1920x1080 (placed + RTV-created,
  MV ALT); depth-slot `8045DA8860` fmt-34 1920x1080 (likewise
  RTV-created). NO conversion/copy (direct resources); tracked
  `Barrier()` to PSR + restore in same list; mvScale 1920.0x1080.0
  (UV hypothesis); jitter 0/0.
- **Role correction (load-bearing):** depth-slot is a SECOND VELOCITY
  BUFFER adopted via the copy-DEST heuristic — NOT true depth. So
  Stage 1 = real MV + velocity-as-depth. Depth-convention questions untouched.

### Stage 2 — engine MV + genuine depth-family (run 20261007T200358Z)

- **Hardening first:** `IsDepthFamilyFormat()` (numerics
  39/40/41/44/45/46); copy-heuristic adopts ONLY depth-family DSTs via
  guarded desc read; never overwrites SRV-sourced true depth
  (`g_depthSrvSourced`). Run 200000Z proved the gate (fmt-34 velocity
  copy REJECTED, `srvDepth=1` kept). fmt-45 correction: D24_UNORM_S8_UINT.
- **Run 20261007T200358Z (p7012, 120s, realInputs=1):**
  `PASS_DLSS_EVAL`, 0 FAILED, 0 FAULTED. REAL from first eval: `ok #1`
  (present 842) → `ok #9000` (present 9841), handoff 1 throughout.
- **Proof:** MV `FF7AACB9D0` fmt-34 1920x1080 ALT (placed + RTV-created,
  provenance logged); depth `FF6D983C90` fmt-45 D24_UNORM_S8_UINT
  1920x1080, SRV-adopted true depth (engine barrier 192→2048=COPY_SOURCE
  + engine copy FROM it at present 186 — alive and in engine use).
- **Findings:** NGX consumed the D24S8 resource directly 9000× with zero
  faults — **no depth-conversion shader is needed**. Totals from #N
  counters (25 sampled lines). Depth value convention UNKNOWN — never
  "true D24". Same-frame association strong-supported (stable pointers
  + engine use + age gates; frame clock frozen so strict proof limited).

## Fail-closed validation (why REAL is rare)

`g_shadowRealInputs` (INI `realInputs=0`, F9 live): liveness
(`SafeGetDesc`), MV fmt-34 + display size (primary else ALT), depth
known non-MSAA fmt + display size, freshness gates (MV age ≤10
presents, depth age ≤20000 frames), tracked entry states via
`LookupTrackedStates`. ANY failure → owned zeros (byte-identical path).
Diagnostics bounded, never per-frame. `IUpscaler` untouched.
Rejection proofs: 192231Z (26 oks, 0 REAL, `why=mv-format` ×27);
191619Z INVALID (adoption hazard, `why=mv-retired`, 0 REAL, 0 crashes).

## Self-adoption feedback loop + guard

- **Loop (pre-guard, found in 204943Z forensics):** NGX's first-eval SRV
  creation on our own placeholder (via hooked device) adopted it into
  the depth slot — depth `D35A68EFF0` was OUR OWN `g_shDepth`
  (committed, SRV-only, born present 842, no RTV role, no engine
  touches). MV `D35A4DF810` was genuine engine (RTV-created,
  rtv-provenance, barrier traffic). Root cause refined by
  adoption-tracer: METADATA-ONLY corruption (SRV-depth / MV-re-adopt /
  depth-copy sites set valid/stamp/fmt/srvSourced unconditionally AFTER
  a rejected store; slot kept engine texture, flags described ours).
  MV slot never at risk (no MV SRV branch exists). Runs 194541Z
  (reframed: real MV + real engine-velocity-as-depth) and 200358Z
  (real MV + real D24 depth) STAND as genuine-engine by creation-path
  fingerprint (placed-heap, 8–17s pre-infra, RTV roles at birth,
  engine traffic; ours committed/SRV-only/born ~842).
- **Fix:** `StoreTracked` returns bool; metadata early-outs at SRV-depth,
  MV re-adopt, depth-copy, scene ALT trio, scene-copy fallback, MV
  track-by-bind; `IsOwnResource` += staging + HUD textures; validation
  adds `self-input` fail-closed reason. Zero-path behavior unchanged.
- **Live proof:** 220445Z (90s, realInputs=1): `self-adopt rejected` ×1
  (loop recurs, blocked), depth-size fallbacks ×22, 21 sampled oks,
  0 FAILED/FAULTED, breaker silent.

## MV / depth semantics unknowns (UNPROVEN at archive time)

- Same-frame association UNPROVEN (frozen frame clock).
- MV sign/axis/scale (UV prev-minus-cur + legacy `mvScale=W/H` =
  hypothesis, untested) + jitter-inclusion UNKNOWN (no readback by policy).
- Depth value convention (inverted?) UNKNOWN, `DepthInverted` NOT set.
- Scene-color correspondence: shadow color is the PRESENTED frame
  (post-composite + HUD) while MV/depth describe the pre-composite
  scene render — approximately pixel-aligned at 1920x1080 but UI
  pixels carry scene MVs (FPS-trail mechanism observed).

## Why the user null result does not invalidate REAL

- **Session 204943Z (p12764):** 7× F9-ON, 7/7 engaged REAL (`why=real`),
  0 FAILED/FAULTED, F8 visibly gated handoff (log handoff bit follows).
- **Null visual explained, not a toggle failure:** inputs were genuine-MV
  + zero-depth REAL at DLAA-native-frozen-jitter over ~2–5s windows =
  E4 (expected subtlety) + depth-void + short windows. F9 worked; the
  signal was half-void. Next human test (A–G protocol) runs with
  genuine depth available.
- Separately, HDR/LDR + sharpness were already eliminated as softness
  causes (183644Z verdict + SDK "Sharpness is not supported"); remaining
  candidates are temporal (frozen jitter + zero/void MV/depth).

## Sources quoted

- `docs/STATUS.md:2125-2232` (shadow-eval contents, engine candidates,
  fail-closed design, 190954Z–193154Z ledger), `:2234-2270` (194541Z),
  `:2272-2305` (depth hardening + 200358Z), `:2345-2396` (204943Z
  forensics + guard fix + 220445Z), `:1926-1948` (F8 gating),
  `:1880-1898` (F7/HDR verdict).

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
