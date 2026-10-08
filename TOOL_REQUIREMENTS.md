# ScaleNG.Drive - Tool Requirements

> Current operator priority: see [docs/STATUS.md](docs/STATUS.md), then use
> [scripts/README.md](scripts/README.md) for the tested BeamNG environment.
> This file inventories tools; its historical optional-tool and installation
> notes are not required steps for the automated test.

## Build Tools

### Required (Must Have)

| Tool | Version | Purpose | Install Command |
|------|---------|---------|-----------------|
| **Visual Studio 2022 Community** | 17.x (v143/v144 toolset) | C++ compiler, linker, MSBuild | [Download](https://visualstudio.microsoft.com/vs/community/) |
| **Windows 10/11 SDK** | 10.0.19041+ | Windows headers, libraries | VS Installer → Individual components |
| **CMake** | 3.20+ | Build system (alternative) | `winget install Kitware.CMake` |
| **Git** | 2.40+ | Version control | `winget install Git.Git` |
| **Python** | 3.11+ | Autonomous test runner, scripts | `winget install Python.Python.3.12` |

### Optional but Recommended

| Tool | Version | Purpose | Install Command |
|------|---------|---------|-----------------|
| **BeamNGpy (test venv)** | 1.35.1 | BeamNG 0.39.3 TCom v1.26 automation; install through `scripts\setup_test_env.bat` |
| **AutoHotkey v2** | 2.0+ | Macro automation for level loading | `winget install AutoHotkey.AutoHotkey` |
| **Pulover's Macro Creator** | 5.0.5+ | Visual macro recorder/player | [Download](https://www.macrocreator.com/) |
| **7-Zip** | 23.x | Archive extraction for level inspection | `winget install 7zip.7zip` |
| **VS Code** | 1.85+ | Code editor with C++ extensions | `winget install Microsoft.VisualStudioCode` |
| **DebugView** | 4.90+ | Debug output capture | [Sysinternals](https://learn.microsoft.com/sysinternals/downloads/debugview) |
| **DebugDiag** | 2.3+ | Crash dump analysis | [Microsoft](https://www.microsoft.com/download/details.aspx?id=58210) |

---

## Runtime Requirements

### BeamNG.drive

| Component | Version | Notes |
|-----------|---------|-------|
| **BeamNG.drive** | 0.39.3.0 (build 20967) | Steam version recommended |
| **NVIDIA GPU** | RTX 20/30/40 series | DLSS requires RTX |
| **NVIDIA Driver** | 550+ | DLSS 3.x support |
| **Windows** | 10 21H2+ / 11 | D3D12 requirement |

### NVIDIA DLSS SDK

| File | Location | Source |
|------|----------|--------|
| `nvngx_dlss.dll` | `dist/nvngx_dlss.dll` + `Bin64/plugins/` | NVIDIA DLSS SDK 3.7.0+ |
| `nvngx_dlss.dll` (validated) | `research/dlss_sdk_370/nvngx_dlss.dll` | Pre-validated snippet |

### Ultimate ASI Loader

| File | Location | Source |
|------|----------|--------|
| `winmm.dll` | `Bin64/winmm.dll` | [Ultimate ASI Loader 9.7.4](https://github.com/Ultimate-ASI-Loader/Ultimate-ASI-Loader/releases/tag/v9.7.4) |

### ReShade (Reference/Testing)

| File | Location | Source |
|------|----------|--------|
| `ReShade64.dll` → `dxgi.dll` | `Bin64/dxgi.dll` | [ReShade 6.x](https://reshade.me/) |

> Note (2026-10-08): installing any proxy as `Bin64\dxgi.dll` conflicts
> with UAL ASI loading — on the test installation the pre-existing proxy
> is reversibly disabled (see [docs/INSTALL.md](docs/INSTALL.md)). Do not
> combine ReShade-as-dxgi with ASI testing unless coexistence is the
> subject of the test.

---

## Development Environment Setup

### Visual Studio 2022 Workloads

Required workloads in VS Installer:
- ✅ **Desktop development with C++**
  - MSVC v143 - VS 2022 C++ x64/x86 build tools
  - Windows 10 SDK (10.0.19041.0 or newer)
  - C++ CMake tools for Windows
  - Windows 10 SDK (10.0.22621.0 or newer)

### VS Code Extensions (Recommended)

```json
{
  "recommendations": [
    "ms-vscode.cpptools",
    "ms-vscode.cmake-tools",
    "ms-vscode.cpptools-extension-pack",
    "vadimcn.vscode-lldb",
    "ms-python.python",
    "ms-python.vscode-pylance",
    "github.vscode-github-actions",
    "github.vscode-pull-request-github"
  ]
}
```

---

## Build Scripts

### ASI Build (Working)
```bash
cd src
build_asi.bat
```
Outputs: `dist/ScaleNG.asi`, `dist/ScaleNG.ini`, `dist/ScaleNG_NGX_helper.exe`, `dist/nvngx_dlss.dll`

### DXGI Proxy Build (Development)
```bash
cd src
build.bat
```
Outputs: `dist/dxgi.dll`, `dist/dxgi.ini`, `dist/nvngx_dlss.dll`

### Deploy Scripts

**ASI Deploy:**
```cmd
copy dist\ScaleNG.asi "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG.asi"
copy dist\ScaleNG.ini "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG.ini"
copy dist\ScaleNG_NGX_helper.exe "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG_NGX_helper.exe"
copy dist\nvngx_dlss.dll "C:\games\BeamNG.drive\Bin64\plugins\nvngx_dlss.dll"
```

**DXGI Proxy Deploy:**
```cmd
copy dist\dxgi.dll "C:\games\BeamNG.drive\Bin64\dxgi.dll"
copy dist\dxgi.ini "C:\games\BeamNG.drive\Bin64\dxgi.ini"
copy dist\nvngx_dlss.dll "C:\games\BeamNG.drive\Bin64\nvngx_dlss.dll"
```

---

## Testing Tools

### Autonomous Test
```bash
scripts\setup_test_env.bat
scripts\launch_test.bat --duration 30
```
- Uses isolated `.venv-test`; do not use global BeamNGpy 1.36 (TCom v1.27).
- Builds, deploys with backups, starts a fresh BeamNG process, loads smallgrid
  and a vehicle, and verifies plugin initialization plus live D3D12 Present activity.
- Output: `logs/test_runs/<UTC timestamp>/result.json` and diagnostic logs.
- PASS proves the game rendered/presented frames and hooks initialized. It does
  not prove DLSS injection or image quality. Use `--require-dlss` to require an
  injection marker; absent marker is inconclusive, not a pass for DLSS.
- Options and outcomes: `scripts/README.md`.

### Debug Logging
- **ScaleNG.log**: `C:\games\BeamNG.drive\Bin64\plugins\ScaleNG.log`
- **ReShade.log**: `C:\games\BeamNG.drive\Bin64\ReShade.log`
- **BeamNG log**: `C:\Users\<user>\AppData\Local\BeamNG\BeamNG.drive\logs\`

### Crash Dumps
- **Local**: `C:\games\BeamNG.drive\Bin64\CrashDumps\`
- **WER**: `C:\Users\<user>\AppData\Local\CrashDumps\`

---

## Quick Install Script (PowerShell)

```powershell
# Run as Administrator
winget install Microsoft.VisualStudio.2022.Community --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
winget install Kitware.CMake
winget install Git.Git
winget install Python.Python.3.12
winget install 7zip.7zip
winget install AutoHotkey.AutoHotkey

# Python test environment (run from the repository root instead of installing
# an arbitrary/global BeamNGpy version)
scripts\setup_test_env.bat

# Manual installs needed:
# 1. Visual Studio 2022 Community (with Desktop C++ workload)
# 2. Ultimate ASI Loader 9.7.4 -> copy winmm.dll to BeamNG/Bin64/
# 3. NVIDIA DLSS SDK -> copy nvngx_dlss.dll to dist/ and Bin64/plugins/
# 4. ReShade 6.x -> rename ReShade64.dll to dxgi.dll, copy to Bin64/
```

---

## Version Tracking

| Component | Version | Last Updated |
|-----------|---------|--------------|
| Visual Studio | 17.10.x | 2026-10 |
| Windows SDK | 10.0.26100 | 2026-10 |
| Python | 3.14.x | 2026-10 |
| BeamNG.drive | 0.39.3.0 (20967) | 2026-08 |
| NVIDIA Driver | 616.92+ | 2026-10 |
| NVIDIA DLSS SDK | 3.7.0+ | 2026-10 |
| ReShade | 6.3.0+ | 2026-10 |
| Ultimate ASI Loader | 9.7.4 | 2026-10 |
| BeamNGpy test environment | 1.35.1 (TCom v1.26) | 2026-10 |
| BeamNGpy global (incompatible with tested game) | 1.36 (TCom v1.27) | 2026-10 |

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `vcvars64.bat` not found | Run `Developer Command Prompt for VS 2022` or call `vcvars64.bat` manually |
| `MSBuild` not found | Install MSBuild via VS Installer |
| `nvngx_dlss.dll` not found | Download NVIDIA DLSS SDK, copy validated `nvngx_dlss.dll` to `dist/` |
| `winmm.dll` not loading | Ensure Ultimate ASI Loader `winmm.dll` is in `Bin64/` |
| `dxgi.dll` not loading | Windows loads system `dxgi.dll` from System32; proxy approach needs different strategy |
| Test reports BeamNGpy protocol mismatch | Run `scripts\setup_test_env.bat`; it pins BeamNGpy 1.35.1 for TCom v1.26 |
| Test level not loading | Use the level name `smallgrid` (default), not `gridmap.mis` |
| Test game crashes at startup | Do not pass `-windowed`; check the run's `game.stdout.txt` and BeamNG log |
| No DLSS injection marker | Render/Present may still pass; inspect result as `INCONCLUSIVE_DLSS`, not DLSS success |

---

## File Structure Reference

```
ScaleNG.Drive/
├── src/
│   ├── build.bat              # DXGI proxy build
│   ├── build_asi.bat          # ASI build (working)
│   ├── build_asi.bat          # ASI build script
│   ├── dxgi_proxy.def         # DXGI export definitions
│   ├── main.cpp               # ASI entry point
│   ├── main_dxgi.cpp          # DXGI proxy entry
│   ├── d3d12_hooks.cpp/h      # D3D12 hooks + NGX logic
│   ├── dxgi_hooks.h/cpp       # DXGI proxy hooks
│   ├── events.h/cpp           # ReShade-style events
│   ├── resource_tracker.h/cpp # Resource discovery
│   ├── ngx_evaluator.h/cpp    # In-process NGX
│   ├── present_evaluator.h/cpp# Present-time evaluation
│   ├── camera_cb.cpp          # Camera constant buffer
│   ├── dlss_ngx.h/cpp         # NGX wrapper
│   ├── ngxc_helper.cpp        # Cross-process helper
│   ├── log.h                  # Logging
│   ├── upscaler.h             # Upscaler interface
│   └── vendor/
│       ├── minhook/           # MinHook library
│       └── nvngx/             # NGX headers
├── dist/
│   ├── ScaleNG.asi            # ASI plugin
│   ├── ScaleNG.ini            # ASI config
│   ├── ScaleNG_NGX_helper.exe # Helper process
│   ├── nvngx_dlss.dll         # DLSS SDK
│   ├── dxgi.dll               # DXGI proxy
│   ├── dxgi.ini               # Proxy config
│   └── dxgi_proxy.def         # Export def
├── scripts/
│   ├── autonomous_test.py     # Build/deploy/live BeamNG integration runner
│   ├── launch_test.bat        # Batch test launcher (isolated venv)
│   ├── setup_test_env.bat     # Pinned BeamNGpy test environment setup
│   ├── requirements-test.txt  # Test runtime dependencies
│   └── README.md              # Runner usage, checks, and limits
│   └── BeamNG_LevelLoader.pmc # Macro template
├── docs/
│   ├── STATUS.md              # Current verified status and agenda
│   ├── INSTALL.md             # Supported ASI install and verification limits
│   ├── README.md              # Documentation hub
│   ├── CACHE.md               # Historical agent experiment ledger
│   └── archive/               # Superseded plans and investigations
└── research/
    └── dlss_sdk_370/
        └── nvngx_dlss.dll     # Validated DLSS snippet
```

---

*Last updated: 2026-10-02*
