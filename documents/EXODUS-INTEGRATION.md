# EXODUS shader and Vulkan integration

Reference: `C:/Users/rodri/Dev1/Projetos/GR2fork-GR2fork-EXODUS`.

The reference compiler and Vulkan pipeline flow is integrated with the existing
UWP, native D3D12 and Dozen contracts. Extended IR/GCN coverage, numeric fixes,
native shader lowerings, Dozen capability fallbacks and resource lifetime fixes
remain in place. Title-specific FSR/DLSS hooks are outside this integration.

## Compiler and pipeline flow

- Six persistent compile workers emit SPIR-V and create graphics pipelines.
  Translation to IR remains on the GPU command thread, as in the reference.
- Tasks own copied shader information, user data, runtime information, fetch
  shader metadata and vertex descriptors. SPIR-V emission uses the captured
  fetch inputs.
- Pending shader permutations and graphics pipelines are published when ready.
  The per-draw wait deadline covers shader emission and pipeline creation.
- Shader IR pools remain alive through emission and are recycled afterward.
  The reference pool cache retains up to 16 released pools.
- Graphics compilation tasks own private driver caches. Their results are merged
  into the persistent cache with external synchronization. Inline graphics and
  compute creation also synchronize access to that cache.
- Cleanup precedes promise completion. Unpublished shader modules have an owner;
  queued work drains before cache shutdown or shader replacement. Partial worker
  startup failure joins the workers already created.

## Vulkan and shader capabilities

The reference BDA capability gate is applied to constant reads, shader metadata,
VMA allocator flags and buffer usage flags. Current UWP/Dozen device setup keeps
BDA disabled. The native D3D12 profile retains its existing emulated DMA path.
Shader-model requirements are unchanged.

The reference combined scissor is used for Vulkan viewports and attachment-free
render areas. The attachment-free render cache validates that scissor and reports
its invalidations. The reference RenderDoc API-version fallback is included.

The Dozen loader, raw-image lowering, persistent PSO library and existing Vulkan
capability fallbacks remain intact. `src/uwp` and `src/core/uwp` are unchanged.
`ACQUIRE_MEM` and `PFP_SYNC_ME` still have no shared-processor `CpSync` calls.

## Configuration and persistent caches

`GPU.async_shader_recompiling` defaults to `true`, is exposed through the existing
settings catalog, and supports global and per-game settings. Setting it to
`false` uses inline shader/pipeline compilation.

`GPU.pipeline_sync_budget_ms` retains its 0-50000 ms range and 50 ms default.
With async compilation enabled, draws whose dependencies are not ready within
that budget are skipped. Subsequent draws retry pipeline lookup, following the
reference.

Shader binary and metadata cache versions are now 23 and 16. Existing shader and
pipeline persistence is retained; incompatible shader records are regenerated.

## Validation

- Release x64 core and UWP frontend built successfully; frontend: zero errors,
  35 existing warnings. Only changed C++ regions were formatted.
- Pac-Man/Dozen with async compilation: image inspected, clean shutdown. A warm
  run preloaded 12 pipelines. Logs confirm work on the compiler threads.
- Pac-Man with async compilation disabled: image presentation and clean shutdown.
- Zero wait-budget startup stopped at 5 seconds with `RUN_RESULT=0` and
  `SHUTDOWN=0`; the new setting was present in the settings catalog.
- Final DLL: Bloodborne/Dozen reached the scenario, image inspected, 30 FPS in
  the 160-175 second window, `RUN_RESULT=0`, `SHUTDOWN=0`.
- Final DLL: native D3D12 initialized and presented frames during a 15-second
  startup, then shut down cleanly. Native visual/gameplay validation is separate.
- Preserved-tree hashes: 13,121 files unchanged; the native shader-cache adapter
  has the intended profile capability assignment. Dozen DLL SHA-256 remains
  `50012fc664440ede1a08da46a5f699c33e4837c411ef8a6b2687d2a025edc049`.
- Final core SHA-256:
  `9ac2fa74ef63f6b5f7e64797599d1cd968e09c283c48944ee533a7d0cfe123e0`.

These PC results do not establish an Xbox performance improvement. No generated
package was installed.

## First-death reload failure and device discovery

The supplied Xbox logs terminate in `GXWorker:1` with `Failed to create Win32
semaphore`, followed by `SessionFailure`. They also show earlier scratch mapping
failures (Win32 8), about 36,800 views and 9,200 live sections. The original
semaphore assertion did not record its Win32 error, so the exact native allocation
failure cannot be recovered from those logs. There is no shader/PSO failure at
that termination point.

Windows semaphore wrappers now reuse exclusive native handles. Binary semaphores
reserve 256 handles on first use; binary and two-permit wake semaphores each cache
at most 256 idle handles. Recycled permits are drained with a non-alertable zero
wait before reuse, and each lease restores its requested initial count. Large
counting semaphores retain direct creation/destruction. The reference's alertable
waits, deadline calculation and guest cancellation behavior remain intact.
Allocation/release/wait failures retain their original Win32 error and diagnostics
separate native creations, live/cached handles and reuse. This targets repeated
waiter allocations under pressure; it does not establish that the preceding
mapping/section pressure has been eliminated.

EXODUS device discovery is integrated through `net_discovery.cpp/.h`, the build,
network settings and emulator lifecycle. With both
`Network.connected_to_network` and `Network.enable_device_discovery` enabled,
the responder accepts `SRCH ` requests on UDP 987 and returns the reference's
DDP identity, protocol, firmware and running-title fields. Discovery defaults to
disabled, supports global/per-game settings and does not initiate broadcasts.
The UWP manifest already grants private-network client/server access.

The response is captured before the worker starts. Stop wakes and joins the
worker before releasing sockets; a new session can restart it with a new title,
and a failed bind does not permanently latch the responder as started. Native
D3D12, Dozen loading, memory budgets and shared-command-processor synchronization
are unchanged by this correction. Xbox first-death reload and companion-device
reachability require a fresh console run.

Validation of the reload/discovery correction:

- Release x64 core and UWP frontend built successfully (frontend: one packaging
  manifest warning, zero errors; no package installation).
- Final DLL: Pac-Man presented an inspected image and stopped cleanly after
  35 seconds; Bloodborne reached an inspected Hunter's Dream scene and stopped
  cleanly after 175 seconds (`RUN_RESULT=0`, `SHUTDOWN=0`).
- Bloodborne telemetry recorded more than 327,000 handle reuses with 378 native
  semaphores created and no creation/wait/release failures. This PC run did not
  exercise the reported Xbox first-death transition or reproduce its pressure.
- Loopback discovery returned the correct title identifiers for Pac-Man and
  Bloodborne. A malformed search received no response; UDP 987 was no longer
  bound after shutdown.
- Dozen binary and protected UWP/native source hashes retain their prior state.
- Correction core SHA-256:
  `4cf13741034b7edfe7390f9e2547dc3ca7659e8d1e730624c6010e7cb1d0d57e`.

## Records and rollback

Current source backups, the prior DLL, changes, hashes, build logs and runtime
results are under `Build/exodus-shader-vulkan`. The earlier baseline restoration
records remain under `Build/exodus-restore`. The reload/discovery correction
backups and validation records are under `Build/death-reload-ddp`.
