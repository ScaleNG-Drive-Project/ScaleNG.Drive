# ScaleNG.Drive — PLANS.md

> Living document. Every attempted approach gets an entry: what it was, why it
> seemed like a good idea, and exactly why it didn't work (or worked partially).
> Goal: never re-attempt a failed approach without remembering why it failed.

---

## THE GOAL

DLAA in BeamNG.drive DX12: render native, evaluate NVIDIA DLAA on the frame,
inject the result into the backbuffer, show a HUD, zero crashes.

## CURRENT STATE (2026-08-22 ~02:00)

- **Hooks: all working.** Device/queue/cmdlist/swapchain hooks live; camera CB
  jitter patching works; scene/MV/depth discovery works; NGX core loads;
  feature creation works.
- **HUD: renders** (variant A-full PSO + descriptor-table root signature).
- **DLAA eval: succeeded once** (frame 9, fix14 era) — proves the whole NGX
  chain can work in-process.
- **BLOCKER: crashes during/after map load whenever present-time activity is
  ON.** Passive control run (fix22): 3+ minutes, zero faults, game stable.
  Active code is definitively the trigger.

## SUSPECT SHORTLIST (next session's bisect order)

1. **HUD draw path** — draws every present into backbuffer via RTV from
   g_hudRtvHeap slot 0; HudDrawText uses 61 KB stack array; HudEnsureRtv
   recreates RTV per new bb. Test: HUD-only run (dlaa=0, F9-equivalent on).
2. **NGX evaluate path** — bridge flow ran once successfully then faults at
   pre-evaluate inside nvwgf2umx+0x453870 (driver!) reading 0x2C. Suspects:
   shared-texture state tracking (COMMON vs explicit), MV format mismatch
   (R16G16F copy of engine MV that may be R32F-ish), or bridge fence timing.
3. **Graveyard/fence interplay** — flushed post-fence-wait now, but verify no
   residual race with engine queue submissions between wait and flush.

---

## FAILED / PARTIAL APPROACHES (append-only log)

### A1. vtable scan of dxgi.dll for swapchain tables (session 17)
What: scan .rdata for pointer runs, patch slot 8/22 to our stubs.
Why it failed: matched non-vtable pointer runs (57 patched → instant
STATUS_STACK_BUFFER_OVERRUN); real swapchain table was runtime-written by the
game anyway. **Never scan-and-patch vtables blindly again.**

### A2. MinHook fn-hooks on scanned candidates (session 17)
What: MH_CreateHook each candidate's Present fn with per-candidate stubs.
Why it failed: deterministic crash exe+0xD57A53 within ~10 s — hooked wrong
functions (string builders etc.). Scan-only build stable → fn-hooks guilty.

### A3. GetDesc() on tracked depth/MV resources as liveness probe (fix7/8)
What: SEH-guarded GetDesc to detect freed resources before NGX eval.
Why it failed: calling methods on freed COM objects is UB even under SEH;
faulted constantly during map loads; correlated with game-side null-deref
crashes. **Replaced with discovery stamps + scene-refresh invalidation.**

### A4. Inline fence Signal+Wait in AdoptDisplaySize (fix11/12)
What: drain-wait before releasing old dlssOut on size change.
Why it failed: raced InjectAtPresent's own fence usage (two incrementers, one
event) → premature SetEventOnCompletion → released textures mid-GPU-flight →
heap corruption → random `call rax` faults (RIP=RAX constant). **Fence value +
event must have exactly one owner. Fixed in fix17 graveyard design.**

### A5. DestroyFeature → pShutdown() on size change (pre-fix16)
What: full NGX core shutdown whenever display size changed.
Why it failed: nuked snippet JIT passes; next EvaluateFeature jumped into
freed JIT page (RIP=RAX constant, anon RX) → crash. **Release feature handle
only (NVSDK_NGX_D3D12_ReleaseFeature); core + JIT stay alive.**

