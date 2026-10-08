# Archived proposals and investigations

These files are retained for engineering history. They contain hypotheses,
implementation ideas, and historical “current status” language that can be
stale or contradicted by later tests. They are not current setup instructions.
For current facts and next steps, read [STATUS.md](../STATUS.md). For currently
supported operations, read [the test guide](../../scripts/README.md) and
[install guide](../INSTALL.md).

## Phase archives (Post-DLSS initialization reorganization)

- [Pre-DLSS initialization](pre-dlss-initialization/README.md) — everything
  before the first verified NGX init + feature (run 20261007T165257Z):
  phase index + init-failure elimination ledger.
- [Post-DLSS initialization](post-dlss-initialization/README.md) — eval,
  real-input, handoff, and fault records since the boundary run.

| Document | Historical purpose | Reading caveat |
|---|---|---|
| [Architecture rewrite plan](ARCHITECTURE_REWRITE_PLAN.md) | Proposed architecture phases and source mapping | DXGI/ReShade approach is not validated for deployment; ASI render/Present test is the confirmed live path. |
| [DLSS integration plan](DLSS_INTEGRATION_PLAN.md) | Proposed DLSS pipeline and gates | Claims about proxy/injection need fresh evidence; no current DLSS marker was recorded. |
| [ReShade integration lessons](RESHADE_INTEGRATION_LESSONS.md) | ReShade comparison and porting ideas | Analysis/hypotheses, not proof ScaleNG's proxy works. |
| [PLANS.md](PLANS.md) | Chronological attempts and rejected experiments | Status at the top reflects an older 2026-08 session. |
| [NEMOTRON_PROMPT.md](NEMOTRON_PROMPT.md) | Original implementation prompt | Historical specification, not a status or acceptance report. |
