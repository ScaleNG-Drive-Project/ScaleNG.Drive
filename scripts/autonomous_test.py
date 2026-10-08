#!/usr/bin/env python3
"""Build, launch, and verify ScaleNG in a fresh BeamNG.drive process.

Requires Windows, Visual Studio C++ build tools, BeamNG.drive, and the pinned
test environment from scripts/setup_test_env.bat. Results/logs are saved under
logs/test_runs/. See scripts/README.md; don't install an arbitrary BeamNGpy.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME = Path(r"C:\Games\BeamNG.drive")
BIN64 = GAME / "Bin64"
PLUGINS = BIN64 / "plugins"
GAME_EXE = BIN64 / "BeamNG.drive.x64.exe"
GAME_LOG = PLUGINS / "ScaleNG.log"
REQUIRED = ("ScaleNG.asi", "ScaleNG_NGX_helper.exe")
FATAL_MARKERS = (
    "FATAL: SEH exception", "access violation", "DEVICE_REMOVED",
    "GetDeviceRemovedReason", "EvaluateFeature FAULTED",
)
USER32 = ctypes.windll.user32 if sys.platform == "win32" else None


def send_hotkey(vk_key: int) -> None:
    """Simulate a single key press+release via the global keyboard state.

    The plugin uses GetAsyncKeyState, which reads the system-wide async state,
    so no window focus manipulation is needed. F9/edge detection requires a
    full down-then-up transition.
    """
    if USER32 is None:
        return
    # keybd_event(vk, scan, flags, extraInfo); 0 = key down, 2 = key up.
    USER32.keybd_event(vk_key, 0, 0, 0)
    time.sleep(0.05)
    USER32.keybd_event(vk_key, 0, 2, 0)


def dismiss_known_library_warning(process_id: int) -> bool:
    """Choose Cancel only on BeamNG's known third-party-library dialog."""
    if USER32 is None:
        return False
    hwnd = ctypes.c_void_p()
    while True:
        hwnd = USER32.FindWindowW(None, "Third-party library warning")
        if not hwnd:
            return False
        pid = ctypes.c_ulong()
        USER32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value != process_id:
            return False
        # BM_CLICK is sent only to the Cancel button nested under this exact
        # BeamNG-owned dialog. Cancel permanently suppresses repeat warnings.
        cancel = USER32.FindWindowExW(hwnd, None, "Button", "Cancel")
        if not cancel:
            return False
        USER32.SendMessageW(cancel, 0x00F5, 0, 0)
        time.sleep(1)
        return True


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def port_is_free(port: int) -> bool:
    with socket.socket() as sock:
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duration", type=int, default=75, help="gameplay observation time in seconds (default: 75)")
    parser.add_argument("--level", default="smallgrid")
    parser.add_argument("--require-dlss", action="store_true", help="require a DLSS eval marker (legacy injection or shadow-eval ok; default only verifies D3D12 frames)")
    parser.add_argument("--port", type=int, default=25252, help="BeamNGpy TCom port")
    parser.add_argument("--startup-timeout", type=int, default=180)
    parser.add_argument("--skip-build", action="store_true", help="use existing dist artifacts")
    parser.add_argument("--no-deploy", action="store_true", help="test currently deployed plugin without copying files")
    parser.add_argument("--real-test", action="store_true",
                        help="after gameplay ready, toggle F9 to REAL engine MV/depth inputs "
                             "(F8 remains ON from INI shadowHandoff=1). Verifies mvTouchAge/depthTouchAge<=3.")
    parser.add_argument("--extra-settle", type=int, default=10,
                        help="seconds to settle after F9 toggle before recording (default: 10)")
    args = parser.parse_args()

    if args.duration < 1:
        parser.error("--duration must be positive")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = ROOT / "logs" / "test_runs" / stamp
    run_dir.mkdir(parents=True, exist_ok=False)
    result = {"started_utc": stamp, "level": args.level, "duration_sec": args.duration,
              "checks": {}, "errors": [], "run_dir": str(run_dir),
              "real_test": args.real_test}
    game = None
    process = None
    baseline_signature = None
    baseline_size = 0
    live_lines: list[str] = []
    game_output_file = None

    def check(name: str, passed: bool, detail: str = "") -> None:
        result["checks"][name] = {"passed": bool(passed), "detail": detail}
        print(f"{'PASS' if passed else 'FAIL'} {name}{': ' + detail if detail else ''}", flush=True)

    try:
        try:
            import beamngpy
            from beamngpy.connection.connection import Connection
            from beamngpy import BeamNGpy
        except ImportError as exc:
            raise RuntimeError("beamngpy is required; install with: py -m pip install beamngpy") from exc
        result["beamngpy_version"] = getattr(beamngpy, "__version__", "unknown")
        check("beamngpy_available", True, result["beamngpy_version"])
        if Connection.PROTOCOL_VERSION != "v1.26":
            raise RuntimeError(
                f"BeamNG {GAME} requires BeamNGpy protocol v1.26; installed package uses "
                f"{Connection.PROTOCOL_VERSION}. Run scripts\\setup_test_env.bat "
                "to install the package version matched to this game's TCom protocol."
            )

        if not GAME_EXE.is_file():
            raise RuntimeError(f"BeamNG executable not found: {GAME_EXE}")
        check("game_install_found", True, str(GAME_EXE))
        tasklist = subprocess.run(["tasklist.exe", "/FI", "IMAGENAME eq BeamNG.drive.x64.exe", "/NH"],
                                  capture_output=True, text=True, timeout=10)
        if "BeamNG.drive.x64.exe" in tasklist.stdout:
            raise RuntimeError("BeamNG is already running; close it so this run only controls its own process")
        if not port_is_free(args.port):
            raise RuntimeError(f"TCom port {args.port} is occupied; choose another with --port")

        dist = ROOT / "dist"
        if not args.skip_build:
            build = subprocess.run(["cmd.exe", "/d", "/c", str(ROOT / "src" / "build_asi.bat")],
                                   cwd=ROOT / "src", capture_output=True, text=True, timeout=300)
            (run_dir / "build.stdout.txt").write_text(build.stdout or "", encoding="utf-8")
            (run_dir / "build.stderr.txt").write_text(build.stderr or "", encoding="utf-8")
            check("build", build.returncode == 0, f"exit={build.returncode}; see build.stdout.txt")
            if build.returncode:
                raise RuntimeError("ASI build failed")
        else:
            check("build", True, "skipped; using existing artifacts")

        for filename in REQUIRED:
            if not (dist / filename).is_file():
                raise RuntimeError(f"Required build artifact missing: {dist / filename}")
        check("build_artifacts", True, ", ".join(REQUIRED))

        if not args.no_deploy:
            backup_dir = run_dir / "deployment_backup"
            backup_dir.mkdir()
            for filename in REQUIRED:
                old = PLUGINS / filename
                if old.exists():
                    shutil.copy2(old, backup_dir / filename)
            for filename in REQUIRED:
                shutil.copy2(dist / filename, PLUGINS / filename)
        else:
            backup_dir = None
        deployed = all((PLUGINS / n).is_file() and sha256(PLUGINS / n) == sha256(dist / n) for n in REQUIRED)
        check("deployment", deployed, "artifacts match dist" if deployed else "missing or hash mismatch")
        if not deployed:
            raise RuntimeError("Deployment verification failed")
        if backup_dir:
            result["deployment_backup"] = str(backup_dir)

        if not args.no_deploy:
            config = PLUGINS / "ScaleNG.ini"
            if not config.is_file():
                shutil.copy2(dist / "ScaleNG.ini", config)
                result["deployment_backup_config"] = "not present before this run"
        baseline_size = GAME_LOG.stat().st_size if GAME_LOG.exists() else 0
        baseline_log = GAME_LOG.read_bytes() if GAME_LOG.exists() else b""
        baseline_signature = hashlib.sha256(baseline_log).hexdigest()
        (run_dir / "plugin_log_before.txt").write_bytes(baseline_log)

        game = BeamNGpy("127.0.0.1", args.port, home=str(GAME),
                        binary="Bin64/BeamNG.drive.x64.exe", quit_on_close=False,
                        )
        level_arg = args.level.rsplit("/", 1)[0].split("/")[-1] if "/" in args.level else args.level
        launch_args = ["-tcom-listen-ip", "127.0.0.1", "-level", level_arg,
                       "-vehicle", "pickup"]
        game_output_file = (run_dir / "game.stdout.log").open("wb")
        command = game._prepare_call(str(GAME_EXE), None, *launch_args)
        print(f"Launching BeamNG once: {' '.join(command)}", flush=True)
        process = subprocess.Popen(command, cwd=BIN64, stdin=subprocess.PIPE,
                                   stdout=game_output_file, stderr=subprocess.STDOUT)
        game.process = process
        pid = getattr(process, "pid", None)
        result["game_pid"] = pid

        started = time.monotonic()
        connected = False
        while time.monotonic() - started < args.startup_timeout:
            if process.poll() is not None:
                raise RuntimeError(f"BeamNG exited during startup (code {process.returncode})")
            dismiss_known_library_warning(pid)
            try:
                game.connection = Connection(game.host, game.port)
                game.connection._process = process
                if game.connection.connect_to_beamng(tries=1, log_tries=False):
                    connected = True
                    break
            except Exception:
                pass
            time.sleep(1)
        check("tcom_connected", connected, f"single BeamNG PID {pid} on port {args.port}")
        if not connected:
            raise RuntimeError("BeamNG did not expose TCom; see game.stdout.log")
        game._load_system_info()

        gameplay_ready = False
        while time.monotonic() - started < args.startup_timeout:
            if process is not None and process.poll() is not None:
                raise RuntimeError(f"BeamNG exited during startup (code {process.returncode})")
            if process is not None:
                dismiss_known_library_warning(process.pid)
            try:
                state = game.control.get_gamestate()
                if state.get("state") == "freeroam":
                    vehicles = game.vehicles.get_current()
                    if vehicles:
                        gameplay_ready = True
                        result["game_state"] = state
                        result["vehicles"] = list(vehicles.keys())
                        break
            except Exception:
                pass
            time.sleep(2)
        check("gameplay_ready", gameplay_ready, f"state={result.get('game_state')}; vehicles={result.get('vehicles')}")
        if not gameplay_ready:
            raise RuntimeError("Game did not reach freeroam with a vehicle before timeout")

        if args.real_test:
            # Toggle F9 to switch from zero placeholders to engine REAL MV/depth.
            # F8 (shadowHandoff) stays ON per INI shadowHandoff=1 / realInputs=0 default.
            send_hotkey(0x78)  # VK_F9
            result["real_test_toggle_sent"] = True
            if args.extra_settle:
                print(f"REAL test: F9 toggled; settling {args.extra_settle}s for engine to feed REAL inputs...", flush=True)
                time.sleep(args.extra_settle)

        observed_start = time.monotonic()
        log_offset = baseline_size
        while time.monotonic() - observed_start < args.duration:
            if process is not None and process.poll() is not None:
                result["errors"].append(f"BeamNG exited during test (code {process.returncode})")
                break
            try:
                if GAME_LOG.exists():
                    size = GAME_LOG.stat().st_size
                    if size < log_offset:
                        log_offset = 0
                    with GAME_LOG.open("rb") as stream:
                        stream.seek(log_offset)
                        new = stream.read()
                        log_offset = stream.tell()
                    live_lines.extend(new.decode("utf-8", errors="replace").splitlines())
            except OSError as exc:
                result["errors"].append(f"Could not read plugin log: {exc}")
            time.sleep(1)

        fresh_log = GAME_LOG.read_bytes() if GAME_LOG.exists() else b""
        if hashlib.sha256(fresh_log).hexdigest() != baseline_signature:
            new_bytes = fresh_log[baseline_size:] if len(fresh_log) >= baseline_size else fresh_log
        else:
            new_bytes = b""
        if not new_bytes and pid:
            current_lines = fresh_log.decode("utf-8", errors="replace").splitlines()
            process_lines = [line for line in current_lines if f"[p{pid}]" in line]
            new_bytes = ("\n".join(process_lines) + "\n").encode("utf-8")
        (run_dir / "plugin_log_new.txt").write_bytes(new_bytes)
        live_lines = (run_dir / "plugin_log_new.txt").read_text(encoding="utf-8", errors="replace").splitlines()
        loaded = any("ScaleNG.asi loaded" in line and (not pid or f"[p{pid}]" in line or f"pid={pid}" in line)
                     for line in live_lines)
        initialized = any("ScaleNG.asi initialization complete" in line for line in live_lines)
        frames = [line for line in live_lines if "hooks: frame " in line and " started " in line]
        render_evidence = [line for line in live_lines if any(marker in line for marker in (
            "hooks: D3D12CreateDevice called #", "hooks: GAME direct queue captured=",
            "hooks: real queue ECL hook INSTALLED", "hooks: PRESENT fn-level hook INSTALLED",
            "hooks: swapchain ", "hooks: GAME CreateCommandList #",
        ))]
        present_progress = [line for line in live_lines if "topo-state: snapshot present=" in line]
        present_values = []
        for line in present_progress:
            match = re.search(r"topo-state: snapshot present=(\d+)", line)
            if match:
                present_values.append(int(match.group(1)))
        render_evidence.extend(present_progress)
        current_pid_lines = [line for line in live_lines if f"[p{pid}]" in line] if pid else live_lines
        target_lines = current_pid_lines or live_lines
        frames = [line for line in target_lines if "hooks: frame " in line and " started " in line]
        injections = [line for line in target_lines if "hooks: DLSS injection recorded" in line]
        # Shadow-eval path (current): "hooks: shadow-eval ok #<n> (present <p> handoff <0|1>)".
        # ok  = NGX feature evaluated successfully on a presented frame.
        # handoff=1 = that evaluation's output was copied into the backbuffer
        # being presented (the visible-handoff proof). The bit is optional so
        # older logs still parse as evidence of evaluation.
        shadow_ok = []
        shadow_handoff = []
        shadow_failures = [line for line in target_lines if any(marker in line for marker in (
            "shadow-eval FAILED", "shadow-eval reset failed", "shadow-eval allocator/list reset failed"))]
        for line in target_lines:
            match = re.search(r"hooks: shadow-eval ok #(\d+) \(present (\d+) handoff (\d)\)", line)
            if match:
                shadow_ok.append(line)
                if match.group(3) == "1":
                    shadow_handoff.append(line)
            elif "hooks: shadow-eval ok #" in line:
                shadow_ok.append(line)
        # Evidence-only: maximum ok counter #N seen. The ok lines are sampled
        # (first 10, then every 600th), so len(shadow_ok) undercounts total
        # evals on long runs; the counter is the true total.
        shadow_eval_max_ok = 0
        for line in shadow_ok:
            ok_num = re.search(r"hooks: shadow-eval ok #(\d+)", line)
            if ok_num:
                try:
                    value = int(ok_num.group(1))
                except ValueError:
                    continue
                if value > shadow_eval_max_ok:
                    shadow_eval_max_ok = value
        # Evidence-only: input-mode breakdown from "shadow-eval inputs"
        # lines. Scoped to target_lines (current PID), same as shadow_ok.
        # Defensive: match only "inputs (REAL|ZERO)" and "why=([A-Za-z-]+)";
        # the remainder of inputs lines is free-form and must not be parsed.
        shadow_input_real_lines = 0
        shadow_input_zero_lines = 0
        shadow_input_real_why: list[str] = []
        shadow_input_zero_why: list[str] = []
        # Touch-age tracking for REAL freshness proof (present-serial same-frame).
        # mvTouchAge/depthTouchAge <= 3 on sustained REAL inputs lines is the
        # falsifiable expectation (see STATUS.md). 9999 = never observed.
        max_mv_touch_age_real = 0
        max_depth_touch_age_real = 0
        real_touch_age_violations = 0
        last_input_mode = "unknown"
        for line in target_lines:
            mode_match = re.search(r"inputs (REAL|ZERO) why=", line)
            if not mode_match:
                continue
            why_match = re.search(r"why=([A-Za-z-]+)", line)
            why = why_match.group(1) if why_match else None
            mv_age_match = re.search(r"mvTouchAge=(\d+)", line)
            depth_age_match = re.search(r"depthTouchAge=(\d+)", line)
            mv_touch = int(mv_age_match.group(1)) if mv_age_match else 9999
            depth_touch = int(depth_age_match.group(1)) if depth_age_match else 9999
            if mode_match.group(1) == "REAL":
                shadow_input_real_lines += 1
                if why and why not in shadow_input_real_why and len(shadow_input_real_why) < 8:
                    shadow_input_real_why.append(why)
                last_input_mode = "REAL"
                if mv_touch > max_mv_touch_age_real:
                    max_mv_touch_age_real = mv_touch
                if depth_touch > max_depth_touch_age_real:
                    max_depth_touch_age_real = depth_touch
                if mv_touch > 3 or depth_touch > 3:
                    real_touch_age_violations += 1
            else:
                shadow_input_zero_lines += 1
                if why and why not in shadow_input_zero_why and len(shadow_input_zero_why) < 8:
                    shadow_input_zero_why.append(why)
                last_input_mode = "ZERO"
        shadow_input_total = shadow_input_real_lines + shadow_input_zero_lines
        if shadow_input_total == 0 and shadow_ok:
            shadow_input_note = "inputs_lines: 0 (modes unknown)"
        else:
            shadow_input_note = f"inputs_lines: {shadow_input_total}"
        eval_failures = [line for line in target_lines if "DLSS evaluate failed" in line or "EvaluateFeature failed" in line]
        fatal = [line for line in target_lines if any(marker.lower() in line.lower() for marker in FATAL_MARKERS)]
        check("plugin_loaded_this_run", loaded, f"BeamNG PID {pid}; based on fresh log bytes")
        check("plugin_initialized", initialized, "fresh initialization-complete marker")
        render_pass = bool(frames or render_evidence) and len(present_values) >= 2 and present_values[-1] > present_values[0]
        check("d3d12_render_frames", render_pass,
              f"{len(frames)} frame markers; {len(render_evidence)} current-process D3D12/Present hook signals; "
              f"Present counter {present_values[0] if present_values else 0}->{present_values[-1] if present_values else 0} "
              f"across {len(present_values)} snapshots")
        if injections:
            check("dlss_frames_evaluated", True,
                  f"{len(injections)} legacy injection markers (retired path)")
        elif shadow_ok:
            desc = f"{len(shadow_ok)} shadow-eval ok markers"
            if shadow_handoff:
                desc += f", {len(shadow_handoff)} with visible handoff"
            check("dlss_frames_evaluated", True, desc + " (not proof of visual quality)")
        else:
            check("dlss_frames_evaluated", False, "no DLSS eval evidence (not proof of visual quality)")
        check("no_fatal_markers", not fatal, f"{len(fatal)} fatal markers")

        if args.real_test:
            # REAL mode freshness proof checks
            check("real_inputs_observed", shadow_input_real_lines > 0,
                  f"{shadow_input_real_lines} REAL input lines (expect >0 after F9 toggle)")
            check("no_reset_failures",
                  len(shadow_failures) == 0 and len(eval_failures) == 0,
                  f"{len(shadow_failures)} shadow reset failures, {len(eval_failures)} eval failures")
            check("no_breaker_lines",
                  not any("breaker" in line.lower() for line in target_lines),
                  "no breaker state lines in current PID log")
            check("real_touch_ages_in_window",
                  max_mv_touch_age_real <= 3 and max_depth_touch_age_real <= 3,
                  f"max mvTouchAge={max_mv_touch_age_real}, max depthTouchAge={max_depth_touch_age_real} "
                  f"(expect <=3 for double/triple buffering); {real_touch_age_violations} violations")
            # Inconclusive depth note (no DSV hook — depth relies on barrier/SRV traffic only)
            if max_depth_touch_age_real > 3:
                result["depth_inconclusive"] = (
                    "depthTouchAge >3: no DSV write hook exists; depth freshness "
                    "relies solely on barrier traffic + SRV creation. Visual A/B "
                    "on depth is INCONCLUSIVE until per-frame depth write is instrumented."
                )
        result["counts"] = {"frame_markers": len(frames), "render_evidence": len(render_evidence),
                             "present_snapshots": len(present_progress), "injection_markers": len(injections),
                             "shadow_eval_ok": len(shadow_ok), "shadow_eval_handoff": len(shadow_handoff),
                             "shadow_eval_failures": len(shadow_failures),
                             "evaluate_failures": len(eval_failures), "fatal_markers": len(fatal),
                             "shadow_eval_max_ok": shadow_eval_max_ok,
                             "shadow_input_real_lines": shadow_input_real_lines,
                             "shadow_input_zero_lines": shadow_input_zero_lines,
                             "shadow_input_real_why": list(shadow_input_real_why),
                             "shadow_input_zero_why": list(shadow_input_zero_why),
                             "last_input_mode": last_input_mode,
                             "shadow_input_note": shadow_input_note,
                             "max_mv_touch_age_real": max_mv_touch_age_real,
                             "max_depth_touch_age_real": max_depth_touch_age_real,
                             "real_touch_age_violations": real_touch_age_violations}
        result["evaluate_failure_samples"] = eval_failures[:20]
        result["fatal_samples"] = fatal[:20]
        base_pass = loaded and initialized and render_pass and not fatal
        dlss_evidenced = bool(injections or shadow_ok)
        if not base_pass:
            result["outcome"] = "FAIL"
        elif args.require_dlss and not dlss_evidenced:
            result["outcome"] = "INCONCLUSIVE_DLSS"
        elif dlss_evidenced:
            result["outcome"] = "PASS_DLSS_EVAL"
        else:
            result["outcome"] = "PASS"
        # PASS_DLSS_EVAL means the run passed AND DLSS evaluation was observed;
        # it is strictly stronger than bare PASS, so it must not read as FAIL.
        check("overall", result["outcome"] != "FAIL", result["outcome"])
    except Exception as exc:
        result["outcome"] = "FAIL"
        result["errors"].append(str(exc))
        print(f"ERROR {exc}", file=sys.stderr, flush=True)
    finally:
        process_exit_code = process.poll() if process is not None else None
        if game is not None:
            try:
                game.close()
            except Exception as exc:
                result["errors"].append(f"Game cleanup: {exc}")
        if process is not None and process_exit_code is None and process.poll() is None:
            try:
                process.terminate()
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
        if process is not None:
            code = process_exit_code if process_exit_code is not None else process.poll()
            result["game_exit_code"] = code
            if code == 0xC0000005:
                try:
                    fresh_log = GAME_LOG.read_bytes() if GAME_LOG.exists() else b""
                    changed = hashlib.sha256(fresh_log).hexdigest() != baseline_signature
                    new_bytes = (fresh_log[baseline_size:] if len(fresh_log) >= baseline_size else fresh_log) if changed else b""
                    (run_dir / "plugin_log_new.txt").write_bytes(new_bytes)
                    lines = new_bytes.decode("utf-8", errors="replace").splitlines()
                    loaded = any("ScaleNG.asi loaded" in line and f"[p{process.pid}]" in line for line in lines)
                    initialized = any("ScaleNG.asi initialization complete" in line for line in lines)
                    result["checks"]["plugin_loaded_this_run"] = {"passed": loaded, "detail": f"BeamNG PID {process.pid}"}
                    result["checks"]["plugin_initialized"] = {"passed": initialized, "detail": "fresh plugin log"}
                    if loaded and initialized:
                        result["outcome"] = "GAME_CRASHED_AFTER_PLUGIN_INIT"
                except Exception:
                    pass
        if game_output_file is not None:
            game_output_file.close()
        result["finished_utc"] = datetime.now(timezone.utc).isoformat()
        (run_dir / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"Result: {result.get('outcome', 'FAIL')} — {run_dir / 'result.json'}", flush=True)
    return 0 if result.get("outcome") in ("PASS", "PASS_DLSS_EVAL", "PASS_DLSS_INJECTION", "INCONCLUSIVE_DLSS") else 1


if __name__ == "__main__":
    raise SystemExit(main())
