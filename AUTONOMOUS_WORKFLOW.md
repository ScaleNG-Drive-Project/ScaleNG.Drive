# ScaleNG.Drive — automated integration test

**Status (2026-10-02): implemented and run successfully.** This supersedes the
older proposal below that described approval as pending. The supported entry
point is `scripts\launch_test.bat`; implementation and requirements are in
`scripts\autonomous_test.py`, `scripts\setup_test_env.bat`, and
`scripts\README.md`.

## Run

Prerequisites: Windows, Visual Studio C++ build tools, BeamNG.drive 0.39.3 at
`C:\Games\BeamNG.drive`, and Python 3. Create the isolated, protocol-matched
Python environment once, then launch the test from the repository root:

```bat
scripts\setup_test_env.bat
scripts\launch_test.bat --duration 30
```

The runner builds the ASI, backs up/deploys plugin artifacts, starts a fresh
BeamNG process on TCom, loads `smallgrid` and a vehicle, observes the log and
Present counter, then stops its own game process and writes a timestamped
`result.json` and supporting logs under `logs\test_runs\`. See
`scripts\README.md` for options and outcome semantics.

The dedicated environment pins BeamNGpy 1.35.1, which speaks the game's TCom
v1.26. Do not substitute globally installed BeamNGpy 1.36 (TCom v1.27). Use the
level name `smallgrid`; do not pass a `.mis` filename or the legacy `gridmap`.
Do not add `-windowed`; this game's command-line path crashes with that flag.

## What the successful run proves

The 2026-10-02 run passed build, artifact validation, deployment, TCom startup,
level/vehicle load, plugin initialization, and D3D12/Present-hook activity.
The Present counter advanced from 1 to 3605 over 35 observations, with no fatal
marker. This is a genuine game-render/Present integration test, not merely a
compile or mocked unit test.

It did **not** log a DLSS-injection marker. Therefore it does not prove that
ScaleNG inserted DLSS output or that image quality improved. Use
`--require-dlss` when a run must be classified as DLSS-injection verified; no
marker produces `INCONCLUSIVE_DLSS`, not a false pass. A captured-frame
comparison is still needed for visual-quality validation.

## Local game setup note

In the tested installation, the pre-existing `Bin64\dxgi.dll` proxy conflicted
with the UAL-loaded ASI. It was reversibly renamed to
`dxgi.dll.scaleng-disabled-20261002` before the successful test. Keep the DXGI
proxy disabled while testing the ASI path unless that interaction is being
explicitly investigated. This is local setup, not a repository artifact.

Older proposal and implementation snippets were removed because they described
an unimplemented workflow, wrong build target, unsupported level arguments,
and stale success criteria. Consult the project log/cache for historical
experiments; prefer the current runner documentation for operating instructions.
