# ScaleNG.Drive - Tool Requirements

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
| **BeamNGpy** | 1.36+ | Python API for BeamNG automation | `pip install beamngpy` |
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
python scripts/autonomous_test.py
```
- 320 second test duration
- Monitors `ScaleNG.log` for success/failure markers
- Requires `psutil` for process management: `pip install psutil`

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

# Python packages
pip install beamngpy psutil

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
| BeamNGpy | 1.36 | 2026-10 |

---

## Troubleshooting

| Issue | Solution |
|-------|----------|
| `vcvars64.bat` not found | Run `Developer Command Prompt for VS 2022` or call `vcvars64.bat` manually |
| `MSBuild` not found | Install MSBuild via VS Installer |
| `nvngx_dlss.dll` not found | Download NVIDIA DLSS SDK, copy validated `nvngx_dlss.dll` to `dist/` |
| `winmm.dll` not loading | Ensure Ultimate ASI Loader `winmm.dll` is in `Bin64/` |
| `dxgi.dll` not loading | Windows loads system `dxgi.dll` from System32; proxy approach needs different strategy |
| Level not loading | BeamNG v0.39 doesn't support `-level` CLI arg; use Lua script or BeamNGpy |
| `beamngpy` connection fails | Game doesn't expose TCP port; use `-tcom -tport` args |

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
│   ├── autonomous_test.py     # 320s test runner
│   ├── launch_test.bat        # Batch test launcher
│   └── BeamNG_LevelLoader.pmc # Macro template
├── docs/
│   ├── CACHE.md               # Fast lookup cache
│   ├── DLSS_INTEGRATION_PLAN.md
│   ├── ARCHITECTURE_REWRITE_PLAN.md
│   ├── RESHADE_INTEGRATION_LESSONS.md
│   └── PROJECT_LOG.md
└── research/
    └── dlss_sdk_370/
        └── nvngx_dlss.dll     # Validated DLSS snippet
```

---

*Last updated: 2026-10-02*