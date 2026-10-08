# Automated BeamNG integration test

Current test status and agenda: [docs/STATUS.md](../docs/STATUS.md). This guide
describes the runnable procedure; it does not claim DLSS injection is verified.

The fast camera-constant-buffer validator regression can be run independently
with `scripts\test_camera_cb.bat`; it checks the observed 12,500 far plane and
sanity-bound rejection. It does not replace the live BeamNG integration test.

From the repository root, run:

```bat
scripts\launch_test.bat
```

The runner builds `src/build_asi.bat`, backs up the deployed ASI/helper, deploys
the new artifacts, launches a fresh BeamNG process through BeamNGpy TCom, waits
for freeroam and a vehicle, then checks fresh ScaleNG log output for plugin
initialization, D3D12 device/queue/swapchain/Present activity, and fatal
errors. Default success verifies the render path and hooks; add `--require-dlss`
to require a DLSS injection marker. `INCONCLUSIVE_DLSS` means rendering worked
but no injection marker was logged.

Options are forwarded to `autonomous_test.py`, for example:

```bat
scripts\launch_test.bat --duration 30
scripts\launch_test.bat --skip-build --no-deploy --duration 60
```

Requirements: Windows, Visual Studio C++ build tools, BeamNG.drive at
`C:\Games\BeamNG.drive`, Python, and the BeamNGpy version matching the game
protocol. BeamNG 0.39.3 uses TCom v1.26; create a dedicated environment using
`scripts\setup_test_env.bat`.
The test requirements pin BeamNGpy 1.35.1 (TCom v1.26); the global BeamNGpy
1.36 uses protocol v1.27 and will be rejected by this game. Its optional web UI
dependencies are deliberately excluded from this test environment.
Do not start another BeamNG instance during a run. Each run creates
`logs/test_runs/<UTC timestamp>/result.json` with its checks, outcome, and
deployment backup location; build output and fresh plugin logs are kept beside
it. The current deployed `ScaleNG.ini` is preserved if present; if absent, the
runner installs the build's default config. The runner does NOT sync
INI — a stale deployed INI silently wins. Compare `dist\ScaleNG.ini`
with the deployed file by hand after changing defaults (see
[docs/INSTALL.md](../docs/INSTALL.md)).

Outcomes include `PASS`, `PASS_DLSS_EVAL`, `INCONCLUSIVE_DLSS`,
`GAME_CRASHED_AFTER_PLUGIN_INIT`, and `FAIL`. (`PASS_DLSS_INJECTION` is
a dead token — accepted when scanning old logs but no longer emitted.)
A plain `PASS` verifies
D3D12 render / Present activity and ScaleNG hooks, not DLSS image
quality — it is awarded even with zero DLSS evidence unless
`--require-dlss` is passed (which requires eval markers, including
`shadow-eval ok`, and yields `INCONCLUSIVE_DLSS` without them;
`INCONCLUSIVE_DLSS` is only emitted with `--require-dlss`).
`PASS_DLSS_EVAL` counts `shadow-eval ok` markers for zeros AND real
inputs alike — check the `why=real` / `why=<reason>` input lines and the
per-eval `handoff` bit, not just the token. Eval `ok` lines are sampled
(first 10, then every 600), so totals come from the `#N` counters, not
line counts; `FAILED` logs the first 10 only and `FAULTED` the first 10
+ every 600. Visual quality still requires a captured-frame comparison
or manual inspection.
