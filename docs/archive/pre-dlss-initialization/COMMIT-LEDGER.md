# Commit-message ledger — Pre-DLSS initialization era (HISTORICAL)

> **HISTORICAL — do not use as current status.**
> Phase label (exact): **Pre-DLSS initialization**.
> These seven `commit_msg*.txt` drafts (all dated 2026-10-02, all commits
> predate the `20261007T165257Z` init boundary) were consolidated here and
> the scratch files removed. Current status lives in `docs/STATUS.md`.

Each entry: draft subject → landed commit (hash, date) → what the commit
changed → rationale worth preserving. Message-claimed test numbers are
labeled as message claims (not re-verified); subjects and file lists were
verified against `git show --stat`.

## 1. `commit_msg.txt` → `6e710ad` (2026-10-02)

Subject: `feat: ReShade integration lessons - DXGI proxy + event-based
architecture for DLSS`.
Committed: `AUTONOMOUS_WORKFLOW.md`, `OMNI.md`,
`RESHADE_INTEGRATION_LESSONS.md`, `dist/` binaries,
`docs/{ARCHITECTURE_REWRITE_PLAN,CACHE,DLSS_INTEGRATION_PLAN}.md`,
`scripts/autonomous_test.py` (+326).
Preserved rationale: ReShade built and tested working in BeamNG as a
DXGI proxy (hooks `CreateDXGIFactory`/`CreateSwapChainForHwnd`); its
event system (`init_device`, `init_swapchain`, `init_resource`,
`copy_texture_region`, `copy_buffer_to_texture`, `init_effect_runtime`)
documents the discovery pattern later reused
(format/size-based depth/MV/color discovery via `init_resource`,
in-process NGX eval at Present, `register_overlay` ImGui overlay); the
`19-depth_motion_dump` example (`init_resource`,
`update_texture_region`, `copy_buffer_to_texture`,
`copy_texture_region`, `init_swapchain` → `depth_motion_log.txt`)
identifies depth/MV/color by format/size. ReShade provides hooks only
— no built-in DLSS. Cross-process helper, D3D12 vtable hooks, and UAL
were removed in this design; NGX stayed blocked by cross-device
rejection. Architectural record now in `docs/archive/` (superseded).
Message claims (unverified): 320s run, GridMap loads, ReShade+ScaleNG
coexist; `19-depth_motion_dump` example outputs.

## 2. `commit_msg2.txt` → `13c6ef9` (2026-10-02)

Subject: `feat: DXGI Proxy Architecture (ReShade-style) for ScaleNG.Drive`.
Committed: `src/dxgi_hooks.{h,cpp}`, `src/events.cpp`,
`src/resource_tracker.{h,cpp}`, `src/ngx_evaluator.{h,cpp}`,
`src/present_evaluator.{h,cpp}`, `src/main_dxgi.cpp`, `dist/dxgi.*`,
`src/build.bat`, `scripts/autonomous_test.py`, `scripts/launch_test.bat`
(this commit also stored the `commit_msg.txt` draft).
Preserved rationale: full refactor from vtable-hooks + cross-process
helper to DXGI proxy + in-process NGX (Windows loads the proxy
automatically, no UAL; single device shared by resources and NGX), with
a ReShade-mirroring event system (`init_device`, `init_swapchain`,
`init_resource`, `copy_texture_region`, `copy_buffer_to_texture`,
`init_effect_runtime`, `present`), creation-time resource tracker,
Present-time pipeline (BackBuffer → Color Input → NGX Evaluate →
Output → BackBuffer), and ImGui-ready overlay infrastructure.
Build verified per message (`dxgi.dll` ~2MB); next step at the time was
deploy + GridMap test. This architecture was later superseded by the
UAL ASI single-device path; see `docs/archive/` plans.

## 3. `commit_msg3.txt` → `64f98f2` (2026-10-02)

Subject: `fix: autonomous test - remove false positive C0000005 marker`.
Committed: `scripts/autonomous_test.py` only (31+/34-).
Preserved rationale: bare `C0000005` matched memory addresses in log
output, causing false critical failures; real crash indicators are
`FATAL: SEH exception` / `access violation`. Still true today:
`FATAL_MARKERS` (`scripts/autonomous_test.py:32-35`) contains exactly
those indicators plus `DEVICE_REMOVED`, `GetDeviceRemovedReason`, and
`EvaluateFeature FAULTED` — no bare `C0000005`.
Message claim (unverified): post-fix run 320s, 495 success markers,
0 failures/critical.

## 4. `commit_msg4.txt` → `b3bb7e4` (2026-10-02)

Subject: `config: test helper=0 for single-device mode`.
Committed: `dist/ScaleNG.ini` only (6+/8-).
Preserved rationale: cross-device blocker confirmed — NGX evaluates on
the helper's device while resources live on the game's device ("NGX
rejected the frame"); hence `helper=0` and NGX on the game's device
directly. Still true today: shipped `helper=0`, bridge creation
disabled, single-device design (`docs/IMPLEMENTATION.md`).

## 5. `commit_msg5.txt` → `4885f41` (2026-10-02)

Subject: `feat: Single-device NGX path working with ASI architecture`.
Committed: `dist/` binaries + INI, `dist/dxgi.*` (this commit also
stored the `commit_msg2/3/4/5.txt` drafts).
Preserved rationale: fixed DLAA forcing and config parsing in
`main.cpp` (`dlaa=0`, `helper=0` respected); ASI+UAL working;
single-device NGX path in `EnsureUpscalerInit`; ReShade-style DXGI
hooks integrated; dual build scripts (ASI + dxgi proxy).
Preserved caveat (still relevant): BeamNG v0.39 does not load levels
via `-level`/`-vehicle` CLI args; `scripts/BeamNG_LevelLoader.pmc`
(Pulover's Macro Creator template, still present, added in `2e6a1ee`)
was the workaround; the current runner instead drives TCom
(`scripts/autonomous_test.py`).
Message claim (unverified): autonomous test passing, 320s,
530 success markers.

## 6. `commit_msg6.txt` → `2e6a1ee` (2026-10-02)

Subject: `docs: add TOOL_REQUIREMENTS.md, PROJECT_LOG.md, and macro template`.
Committed: `PROJECT_LOG.md` (+244), `TOOL_REQUIREMENTS.md` (+250),
`scripts/BeamNG_LevelLoader.pmc` (+121). Never tracked as a txt file
(created untracked afterwards); content verified against the commit.
Preserved: tool inventory + project history + level-loader macro as the
test-infrastructure baseline both later docs build on.

## 7. `commit_msg7.txt` → `5e6110f` (2026-10-02)

Subject: `docs: update CACHE.md and PROJECT_LOG.md with test
infrastructure details`.
Committed: `PROJECT_LOG.md`, `docs/CACHE.md` (large rewrite, 511+/1867-).
Never tracked as a txt file; content verified against the commit.
Preserved: autonomous test flow/outcomes/commands documentation plus the
v0.39 level-loading issue note (see entry 5).

## Disposition of the scratch files

`commit_msg.txt` … `commit_msg5.txt` were tracked in HEAD (stored
alongside their code changes); `commit_msg6/7.txt` were never tracked.
All seven are redundant once this ledger exists: subjects, commit IDs,
file lists, rationale, and caveats above preserve every substantive
line; message-claimed test numbers are explicitly labeled unverified.
Removed in the same commit as this ledger (tracked ones via `git rm`).

Return: [Current status](../../STATUS.md) · [Archive index](../README.md)
