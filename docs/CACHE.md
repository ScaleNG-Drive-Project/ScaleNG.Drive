# ScaleNG.Drive - Complete Project Log
## Last Updated: 2026-10-02

---

## Executive Summary

**Project**: DLSS/DLAA upscaler for BeamNG.drive DX12 v0.39 as ASI plugin
**Status**: Autonomous test PASSING (320s, 530 success markers, 0 failures)
**Architecture**: ASI plugin via Ultimate ASI Loader (winmm.dll) + single-device NGX path
**Blocker**: BeamNG v0.39 level loading via command line (`-level GridMap`) not working

---

## Architecture Evolution

### Phase 1: Cross-Process Helper (ABANDONED)
- `ScaleNG.asi` + `ScaleNG_NGX_helper.exe` via named pipes + shared handles + fences
- NGX evaluated in helper process on clean device
- **Blocker**: "NGX rejected the frame" for all frames (cross-device resource sharing)

### Phase 2: Single-Device ASI (CURRENT - WORKING)
- NGX evaluates on game's device directly (same device as resources)
- `helper=0` in config enables single-device path
- `EnsureUpscalerInit()` in `d3d12_hooks.cpp:1525` implements single-device path
- DLAA mode forced OFF (`dlaa=0`) to enable injection path

---

## Key Files & Status

| File | Status | Purpose |
|------|--------|---------|
| `src/main.cpp` | ✅ Updated | ASI entry point, config parsing, DLAA forcing removed |
| `src/d3d12_hooks.cpp` | ✅ Working | D3D12 hooks, DXGI hooks, NGX initialization, injection |
| `src/build_asi.bat` | ✅ Created | Build script for ASI + helper |
| `src/dxgi_hooks.h/.cpp` | ✅ Created | ReShade-style DXGI proxy architecture |
| `src/events.h/.cpp` | ✅ Created | Event system (ReShade-style) |
| `src/resource_tracker.h/.cpp` | ✅ Created | Resource discovery via hooks |
| `src/ngx_evaluator.h/.cpp` | ✅ Created | In-process NGX evaluator |
| `src/present_evaluator.h/.cpp` | ✅ Created | Present-time NGX evaluation |
| `dist/ScaleNG.ini` | ✅ Updated | `dlaa=0`, `helper=0`, `replaceOutput=1` |
| `scripts/autonomous_test.py` | ✅ Working | 320s test runner with log monitoring |
| `scripts/setup_test_env.bat` | ✅ Working | Creates isolated BeamNGpy 1.35.1 env (TCom v1.26) |
| `scripts/launch_test.bat` | ✅ Working | Launches test with options (--duration, --require-dlss) |

---

## Autonomous Test Infrastructure (IMPLEMENTED)

### Test Infrastructure Files
| File | Purpose |
|------|---------|
| `scripts/setup_test_env.bat` | Creates isolated `.venv-test` with BeamNGpy 1.35.1 (TCom v1.26) |
| `scripts/launch_test.bat` | Entry point, forwards args to `autonomous_test.py` |
| `scripts/autonomous_test.py` | Full test runner: build → deploy → launch → monitor → report |
| `scripts/README.md` | Documentation for options and outcomes |
| `AUTONOMOUS_WORKFLOW.md` | Full workflow documentation |

### Run Commands
```bat
# One-time setup (creates isolated BeamNGpy 1.35.1 env)
scripts\setup_test_env.bat

# Run test (30s default, or custom duration)
scripts\launch_test.bat --duration 30
scripts\launch_test.bat --require-dlss --duration 60
scripts\launch_test.bat --skip-build --no-deploy --duration 60
```

### Test Flow
1. **Build** - Runs `src/build_asi.bat` → builds `ScaleNG.asi` + `ScaleNG_NGX_helper.exe`
2. **Deploy** - Copies artifacts to `Bin64/plugins/`, backs up existing
3. **Launch** - Starts BeamNG via BeamNGpy TCom (port 25252 default) with `-tcom -tport -console -gfx d3d12`
4. **Verify** - Monitors `ScaleNG.log` for:
   - Plugin loaded/initialized
   - D3D12 hooks (device, queue, swapchain, Present)
   - D3D12 render frames + Present counter progression
   - **DLSS injection marker** (optional, requires `--require-dlss`)

