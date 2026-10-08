# Build, install, configure, and manual checks

This guide documents the supported UAL-loaded ASI path. For automated live
testing, use [the test guide](../scripts/README.md). For what the build
does, see [IMPLEMENTATION.md](IMPLEMENTATION.md); for evidence, see
[STATUS.md](STATUS.md).

## Build

Run `src\build_asi.bat` with Visual Studio C++ build tools available. It
produces `dist\ScaleNG.asi`, config/helper artifacts, and build diagnostics.
`src\build.bat` is the separate experimental DXGI proxy build, not the ASI
build.

## Install

1. Install/configure UAL for BeamNG.drive and its `Bin64\plugins` directory.
2. Copy the built `ScaleNG.asi`, `ScaleNG.ini`, and required helper/runtime
   artifacts from `dist\` to that plugin directory.
3. Provide `nvngx_dlss.dll` if the plugin configuration/runtime requires it;
   ScaleNG does not download NVIDIA binaries.
4. Start BeamNG and inspect the fresh `plugins\ScaleNG.log`.

Machine-specific note: on the verified test installation the existing
`Bin64\dxgi.dll` proxy was reversibly renamed to
`dxgi.dll.scaleng-disabled-20261002` to avoid a conflict with UAL ASI loading.
Do not overwrite or rename another machine's DLL without first checking the
actual installation. Keep the DXGI proxy disabled for the ASI test unless
coexistence is the subject of the test.

## Configure (`ScaleNG.ini`, `[ScaleNG]` section)

The file is read once at load; **INI changes require a game restart**.
`F7`/`F8`/`F9` below are live toggles (no restart).

| Key | Default | Effect |
|---|---|---|
| `enabled` | 1 | Plugin on/off (note: forced on after load; `0` currently inert) |
| `upscaler` | dlss | Anything else disables the plugin |
| `scale` / `renderScale` | 0.67 | **Currently NOT honored** on the live DLAA-native path (render == display always) |
| `sharpness` | 0.0 | Forwarded; the SDK declares sharpening unsupported |
| `perfQuality` | 1 | Balanced |
| `mvJittered` | 1 | `MVJittered` create flag (moot at jitter 0/0) |
| `autoExposure` | 1 | `AutoExposure` flag; no exposure texture supplied |
| `appId` | 241534720 | NGX application ID |
| `ngxApiVersion` | 21 | NGX API version passed to Init |
| `shadowHandoff` | 1 | Write eval output into the presented frame (also F8) |
| `abWindow` | 0 | 0 = steady manual control; nonzero auto-alternates handoff ON/OFF per N presents (bot captures) |
| `realInputs` | 0 | Engine MV/depth instead of zero placeholders (also F9); fail-closed to zeros |
| `dlaa` | 0 | **1 currently yields no eval path (silent no-op)** |
| `hud` | 1 | HUD overlay bit (overlay drawing itself inert) |
| `legacyScale`, `passive`, `jitterPattern` | 0, 0, halton | Parsed but ineffectual (logged only) |

`[bridge]` keys (`helper`, `replaceOutput`, `deferredOutput`, `queueCopy`)
are parsed but ineffectual while bridge creation is disabled.
Deployment warning: the test runner deploys ASI/helper but does **not**
sync `ScaleNG.ini` — a stale deployed INI silently wins. Compare
`dist\ScaleNG.ini` with the deployed file by hand after changing defaults.

## Live controls (while driving, logged)

| Key | Effect |
|---|---|
| F8 | Handoff ON/OFF — visibly gates whether DLSS output reaches the screen |
| F9 | Zero placeholders ↔ validated engine MV/depth (logged `REAL`/`ZERO` + reason) |
| F7 | HDR ↔ LDR color mode (recreates the feature) |
| F10 | Legacy DLAA/HUD toggle path (no eval effect in current config) |

Note: F9 is also read by the inert legacy HUD path (`hud: overlay` log
only; no pixels in current config). If both logs appear on one press, the
`shadow-eval inputs … (F9)` line is the authoritative one.

## Disable / uninstall / rollback

- Disable: rename `Bin64\plugins\ScaleNG.asi` (e.g. append `.disabled`) and
  restart the game. No registry or system changes are ever made.
- Uninstall: delete `ScaleNG.asi`, `ScaleNG.ini`, `ScaleNG.log`,
  `ScaleNG_NGX_helper.exe` from the plugin directory; restore the
  `dxgi.dll` proxy name if you renamed it.
- Rollback a bad build: the test runner keeps a per-run
  `deployment_backup`; restore the backed-up ASI/helper/INI and restart.
- Rollback a bad source change: every feature documents its reversal in
  STATUS.md (usually: revert the hunk, rebuild, rerun).

## What counts as verified

Plugin initialization and increasing D3D12 Present activity verify that the
game is rendering while the plugin is loaded. They do not prove DLSS injection.
Require an explicit injection marker and a captured-frame or manual visual
comparison before claiming DLSS output or quality. Current evidence is tracked
in [STATUS.md](STATUS.md).

Older expected log strings and performance estimates preserved in
[TECHNICAL_REFERENCE.md](TECHNICAL_REFERENCE.md) describe intended behavior or
historical observations, not guarantees of the current build.
