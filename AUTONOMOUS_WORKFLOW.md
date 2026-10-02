# ScaleNG.Drive — Autonomous Operation Plan

**Status:** Design document — awaiting approval before implementation  
**Date:** 2026-10-02  
**Goal:** Fully automated build → deploy → launch → test → log → iterate loop

---

## 1. Autonomous Launch Configuration

### BeamNG Direct Launch (no menu)
```cmd
# Launch directly into gridmap with a vehicle, D3D12, console visible
BeamNG.drive.x64.exe -level gridmap.mis -vehicle pickup -console -gfx d3d12
```

### Available Test Maps
| Map | File | Use Case |
|-----|------|----------|
| Gridmap | `gridmap.mis` | Default test, flat, good for FPS measurement |
| Smallgrid | `smallgrid.mis` | Fast load, minimal geometry |
| West Coast USA | `west_coast_usa.mis` | Real-world complexity, stress test |
| Utah | `utah.mis` | Desert, different lighting |

---

## 2. Automation Scripts to Create

### `scripts/launch_test.bat` — Windows Batch Launcher
```bat
@echo off
setlocal

:: ==== CONFIGURATION ====
set "BEAMNG_EXE=C:\games\BeamNG.drive\Bin64\BeamNG.drive.x64.exe"
set "PLUGINS_DIR=C:\games\BeamNG.drive\Bin64\plugins"
set "LEVEL=gridmap.mis"
set "VEHICLE=pickup"
set "EXTRA_ARGS=-console -gfx d3d12"

:: ==== BUILD & DEPLOY ====
cd /d "%~dp0..\src"
call build.bat
if errorlevel 1 (
    echo [ERROR] Build failed
    exit /b 1
)

:: Copy deployables
copy /y "..\dist\ScaleNG.asi" "%PLUGINS_DIR%\"
copy /y "..\dist\ScaleNG.ini" "%PLUGINS_DIR%\"
copy /y "..\dist\ScaleNG_NGX_helper.exe" "%PLUGINS_DIR%\"
copy /y "..\dist\nvngx_dlss.dll" "%PLUGINS_DIR%\"

:: ==== LAUNCH ====
cd /d "%PLUGINS_DIR%\.."
echo [LAUNCH] Starting BeamNG with %LEVEL% %VEHICLE%
start "" "%BEAMNG_EXE%" -level %LEVEL% -vehicle %VEHICLE% %EXTRA_ARGS%

:: ==== WAIT & MONITOR ====
echo [MONITOR] Waiting for ScaleNG.log...
timeout /t 5 >nul
:WAIT_LOG
if not exist "%PLUGINS_DIR%\ScaleNG.log" (
    timeout /t 2 >nul
    goto WAIT_LOG
)
echo [MONITOR] ScaleNG.log found. Tailing...
powershell -Command "Get-Content '%PLUGINS_DIR%\ScaleNG.log' -Wait -Tail 50"
```

### `scripts/autonomous_test.py` — Python Automation (BeamNGpy)
```python
#!/usr/bin/env python3
"""
Autonomous test runner for ScaleNG.Drive
Requires: pip install beamngpy
"""
import subprocess
import time
import os
import sys
from pathlib import Path

# Config
BEAMNG_HOME = Path(r"C:\games\BeamNG.drive")
BEAMNG_EXE = BEAMNG_HOME / "Bin64" / "BeamNG.drive.x64.exe"
PLUGINS_DIR = BEAMNG_HOME / "Bin64" / "plugins"
SRC_DIR = Path(__file__).parent.parent / "src"
DIST_DIR = Path(__file__).parent.parent / "dist"

TEST_CONFIG = {
    "level": "gridmap.mis",
    "vehicle": "pickup",
    "duration_sec": 60,  # test duration
    "target_fps": 60,
}

def build_plugin():
    """Run build.bat and verify outputs."""
    print("[BUILD] Running src/build.bat...")
    result = subprocess.run(["build.bat"], cwd=SRC_DIR, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[ERROR] Build failed:\n{result.stdout}\n{result.stderr}")
        return False
    print("[BUILD] Success")
    return True

def deploy():
    """Copy dist/* to plugins/."""
    files = ["ScaleNG.asi", "ScaleNG.ini", "ScaleNG_NGX_helper.exe", "nvngx_dlss.dll"]
    for f in files:
        src = DIST_DIR / f
        dst = PLUGINS_DIR / f
        if src.exists():
            import shutil
            shutil.copy2(src, dst)
            print(f"[DEPLOY] {f} -> {dst}")
        else:
            print(f"[WARN] Missing: {src}")

def launch_beamng():
    """Launch BeamNG with test level."""
    args = [
        str(BEAMNG_EXE),
        f"-level {TEST_CONFIG['level']}",
        f"-vehicle {TEST_CONFIG['vehicle']}",
        "-console",
        "-gfx d3d12",
    ]
    print(f"[LAUNCH] {' '.join(args)}")
    proc = subprocess.Popen(args, cwd=BEAMNG_HOME / "Bin64")
    return proc

def monitor_log(log_path, duration):
    """Tail ScaleNG.log for key indicators."""
    import threading
    stop_event = threading.Event()
    
    def tail():
        with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
            f.seek(0, 2)  # seek to end
            while not stop_event.is_set():
                line = f.readline()
                if line:
                    print(f"[LOG] {line.rstrip()}")
                    # Check for success/failure markers
                    if "DLSS: feature created" in line:
                        print("[SUCCESS] NGX feature created!")
                    if "DLSS injection recorded" in line:
                        print("[SUCCESS] DLSS injection active!")
                    if "EvaluateFeature failed" in line:
                        print("[FAIL] DLSS evaluate failed")
                else:
                    time.sleep(0.5)
    
    t = threading.Thread(target=tail, daemon=True)
    t.start()
    time.sleep(duration)
    stop_event.set()
    t.join(timeout=2)

def main():
    print("=" * 60)
    print("ScaleNG.Drive Autonomous Test")
    print("=" * 60)
    
    if not build_plugin():
        sys.exit(1)
    deploy()
    
    proc = launch_beamng()
    try:
        # Wait for log to appear
        log_path = PLUGINS_DIR / "ScaleNG.log"
        for _ in range(30):
            if log_path.exists():
                break
            time.sleep(1)
        
        if log_path.exists():
            monitor_log(log_path, TEST_CONFIG["duration_sec"])
        else:
            print("[ERROR] ScaleNG.log never appeared")
    finally:
        proc.terminate()
        proc.wait(timeout=10)
    
    print("[DONE] Test complete")

if __name__ == "__main__":
    main()
```