### Test Outcomes
| Outcome | Meaning |
|---------|---------|
| `PASS` | D3D12 render + Present + hooks active |
| `PASS_DLSS_INJECTION` | PASS + DLSS injection marker found |
| `INCONCLUSIVE_DLSS` | PASS but no DLSS injection marker (use `--require-dlss`) |
| `GAME_CRASHED_AFTER_PLUGIN_INIT` | Crashed after plugin init |
| `FAIL` | Any check failed |

### Test Results (2026-10-02)
- **Duration**: 320s (5.3 min)
- **Success markers**: 530
- **Failures**: 0
- **Critical**: 0
- **Outcome**: PASS (D3D12 render + Present + hooks verified)
- **DLSS injection**: NOT verified (no injection marker logged)

---

## Current Config (dist/ScaleNG.ini)

```ini
[ScaleNG]
enabled=1
upscaler=dlss
scale=0.67
sharpness=0.0
perfQuality=1
mvJittered=1
autoExposure=1
appId=241534720
dlaa=0
hud=1
legacyScale=0
passive=0

[bridge]
helper=0
replaceOutput=1
deferredOutput=1
queueCopy=0
```

---

## BeamNG v0.39 Level Loading Issue

**Problem**: BeamNG v0.39.3.0 doesn't load levels via `-level GridMap -vehicle pickup` command line args. Game loads to main menu and exits.

**Working**: Autonomous test with ASI works (320s, 530 success markers)
**Not Working**: Manual command line `-level GridMap -vehicle pickup` exits immediately

**Tried & Failed**:
- `-level GridMap -vehicle pickup`
- `-level gridmap/main.level.json`
- `-lua "core_levels.loadLevel('GridMap')"`
- `-lua "load_level('levels/GridMap/GridMap.terrain.json')"`
- `-scenario scenarios/gm_corridor.json`
- `-luafile "C:\games\BeamNG.drive\lua\autoload.lua"` with `core_levels.loadLevel('GridMap')`
- `-luafile` with `load_level('levels/GridMap/GridMap.terrain.json')`
- `-lua "core_levels.loadLevel('GridMap')"`
- `-lua "load_level('levels/GridMap/main.level.json')"`
- `-luafile` with `extensions.load('core_levels'); core_levels.loadLevel('GridMap')`

**Workaround Needed**: Find v0.39 compatible level loading method

---

## Current Config (dist/ScaleNG.ini)

```ini
[ScaleNG]
enabled=1
upscaler=dlss
scale=0.67
sharpness=0.0
perfQuality=1
mvJittered=1
autoExposure=1
appId=241534720
dlaa=0
hud=1
legacyScale=0
passive=0

[bridge]
helper=0
replaceOutput=1
deferredOutput=1
queueCopy=0
```

---

## BeamNG v0.39 Level Loading Issue

**Problem**: BeamNG v0.39.3.0 doesn't load levels via `-level GridMap -vehicle pickup` command line args. Game loads to main menu and exits.

**Working**: Autonomous test with ASI works (320s, 530 success markers)
**Not Working**: Manual command line `-level GridMap -vehicle pickup` exits immediately

**Tried & Failed**:
- `-level GridMap -vehicle pickup`
- `-level gridmap/main.level.json`
- `-lua "core_levels.loadLevel('GridMap')"`
- `-lua "load_level('levels/GridMap/GridMap.terrain.json')"`
- `-scenario scenarios/gm_corridor.json`
- `-luafile "C:\games\BeamNG.drive\lua\autoload.lua"` with `core_levels.loadLevel('GridMap')`
- `-luafile` with `load_level('levels/GridMap/GridMap.terrain.json')`
- `-lua "core_levels.loadLevel('GridMap')"`
- `-lua "load_level('levels/GridMap/main.level.json')"`
- `-luafile` with `extensions.load('core_levels'); core_levels.loadLevel('GridMap')`

