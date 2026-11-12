# Guest memory backend

The original integration derives from `KytyPS5-xbox-on-upstream`; the adaptive
pager and LZ4 read cache use the November 9, 2026 reference at
`C:/Users/rodri/Dev1/Projetos/KytyPS5-xbox-on-upstream-2026-11-09-f399d481-memory-and-fixes`.
The upstream license remains in `COPYING`. Source hashes and pre-change backups
for this update are in `Build/kyty-nov09-memory`.

`memory_backend.cpp` adapts lazy regions to shadPS4 physical backing and preserves
alias coherence, page protections, temporary pin generations, deferred discards,
and mapping leases. Native D3D12 and Vulkan consumers retain their existing
lifetime contracts. Resident views are not converted to private copies during
execution. `memory_api.cpp` provides the guarded C interface.

## Swap and compression

The file remains the authoritative copy of every swapped 64 KiB window.
`General.memory_swap_compression` enables the LZ4 cache at the next game start;
it defaults to false and is also available in per-game settings. The cache is
owned by the guest session, uses 4 KiB granules in 4 MiB chunks, and retains at
most 64 MiB of payload. Metadata and fixed compression scratch space are
additional allocations (about 17 MiB). Windows compressed to more than 75% of
their size are omitted. The oldest cached copies are discarded when full.
A failed decompression or checksum falls back to the file; a failed file checksum
is reported as corruption. Disabling the cache does not disable disk swap.

## Pager policy

The updated policy samples cold windows before pressure, keeps a standby queue,
uses the upstream adaptive idle time (8-32 seconds normally, 1 second over
budget), and protects windows returning within 5 seconds. The protection starts
at 15 seconds and grows to 240 seconds, while emergency reclaim may override it
after considering other candidates. Checks for pins, inaccessible/protected pages
and writes during eviction are retained. There are no new GPU waits or scheduler
finish calls. Page-in mappings still use shadPS4's shared physical sections.

On Xbox, swap is mandatory and the process controller ceiling is 4800 MiB. Its
normal target is 4672 MiB, leaving 128 MiB for bursts. The guest resident budget
subtracts non-guest app allocations. New guest commits and page-ins are denied
when their sampled process charge would exceed the ceiling, allowing existing
bounded fault recovery to reclaim eligible windows. Unavailable/pinned memory
cannot be discarded to satisfy the ceiling.

This is an admission policy for guest backing, not an OS-enforced cap on the
entire application: the driver, DLLs and other threads can allocate independently.
Those allocations can briefly or permanently exceed 4800 MiB; the pager cannot
guarantee otherwise. `SHADPS4_UWP_APP_LIMIT_MB` may lower the controller ceiling
on PC for validation (512-4800 MiB), but cannot raise the Xbox ceiling. It does
not impose a Windows working-set or job-object limit. All values use MiB.

The FPS overlay continues showing app usage/OS app limit and logical occupied
swap/file size. The compressed RAM cache is included in app RAM usage; it does
not change the on-disk swap format or make in-flight GPU resources pageable.

## Validation

Release x64 core and UWP frontend compiled successfully. With the final DLL,
Pac-Man ran for 65 seconds with a 128 MiB guest budget on PC in both cache modes
and shut down with `RUN=0`. Both runs paged guest windows and reported zero pager
errors/corruption/privatization. The cache-enabled run recorded 30,477 cache hits,
34 misses and zero bad copies; 45.2 MiB of granules held 150.9 MiB of raw windows.
The cache-disabled run had no cache samples. These short runs validate operation,
not a performance improvement. The process controller was configured to 4800 MiB
for PC validation, but these Pac-Man runs did not reach its admission ceiling.

Bloodborne gameplay validation was inconclusive: the updated DLL encountered a
Dozen readback fault before swap use was recorded; the previous DLL stopped at a
different shader assertion. Neither establishes that the pager caused the failure.
Xbox pressure and first-death transitions remain unverified. No package was
installed. Detailed logs and rollback material are in `Build/kyty-nov09-memory`.

## Updated upstream memory fixes

The current `C:/Users/rodri/Dev1/Projetos/KytyPS5-xbox-on-upstream` snapshot
adds backed-memory run traversal and fixes protection restoration. Its LZ4 cache,
idle-time rules and lazy-region policy match the November reference already
integrated here. `Build/kyty-updated-memory/source-manifest.csv` records hashes
of the inspected source because this source directory has no Git metadata.

`MemoryManager::CopySparseMemory` now groups physically contiguous backing pages
and contiguous holes. Each backed run is copied under the existing GPU/mapping
access scope, and holes remain zero-filled. The UWP run goes through the existing
coherent physical-copy adapter, retaining its paging and alias checks. The
upstream lock-free table is not copied into this different table/lifetime model;
shadPS4's existing mapping locks and page table remain authoritative.

Temporary host copies restore the current recorded protection of the owner page
rather than an old native protection snapshot. The backend memory lock remains
held while opening, copying and restoring. Homogeneous page protection first uses
one native call; invalid-address/invalid-parameter failures fall back to 4 KiB
subranges, avoiding protection calls spanning separate native allocations. Other
native errors retain the failure diagnostic.

No new GPU waits, RAM budgets, compression defaults or private-memory conversion
are introduced by this update. The 4800 MiB Xbox controller and compression toggle
remain as described above. Backups and build/runtime logs are under
`Build/kyty-updated-memory`.

Validation of the updated snapshot: Release core and UWP frontend built with zero
errors. Pac-Man completed 65-second PC runs with compression enabled and disabled,
using a 128 MiB guest budget to exercise swap, and shut down with `RUN=0` in both.
The runs reported zero pager errors, corruption, or private-memory conversions.
The compressed run served 280 cache hits with zero bad copies. This does not
establish Xbox performance, protection-failure recovery under console exhaustion,
or Bloodborne gameplay correctness. No package was installed.
