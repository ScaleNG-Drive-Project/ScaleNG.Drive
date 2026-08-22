# ScaleNG.Drive

DLSS render-scale upscaler for **BeamNG.drive** (DX12, v0.39), implemented as an ASI plugin loaded by
OptiScaler's ASI loader. Upscaling only — no frame generation.

## Layout

```
ScaleNG.Drive/
├── src/        Plugin source: main.cpp, camera_cb, d3d12_hooks, dlss_ngx, log.h, upscaler.h,
│               build.bat, vendor\ (MinHook + NVSDK_NGX headers)
├── dist/       Deployables: ScaleNG.asi + ScaleNG.ini  (copy these into BeamNG Bin64\plugins)
├── docs/       README.md (full docs incl. design notes + verification guide) ·
│               CACHE.md (agent cache — update every prompt) · NEMOTRON_PROMPT.md
└── research/   PIX GPU capture + C++ replay export + saved shader CSOs (ground truth)
```

## Quick start

1. Build: double-click `src\build.bat` → produces `dist\ScaleNG.asi` (requires VS2022 C++ tools).
2. Install: copy `dist\ScaleNG.asi` + `dist\ScaleNG.ini` into BeamNG `Bin64\plugins\`
   (OptiScaler must be `Bin64\dxgi.dll` with `LoadAsiPlugins=1`).
3. Drop `nvngx_dlss.dll` (NVIDIA DLSS SDK) into the same folder.
4. Launch the game; check `ScaleNG.log` next to the ASI. See `docs\README.md` §8 for expected log lines.

## Docs

- `docs\README.md` — full project documentation (architecture findings, design decisions, file map, in-game verification guide).
- `docs\CACHE.md` — fast-lookup agent cache.