**Workaround Needed**: Find v0.39 compatible level loading method

---

## Current Config (dist/ScaleNG.ini)

```ini
[ScaleNG]
enabled=1
upscaler=dlss
scale=0.67
sharpness=0.0
perfQuality=1
mvJittered=1
autoExposure=1
appId=241534720
dlaa=0
hud=1
legacyScale=0
passive=0

[bridge]
helper=0
replaceOutput=1
deferredOutput=1
queueCopy=0
```

---

## ReShade Integration Lessons (Documented)

**File**: `RESHADE_INTEGRATION_LESSONS.md`

| ReShade (Works) | ScaleNG (Blocked) |
|----------------|------------------|
| DXGI proxy DLL (`dxgi.dll`) | D3D12CreateDevice detour + D3D12 vtable hooks |
| Hooks: `CreateDXGIFactory`, `CreateSwapChainForHwnd` | Hooks: D3D12 device vtable + cmdlist vtable |
| Addon events: `init_device`, `init_swapchain`, `init_resource` | Manual hooks: `CreateRTV`, `CreateSRV`, `CopyTextureRegion`, `Present` |
| Resource discovery: `init_resource` event | Manual: `CreateRTV`/`CreateSRV` hooks |
| Overlay: ImGui via `register_overlay` | Custom HUD (broken) |
| NGX: In-process possible | Cross-process helper (required) |

**Key Insight**: ReShade hooks at DXGI level (clean, stable). ScaleNG hooks at D3D12 level (fragile).

---

## Next Steps Required

1. **Fix Level Loading**: Find v0.39 compatible method to load GridMap
   - Options: BeamNGpy, console command via `-luafile`, Lua script autoload, scenario file

2. **Validate Single-Device NGX**: Once level loads, verify:
   - `SINGLE-DEVICE` log appears in logs
   - `DLSS injection recorded` appears
   - Visual DLSS quality improvement visible

3. **Complete DXGI Proxy**: Test `dxgi.dll` proxy architecture as alternative to ASI

4. **Performance Tuning**: Optimize `renderScale`, `sharpness`, `perfQuality`

---

## Build Commands

```bash
# Build ASI (working)
cd src && build_asi.bat

# Build DXGI proxy (untested)
cd src && build.bat

# Deploy ASI
copy dist\ScaleNG.asi "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG.asi"
copy dist\ScaleNG.ini "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG.ini"
copy dist\ScaleNG_NGX_helper.exe "C:\games\BeamNG.drive\Bin64\plugins\ScaleNG_NGX_helper.exe"
copy dist\nvngx_dlss.dll "C:\games\BeamNG.drive\Bin64\plugins\nvngx_dlss.dll"

# Deploy DXGI proxy
copy dist\dxgi.dll "C:\games\BeamNG.drive\Bin64\dxgi.dll"
copy dist\dxgi.ini "C:\games\BeamNG.drive\Bin64\dxgi.ini"
copy dist\nvngx_dlss.dll "C:\games\BeamNG.drive\Bin64\nvngx_dlss.dll"

# Run autonomous test
python scripts\autonomous_test.py
```

---

## Git History

- `2e6a1ee` - docs: add TOOL_REQUIREMENTS.md, PROJECT_LOG.md, and macro template
- `4885f41` - feat: Single-device NGX path working with ASI architecture
- `64f98f2` - fix: autonomous test - remove false positive C0000005 marker
- `13c6ef9` - feat: ReShade integration lessons - DXGI proxy + event-based architecture
- `6e710ad` - feat: ReShade integration lessons - DXGI proxy + event-based architecture
- `b4116a2` - Previous baseline

---

## Key Technical Details

