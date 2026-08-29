# ScaleNG.Drive — genuine DLSS/DLAA integration plan

Updated 2026-08-27

## Goal

Produce visible DLSS or DLAA output in BeamNG.drive while preserving a normal, resizable, stable game window. A run counts as successful only when the user sees a repeatable image change attributable to the plugin and the log proves that helper evaluation and the game-frame handoff occurred in the same frame sequence.

## Current evidence

| Area | Proven result | Consequence |
|---|---|---|
| Plugin loading | ScaleNG.asi loads and reaches the D3D12/Present hooks. | Loader/deployment is not the primary blocker. |
| BeamNG device/queue | The game device and a real direct graphics queue are captured. | We have an ordering anchor, but queue capture alone is not an output path. |
| Present | A stable Present stream and active backbuffer are observed. | Present is a reliable observation point, not automatically a safe write point. |
| Helper NGX | The separate helper initializes NVIDIA DLSS and returns successful evaluations for thousands of frames with completed fences. | NGX initialization, evaluation, and cross-process synchronization work. |
| Deferred protocol | Helper acknowledgements arm deferred output with valid fence values. | The output becomes ready; it is not yet consumed by BeamNG. |
| Native render graph | Current command-list/resource discovery reports no native scene target, RTV binding, or copy source. | The present blocker is visibility of BeamNG's actual render graph. |
| Direct Present replacement | Black window followed by an access violation after the first acknowledged frame. | Do not use as the default integration mechanism. |
| Game-queue swapchain copy | Correct queue, warmup, and fence guards still led to device removal/crash on the first copy. | Queue correctness does not make the swapchain backbuffer safe. |
| Stability | Observation-only runs remain stable for minutes. | Keep observation and mutation strictly separated. |
| Resize | Historical output work raced resize and froze/crashed; observation-only runs are safer. | Resize is a later acceptance test, not discovery. |
| Memory | Helper commit grows substantially during long sessions. | Important debt, but not the immediate integration gate. |

## Actual blocker

The blocker is not DLSS initialization. It initializes and evaluates. The blocker is that the helper output has no proven, safe insertion point in BeamNG's native presentation chain.

The bridge contains a valid game-frame copy and a separate DLSS output resource, but those resources are not automatically visible to BeamNG. Directly copying the result into the swapchain backbuffer has already been disproven by crash/device-removal evidence.

## Rules for every build

1. One build has one hypothesis.
2. Observation builds must not write to game resources, alter command lists, or enable `queueCopy`/`replaceOutput`.
3. Mutation builds are disabled by default and enabled only after the preceding observation gate passes.
4. Every mutation has a one-shot limit, pointer/descriptor/fence logging, and a circuit breaker on device removal, black-window behavior, or crash-adjacent failure.
5. Width and height alone never qualify a resource. Ownership, format, ordering, and lifetime must also be proven.
6. Visible DLSS is not claimed from helper success alone; it requires a user-visible change plus matching handoff evidence.

## Phases and exit criteria

### Phase 0 — preserve the baseline

Keep the deployed baseline at:

```ini
replaceOutput=0
deferredOutput=1
queueCopy=0
```

Record clean launch, menu/map reachability, Present cadence, helper evaluations, and shutdown. This is the rollback reference.

### Phase 1 — make native render observation work

Determine why `Hook_CopyTextureRegion`, `Hook_OMSetRenderTargets`, and resource-creation tracking do not see BeamNG's native work:

1. Verify whether command-list vtable hooks attach to every real game command list or only a temporary/internal list.
2. Instrument command-list acquisition and vtable identity without calling hot-path methods or changing commands.
3. If vtable patching is incompatible with BeamNG wrappers, use a device-level creation/capture route for read-only observation.
4. Maintain a bounded candidate table: pointer, descriptor, device identity, creation/first-seen serial, last-seen serial, and whether it was observed as RTV, SRV, copy source, or copy destination.

Exit: one stable, non-swapchain, full-resolution native resource is observed in a repeated chain immediately before Present. If not, change observation rather than attempting output mutation.

### Phase 2 — dry-run frame ordering

For the selected candidate, log pointer/descriptor, ECL serial when written or bound, Present serial, helper input/output values, completed fence, and lifetime across the handoff window.

Exit: candidate remains stable for at least 300 frames with no descriptor faults and a deterministic relationship between native rendering, helper completion, and Present. The current run proves freshness rejection, but not yet a coherent same-batch color handoff.

### Phase 3 — native-compatible output target

Do not start with the swapchain backbuffer. Determine whether BeamNG has a presentation intermediate or a renderer-owned target/view path. Validate device/queue ownership, dimensions, format/state, flags, samples, transitions, and lifetime across a normal frame and controlled resize.

Exit: the target is demonstrably part of BeamNG's presentation chain and accepts a no-op/identity test without corruption.

### Phase 4 — one reversible visible experiment

Implement exactly one evidence-backed option: native handoff-copy substitution, presentation-intermediate replacement, or renderer-supported output/view redirection. Run it once after warmup, log all identities/fences, and automatically disarm after the first attempt.

Success requires no crash/device removal, no black window, continued Present, a repeatable user-visible image change/artifact, and log proof that the helper output was the changed-pixel source.

### Phase 5 — hardening

Only after visible output works: handle resize/reinitialization, address helper memory growth, reduce logging, test menu/map/gameplay/alt-tab/minimize/resize/shutdown, and package the stable default.

## Immediate next action

Phase 2 continuation: record native copy, OM, barrier, helper, and Present events as one coherent per-batch unit keyed by a narrow ECL/Present window. Preserve the safe read-only configuration; do not enable `replaceOutput` or `queueCopy` and do not attempt another backbuffer copy. Judge the next build by whether it identifies a plausible color resource with matching ordering and freshness, not by artifact production.

## Stop conditions

Roll back if BeamNG crashes, reports device removal, becomes black, stops presenting, repeatedly faults on resource descriptors, changes candidate identity without a resize/reinitialization event, produces invalid helper ACKs/fences, or if the only proposed target is the swapchain backbuffer.
