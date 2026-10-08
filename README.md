# ScaleNG.Drive

DLSS Super Resolution for BeamNG.drive (Direct3D 12), delivered as a
UAL-loaded ASI plugin. The plugin hooks the game's D3D12 calls, evaluates
NVIDIA DLSS on presented frames, and hands the result back into the
presented backbuffer.

## Current state (2026-10-08)

- **Working:** NGX init → native-size DLSS feature → thousands of
  consecutive evaluated + presented frames (up to ok #9000), on zero
  placeholders, on engine MV + engine velocity, and on engine MV +
  engine depth-family inputs. F8 visibly gates the handoff; F9 switches
  input sets with fail-closed fallback to zeros.
- **Not proven:** image-quality improvement (no controlled comparison
  yet), same-frame input association, MV/depth value semantics,
  baseline crash-freedom (one CreateFeature crash + two fault storms on
  record, causes undetermined).
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