### A6. Present-time injection on our own queue (all builds ≤ fix17)
What: at Present hook, submit DLAA eval list on g_graphicsQueue (our queue),
referencing game-owned textures.
Why it failed: two queues touching same resources unsynchronized = GPU races;
crash "soon after loading" regardless of internal fixes. **Injection must run
on the GAME'S OWN QUEUE immediately after its lists (fix18 architecture).**

### A7. NGX on the game's wrapped device (fix2–fix14 era)
What: initialize NGX with m_device captured at D3D12CreateDevice hook.
Why it failed: device is a custom wrapper class — QI(IDXGIDevice) =
E_NOINTERFACE on EVERY instance; NGX needs DXGI interop; CreateFeature gave
PlatformError (padding bug fixed later), then driver crashed inside evaluate
(nvwgf2umx+0x453870 read 0x2C). Wrapper cannot host NGX. Period.
**Fix: cross-device bridge (fix19+) — NGX on our clean device.**

### A8. Root SRV descriptor for the HUD atlas texture (fix≤15)
What: root signature exposed atlas Texture2D via in-root SRV descriptor.
Why it failed: D3D12 only allows raw/structured BUFFERS as root descriptors;
Texture2D requires descriptor table. E_INVALIDARG storm misread for weeks.
Debug layer named it instantly once enabled. **Use descriptor tables for
textures; enable Agility debug layer EARLY next time (app-local
d3d12SDKLayers.dll + exports, no OS component needed).**

### A9. Hardcoded feature level 0x1100 for bridge device (fix19)
What: EnsureBridge created clean device with FL 0x1100.
Why it failed: invalid constant (FL11_0 = 0xb000) → silent create failure →
'bridge unavailable'. One-character class of bug. **Use named enums, never raw
hex, for API constants.**

### A10. String-surgery edits via PowerShell Replace on large C++ (recurring)
What: patch big files via [System.IO.File]::ReadAllText/Replace/WriteAllText.
Why it failed: backtick/escape mangling (`r`n literals in strings), misplaced
insertions (g_injStep at DoInjection), duplicate declarations, mangled guard
blocks requiring full rewrites. **For multi-line C++ edits use the Edit tool
with exact anchors; reserve script-surgery for single-token swaps.**

### A11. Trusting "no module matched" from parse_dump.ps1 (fix14–19 forensics)
What: concluded fault address was 'non-module' (JIT/trampoline) based on
parser output.
Why it failed: parser script had args/encoding bugs AND the UAL crash-log
module attribution actually named nvwgf2umx.dll (+0x453870) once read directly.
The fault was IN THE DRIVER all along. **Verify tooling output against a
second source before building theories on it.**

---

## ACTIVE PLAN (bisect the active-path crash)

Phase B1 — HUD-only stability run:
  passive=0, dlaa=0 (new ini gate or F10-off before load), HUD stays on.
  Outcome A: stable → HUD exonerated; DLAA path is the offender → Phase B2.
  Outcome B: crashes → HUD is the offender → rewrite HUD submission
  (own cmdlist on game queue, heap-slot rotation, smaller stack arrays).

Phase B2 — DLAA-only stability run:
  passive=0, dlaa=1, HUD suppressed via ini key hud=0.
  Outcome A: stable across 2 map loads → DLAA works; combined-run crash was
  interaction → re-enable both and stress.
  Outcome B: crashes → isolate inside eval: try (a) skipping our input-copy
  barriers, (b) R16G16F→R32F mv probe, (c) eval every-N-frames to see if
  frequency-dependent.

Phase B3 — if both individually stable but combined unstable:
  serialize harder: HUD draw moves into the SAME our-device pass as DLSS
  (single submission), or alternate frames HUD/DLAA.

## STANDING FIXES TO CARRY FORWARD

- Graveyard release pattern (no inline fence ops outside one owner).
- Feature Release-not-Shutdown on size change.
- Gameplay gate (camera-CB stamp) gating ALL present-time activity.
- Debug layer app-local technique for any future D3D12 mystery.
- nvngx.log telemetry (__NGX_LOG_LEVEL=3 setx'd user-wide).