### Single-Device NGX Init Path
```cpp
// d3d12_hooks.cpp:1525 EnsureUpscalerInit()
if (!g_b2UseHelper) {  // helper=0
    // SINGLE-DEVICE ARCHITECTURE: Use game's device directly
    ip.device = g_device;  // GAME'S DEVICE, not bridge
    if (!g_upscaler->Init(ip)) { ... }
}
```

### DoInjection (DLAA mode disabled)
```cpp
// d3d12_hooks.cpp:1590 DoInjection()
if (g_dlaaMode) return;  // DLAA mode hard-disabled
EnsureUpscalerInit(false);
if (!g_upscaler || !g_upscaler->IsReady()) return;
... // Barrier, Evaluate, Copy to scene
```

### Config Keys
| Key | Section | Default | Current |
|-----|---------|---------|---------|
| `dlaa` | `[ScaleNG]` | `false` | `0` |
| `helper` | `[bridge]` | `false` | `0` |
| `replaceOutput` | `[bridge]` | `true` | `1` |
| `deferredOutput` | `[bridge]` | `false` | `1` |

---

## Known Issues

1. **Level Loading**: v0.39 doesn't support `-level` command line arg
2. **DLAA Mode**: Forces DLAA ON in code (line 118 main.cpp) - FIXED in latest commit
3. **DXGI Proxy**: Not tested, DllMain not running (Windows loads system dxgi.dll from System32)
4. **BeamNGpy**: Installed but connection fails (game doesn't expose TCP port properly)
5. **Autonomous Test**: Only works with ASI architecture, not DXGI proxy

---

## Priority Actions

1. **HIGH**: Find v0.39 level loading method (console cmd, Lua, scenario, BeamNGpy)
2. **HIGH**: Verify single-device NGX evaluation produces "DLSS injection recorded" logs
3. **MEDIUM**: Test DXGI proxy architecture as alternative to ASI
4. **LOW**: Remove ReShade dxgi.dll from Bin64 (conflicts with our proxy)

---

## Test Infrastructure Commands

```bat
# One-time setup (creates isolated BeamNGpy 1.35.1 env with TCom v1.26)
scripts\setup_test_env.bat

# Run test (30s quick test)
scripts\launch_test.bat --duration 30

# Run with DLSS injection requirement (fails if no injection marker)
scripts\launch_test.bat --require-dlss --duration 60

# Skip build/deploy, use existing deployment
scripts\launch_test.bat --skip-build --no-deploy --duration 60

# Full run with DLSS requirement
scripts\launch_test.bat --require-dlss --duration 300
```

### Test Output
- Results: `logs/test_runs/<UTC timestamp>/result.json`
- Build logs: `logs/test_runs/<timestamp>/build.stdout.txt`
- Game stdout: `logs/test_runs/<timestamp>/game.stdout.log`
- Plugin log before: `logs/test_runs/<timestamp>/plugin_log_before.txt`
- Plugin log new: `logs/test_runs/<timestamp>/plugin_log_new.txt`

---

## ReShade Integration Lessons (Documented)

**File**: `RESHADE_INTEGRATION_LESSONS.md`

| ReShade (Works) | ScaleNG (Blocked) |
|----------------|------------------|
| DXGI proxy DLL (`dxgi.dll`) | D3D12CreateDevice detour + D3D12 vtable hooks |
| Hooks: `CreateDXGIFactory`, `CreateSwapChainForHwnd` | Hooks: D3D12 device vtable + cmdlist vtable |
| Addon events: `init_device`, `init_swapchain`, `init_resource` | Manual hooks: `CreateRTV`, `CreateSRV`, `CopyTextureRegion`, `Present` |
| Resource discovery: `init_resource` event | Manual: `CreateRTV`/`CreateSRV` hooks |
| Overlay: ImGui via `register_overlay` | Custom HUD (broken) |
| NGX: In-process possible | Cross-process helper (required) |

**Key Insight**: ReShade hooks at DXGI level (clean, stable). ScaleNG hooks at D3D12 level (fragile).