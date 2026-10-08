# ScaleNG.Drive documentation hub

Start at the repository [README](../README.md), then use [STATUS.md](STATUS.md)
for what is true now and what should happen next. This page is the index and
technical reference map; it is not the current-status ledger.

## Current operator guides

- [Current status and agenda](STATUS.md): latest verified live test and open work.
- [Implementation (current)](IMPLEMENTATION.md): what the shipped build
  actually does — init, feature, eval, inputs, handoff, guards.
- [Automated test](../scripts/README.md): create the pinned test environment,
  run BeamNG, and interpret reports.
- [Build/install/configure and manual verification](INSTALL.md): ASI
  deployment, INI table, live controls, uninstall.
- [Tool requirements](../TOOL_REQUIREMENTS.md): developer tool inventory.

## Development and validation rules

- Build the supported UAL plugin with `src\build_asi.bat`. `src\build.bat`
  builds the experimental DXGI proxy; it is not the ASI build command.
- Preserve the safe render path. Eval failure, feature failure, and the
  fault breaker (live but UNFIRED — 30-reset / 120-halt paths
  unexercised live) all present the engine's original frame, except the
  192627Z CreateFeature crash; never claim more than the logs prove
  (see STATUS.md for what `handoff 1`, `why=real`, and ok `#N`
  counters do and do not establish — `ok` lines are sampled, totals
  come from the counters).
- F9 is double-owned: the inert legacy HUD path logs `hud: overlay`
  only (no pixels), while the shadow path logs input switches. The
  authoritative line is `shadow-eval inputs … (F9)`.
- The repeatable automated test verifies live render/Present, plugin init,
  and (with `--require-dlss`) NGX evaluation — see scripts/README for what
  the outcome tokens mean. Image quality requires a controlled comparison.
- The old architecture drafts were retained under [archive/](archive/README.md).
  They include rejected experiments and hypotheses; they are not current
  deployment instructions.
- [CACHE.md](CACHE.md) is a chronological agent cache. It has legacy entries and
  is not a substitute for STATUS.md or the operator guides.

## Technical reference

The long-form implementation/capture reference is maintained in
[TECHNICAL_REFERENCE.md](TECHNICAL_REFERENCE.md). It covers the PIX capture,
render graph, camera constant buffer, root signature, design rationale, source
map, and historical verification details preserved from the former monolithic
README. For actionable setup and current verification limits, use
[INSTALL.md](INSTALL.md) instead.

## History and supporting records

Post-DLSS initialization (current): [STATUS.md](STATUS.md) +
[IMPLEMENTATION.md](IMPLEMENTATION.md). Pre-DLSS initialization
phases and rejected experiments live under
[archive/](archive/README.md) — provenance, not guidance.

- [PROJECT_LOG.md](../PROJECT_LOG.md): dated project log; current summary at its
  beginning, historical entries below.
- [CACHE.md](CACHE.md): agent-oriented experiment ledger; historical, with a
  current status pointer near its end.
- [archive/](archive/README.md): architectural proposals, integration plans,
  and lessons that contain stale or disproven assumptions.
- [User engineering notes](../User/README.md): original correctness checklist
  and completion log, retained for traceability rather than current status.
- [NEMOTRON_PROMPT.md](archive/NEMOTRON_PROMPT.md): original implementation
  prompt; historical source.

## Contents of the technical reference

- [Project intent](TECHNICAL_REFERENCE.md#1-why-this-project-exists)
- [PIX verification findings](TECHNICAL_REFERENCE.md#2-the-verification-work-what-was-found-and-why-it-matters)
- [Original injection design](TECHNICAL_REFERENCE.md#3-the-dlss-injection-design-accepted-middle-ground)
- [Tooling, implementation, and file map](TECHNICAL_REFERENCE.md#4-tooling--environment-how-the-work-gets-done)
- [Detailed historical design and verification notes](TECHNICAL_REFERENCE.md#7-design-notes)
- [Build/install and manual verification](INSTALL.md)
- [Original change history](TECHNICAL_REFERENCE.md#9-change-log)
