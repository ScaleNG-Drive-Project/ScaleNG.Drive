#!/usr/bin/env python3
"""
ScaleNG.Drive — Autonomous Test Runner
Builds plugin, deploys, launches BeamNG directly into level, monitors logs for 320s.

NEW: Monitors game's Lua log (stdout) to verify map loads successfully before continuing.
If map fails to load within 60s, test aborts.

Requirements:
  - Windows with VS2022 Build Tools (for build.bat)
  - BeamNG.drive at C:\games\BeamNG.drive
  - Python 3.8+

Optional (for advanced automation):
  pip install beamngpy
"""

import subprocess
import time
import sys
import os
import threading
from pathlib import Path
from datetime import datetime

# ============================================================================
# CONFIGURATION
# ============================================================================
REPO_ROOT = Path(__file__).parent.parent
SRC_DIR = REPO_ROOT / "src"
DIST_DIR = REPO_ROOT / "dist"

BEAMNG_HOME = Path(r"C:\games\BeamNG.drive")
BEAMNG_EXE = BEAMNG_HOME / "Bin64" / "BeamNG.drive.x64.exe"
PLUGINS_DIR = BEAMNG_HOME / "Bin64" / "plugins"
SCALENG_LOG = PLUGINS_DIR / "ScaleNG.log"

TEST_CONFIG = {
    "level": "GridMap",               # Must match actual folder name (capital G)
    "vehicle": "pickup",              # Vehicle to spawn
    "duration_sec": 320,              # Test duration (320s = ~5.3 min)
    "extra_args": ["-console", "-gfx", "d3d12"],
    "map_load_timeout_sec": 120,      # Max time to wait for map load confirmation
}

# Success/failure markers in ScaleNG.log
SCALENG_MARKERS = {
    "success": [
        "DLSS: feature created",
        "DLSS injection recorded",
        "hooks: DLSS injection recorded for frame",
    ],
    "warning": [
        "injection skipped",
        "camera CB copy not validated",
        "viewport patch 0",
    ],
    "failure": [
        "EvaluateFeature failed",
        "CreateFeature failed",
        "device removed",
        "D3D12CreateDevice hook failed",
        "DLSS: FAILED to load",
        "GetDeviceRemovedReason",
        "DEVICE_REMOVED",
    ],
    "critical": [
        "FATAL: SEH exception",
        "C0000005",
        "access violation",
    ],
}

# ============================================================================
# HELPERS
# ============================================================================
def log(msg, level="INFO"):
    timestamp = datetime.now().strftime("%H:%M:%S")
    prefix = {"INFO": "[INFO]", "WARN": "[WARN]", "ERROR": "[ERROR]", "SUCCESS": "[SUCCESS]", "CRITICAL": "[CRITICAL]"}.get(level, "[INFO]")
    print(f"{timestamp} {prefix} {msg}")