### `scripts/ci_test.yml` — GitHub Actions CI (Future)
```yaml
# For future CI integration
name: ScaleNG Autonomous Test
on: [push, workflow_dispatch]
jobs:
  test:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install VS2022 Build Tools
        uses: microsoft/setup-msbuild@v1
      - name: Build
        run: cd src && build.bat
      - name: Deploy
        run: copy dist\* C:\games\BeamNG.drive\Bin64\plugins\
      - name: Launch BeamNG
        run: |
          start "" C:\games\BeamNG.drive\Bin64\BeamNG.drive.x64.exe -level gridmap.mis -vehicle pickup -console -gfx d3d12
      - name: Wait and collect logs
        run: |
          # Wait for log, parse for success markers
```

---

## 3. Plugin-Side Autonomous Features (Already Implemented)

| Feature | Location | Status |
|---------|----------|--------|
| Gameplay detection (camera CB patches) | `d3d12_hooks.cpp:4260` | ✅ |
| Menu/loading suppression | `InjectAtPresentImpl` | ✅ |
| 300-frame stability gate | `d3d12_hooks.cpp:4294` | ✅ |
| Cross-copy guard (UAL multi-load) | `main.cpp:155` | ✅ |
| VEH fault logging | `main.cpp:137` | ✅ |
| DRED breadcrumbs | `EnsureBridge:174` | ✅ |

**No plugin changes needed** — the plugin already handles menu→gameplay transition autonomously.

---

## 4. Verification Checklist (Per `docs/README.md` §8.2)

Autonomous test passes when **all** appear in `ScaleNG.log`:

| Stage | Expected Log Line |
|-------|-------------------|
| Load | `ScaleNG.asi loaded` + `config: renderScale=0.67 ...` |
| Device | `hooks: D3D12CreateDevice detour installed` |
| Resources | `hooks: scene color RTV ... (1920x992 R16G16B16A16_UNORM)` |
| | `hooks: motion vector RTV ... (1920x992 R16G16_FLOAT)` |
| Command List | `hooks: command list slots 15/16/21/22/26/28/46 hooked` |
| Per Frame | `hooks: frame N started (render 1286x664, jitter ...)` |
| | `hooks: camera CB patched in place` (2x/frame) |
| | `hooks: velocity CB patched in place` (1x/frame) |
| | `hooks: DLSS injection recorded for frame N` (1x/frame) |
| DLSS Init | `DLSS: loaded plugins\nvngx_dlss.dll` |
| | `DLSS: feature created (render 1286x664 -> display 1920x992)` |

---

## 5. Approval Gate

**Do not build until approved.** Upon approval:

1. Create `scripts/` folder with `launch_test.bat` and `autonomous_test.py`
2. Run `src/build.bat` once to verify toolchain
3. Deploy to `C:\games\BeamNG.drive\Bin64\plugins\`
4. Run autonomous test: `python scripts/autonomous_test.py`
5. Analyze logs → iterate

---

## 6. Stop Conditions (Rollback Triggers)

From `DLSS_INTEGRATION_PLAN.md`:
- BeamNG crashes / device removal / black window / stops presenting
- Repeated resource descriptor faults
- Candidate identity changes without resize
- Invalid helper ACKs/fences
- Only proposed target is swapchain backbuffer

---

**Ready for approval.** Reply "approved" to proceed with script creation and first build.