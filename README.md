# ScaleNG.Drive

DLSS Super Resolution for BeamNG.drive (Direct3D 12), delivered as a
UAL-loaded ASI plugin. The plugin hooks the game's D3D12 calls, evaluates
NVIDIA DLSS on presented frames, and hands the result back into the
presented backbuffer.

## Current state (2026-10-08) — Post-DLSS initialization era

- **Boundary:** run `20261007T165257Z` is the first verified NGX
  `Init_Ext` + feature + evals (loader commit `5e2b6c2`).
  Pre-DLSS initialization history lives in
  [docs/archive/](docs/archive/README.md); everything below is
  Post-DLSS initialization.
- **Working:** DLAA-native shadow eval (render == display ==
  backbuffer) → thousands of consecutive evaluated + presented
  frames (ok `#N` counters; `ok` lines are sampled). Best streaks:
  REAL `#9000` with engine MV + engine depth-family fmt-45 input
  (20261007T200358Z; depth value convention UNKNOWN),
  REAL `#7200` with engine MV + engine velocity-as-depth
  (20261007T194541Z), zeros `#7800` refs. User run
  20261007T204943Z: 7 F9 REAL engagements (genuine MV + SELF zero
  depth) with null visual. F8 visibly gates the handoff (default
  ON); F9 switches input sets with fail-closed fallback to owned
  zero MV/depth placeholders. Jitter 0/0, mvScale 1.0 on zeros /
  W×H on real, HDR default with F7 LDR toggle, sharpness
  SDK-unsupported. 30/120 fault breaker is live but UNFIRED;
  self-adopt guard blocked in all observed cases.
- **Not proven:** image-quality improvement (no controlled comparison
  yet), same-frame input association, MV/depth value semantics,
  baseline crash-freedom (UNKNOWN — one CreateFeature crash
  192627Z + 1904-fault cluster 194541Z + 7390-storm 200904Z
  starting 1 ms after an F9 press, F9 involvement UNRESOLVED both
  directions). Failures present the original frame except the
  192627Z crash. Run 120239Z contains 8 F9 toggles — not a
  pure-zeros reference.
- **Defaults are the safe fallback:** zero MV/depth inputs, handoff on,
  real inputs off. See [docs/INSTALL.md](docs/INSTALL.md) (config table)
  and [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md) (what the build does).

## Read in this order

1. [Current status and agenda](docs/STATUS.md) — latest verified live test and open work.
2. [Implementation (current)](docs/IMPLEMENTATION.md) — init, feature, eval, inputs, handoff, guards.
3. [Build/install/configure](docs/INSTALL.md) — ASI setup, INI table, live controls, uninstall.
4. [Test guide](scripts/README.md) — setup, commands, flags, result meanings.
5. [Technical documentation index](docs/README.md) — hub, history, references.

## Quick start (test machine)

`scripts\setup_test_env.bat` (once), then
`scripts\launch_test.bat --duration 60`. The project test machine uses
BeamNG.drive 0.39.3, BeamNGpy 1.35.1 / TCom v1.26, and the `smallgrid`
level. Do not start another BeamNG instance during a run.

Machine-specific: the test installation's pre-existing `Bin64\dxgi.dll`
proxy is reversibly disabled (`dxgi.dll.scaleng-disabled-20261002`) for
UAL ASI runs; do not copy that workaround elsewhere blindly.

## Documentation map

- [docs/](docs/README.md): hub, current implementation, status, history.
- [docs/archive/](docs/archive/README.md): superseded plans/hypotheses (provenance, not guidance).
- [scripts/](scripts/README.md): test runner documentation.
- [User/](User/README.md): archived engineering checklist and completion notes.
- [TOOL_REQUIREMENTS.md](TOOL_REQUIREMENTS.md): toolchain and environment inventory.