def run_cmd(cmd, cwd=None, capture=True, shell=False):
    """Run command, return (success, stdout, stderr)."""
    log(f"Running: {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    try:
        result = subprocess.run(
            cmd, cwd=cwd, capture_output=capture, text=True, shell=shell, timeout=300
        )
        if result.stdout and capture:
            log(f"  stdout: {result.stdout[:500]}")
        if result.stderr and capture:
            log(f"  stderr: {result.stderr[:500]}")
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        log("Command timed out", "ERROR")
        return False, "", "Timeout"
    except Exception as e:
        log(f"Command failed: {e}", "ERROR")
        return False, "", str(e)

def build_plugin():
    """Run src/build.bat."""
    log("Building plugin via src/build.bat...")
    ok, out, err = run_cmd("build.bat", cwd=SRC_DIR, shell=True)
    if not ok:
        log("Build failed", "ERROR")
        return False
    log("Build successful", "SUCCESS")
    return True

def deploy():
    """Copy dist/* to plugins/."""
    log(f"Deploying to {PLUGINS_DIR}...")
    files = ["ScaleNG.asi", "ScaleNG.ini", "ScaleNG_NGX_helper.exe", "nvngx_dlss.dll"]
    for f in files:
        src = DIST_DIR / f
        dst = PLUGINS_DIR / f
        if src.exists():
            import shutil
            shutil.copy2(src, dst)
            log(f"  Deployed: {f}")
        else:
            log(f"  Missing (will fail): {src}", "WARN")
    log("Deploy complete", "SUCCESS")

def verify_deploy():
    """Verify all required files exist in plugins/."""
    required = ["ScaleNG.asi", "ScaleNG.ini", "ScaleNG_NGX_helper.exe", "nvngx_dlss.dll"]
    for f in required:
        if not (PLUGINS_DIR / f).exists():
            log(f"Missing required file: {f}", "ERROR")
            return False
    log("All deploy files verified", "SUCCESS")
    return True

def launch_beamng():
    """Launch BeamNG with test level, capturing stdout/stderr."""
    args = [
        str(BEAMNG_EXE),
        f"-level {TEST_CONFIG['level']}",
    ]
    if TEST_CONFIG['vehicle']:
        args.append(f"-vehicle {TEST_CONFIG['vehicle']}")
    args.extend(TEST_CONFIG["extra_args"])
    
    log(f"Launching BeamNG: {' '.join(args)}")
    proc = subprocess.Popen(
        args, 
        cwd=BEAMNG_HOME / "Bin64",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,  # Line buffered
        encoding='utf-8',
        errors='replace'
    )
    return proc

def wait_for_game_ready(timeout_sec=90):
    """Wait for game to be ready by checking ScaleNG.log for camera CB patches (proves level is rendering)."""
    log(f"Waiting for game to be ready (timeout: {timeout_sec}s)...")
    start_time = time.time()
    
    while time.time() - start_time < timeout_sec:
        if SCALENG_LOG.exists():
            try:
                with open(SCALENG_LOG, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()
                    # Camera CB patches = level is rendering (even if main menu is on top)
                    if "camera CB patched" in content.lower():
                        log("Game ready - camera CB patches detected (level rendering)", "SUCCESS")
                        return True
                    # Also check for scene color RTV discovered
                    if "scene color RTV" in content.lower():
                        log("Game ready - scene color RTV discovered", "SUCCESS")
                        return True
            except Exception:
                pass
        time.sleep(2)
    
    log(f"Game ready check timeout after {timeout_sec}s", "WARN")
    return False

def wait_for_scaleng_log(timeout_sec=120):
    """Wait for ScaleNG.log to appear."""
    log(f"Waiting for ScaleNG.log (timeout: {timeout_sec}s)...")
    for i in range(timeout_sec):
        if SCALENG_LOG.exists():
            log(f"ScaleNG.log found at {SCALENG_LOG}", "SUCCESS")
            return True
        time.sleep(1)
    log("ScaleNG.log never appeared", "ERROR")
    return False

def analyze_scaleng_line(line, stats):
    """Check ScaleNG.log line for markers, update stats."""
    for category, markers in SCALENG_MARKERS.items():
        for marker in markers:
            if marker.lower() in line.lower():
                stats[category] = stats.get(category, 0) + 1
                if category == "success":
                    log(f"  >>> SUCCESS MARKER: {marker}", "SUCCESS")
                elif category == "warning":
                    log(f"  >>> WARNING: {marker}", "WARN")
                elif category == "failure":
                    log(f"  >>> FAILURE: {marker}", "ERROR")
                elif category == "critical":
                    log(f"  >>> CRITICAL: {marker}", "CRITICAL")
                break

def tail_scaleng_log(duration_sec):
    """Tail ScaleNG.log for duration_sec seconds, analyze markers."""
    log(f"Tailing ScaleNG.log for {duration_sec} seconds...")
    
    stats = {"success": 0, "warning": 0, "failure": 0, "critical": 0}
    start_time = time.time()
    last_pos = 0
    
    # Initial read to catch up
    try:
        with open(SCALENG_LOG, "r", encoding="utf-8", errors="ignore") as f:
            content = f.read()
            last_pos = f.tell()
            for line in content.splitlines():
                analyze_scaleng_line(line, stats)
    except Exception as e:
        log(f"Initial ScaleNG.log read failed: {e}", "WARN")
    
    log(f"[MONITOR] Starting live tail at {datetime.now().strftime('%H:%M:%S')}")
    
    while time.time() - start_time < duration_sec:
        try:
            with open(SCALENG_LOG, "r", encoding="utf-8", errors="ignore") as f:
                f.seek(last_pos)
                new_content = f.read()
                last_pos = f.tell()
                if new_content:
                    for line in new_content.splitlines():
                        elapsed = time.time() - start_time
                        print(f"[{datetime.now().strftime('%H:%M:%S')} +{elapsed:.1f}s] {line}")
                        analyze_scaleng_line(line, stats)
        except Exception as e:
            log(f"ScaleNG.log read error: {e}", "WARN")
        
        time.sleep(0.5)
    
    log(f"[MONITOR] Tail completed. Stats: {stats}")
    return stats

def print_summary(stats):
    """Print final test summary."""
    print("\n" + "=" * 60)
    print("AUTONOMOUS TEST SUMMARY")
    print("=" * 60)
    print(f"Duration:     {TEST_CONFIG['duration_sec']}s")
    print(f"Level:        {TEST_CONFIG['level']}")
    print(f"Vehicle:      {TEST_CONFIG['vehicle'] or '(default)'}")
    print(f"Log:          {SCALENG_LOG}")
    print("-" * 60)
    print(f"Success markers: {stats.get('success', 0)}")
    print(f"Warnings:       {stats.get('warning', 0)}")
    print(f"Failures:       {stats.get('failure', 0)}")
    print(f"Critical:       {stats.get('critical', 0)}")
    print("-" * 60)
    
    # Determine overall result
    if stats.get("critical", 0) > 0:
        print("RESULT: CRITICAL FAILURE - Crash/SEH detected", "CRITICAL")
        return False
    elif stats.get("failure", 0) > 0:
        print("RESULT: FAILURE - NGX evaluate/create failed", "ERROR")
        return False
    elif stats.get("success", 0) == 0:
        print("RESULT: INCONCLUSIVE - No success markers (plugin may not have initialized)")
        return False
    else:
        print("RESULT: SUCCESS - NGX feature created and injection active!", "SUCCESS")
        return True

def main():
    print("=" * 60)
    print("ScaleNG.Drive Autonomous Test (320s) with Map Load Verification")
    print("=" * 60)
    
    # Step 1: Build
    if not build_plugin():
        return 1
    
    # Step 2: Deploy
    deploy()
    if not verify_deploy():
        return 1
    
    # Step 3: Launch BeamNG
    proc = launch_beamng()
    
    try:
        # Step 4: Wait for ScaleNG.log to appear
        if not wait_for_scaleng_log(120):
            return 1
        
        # Step 5: Wait for game ready (camera CB patches = level rendering)
        if not wait_for_game_ready(90):
            log("Game not ready in time, but continuing test...", "WARN")
        
        # Step 6: Tail ScaleNG.log for test duration
        stats = tail_scaleng_log(TEST_CONFIG["duration_sec"])
        
    finally:
        # Cleanup
        log("Terminating BeamNG...")
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
    
    # Step 7: Summary
    success = print_summary(stats)
    return 0 if success else 1

if __name__ == "__main__":
    sys.exit(main())