# Switch 2 titles in Nightshade (experimental): port plan and progress log

Branch `feature/nx2`, started from `origin/main` at v1.010.003 (cb8af5a).

Switch 2 support comes from Takoroka's **eden-nx2**
(<https://git.eden-emu.dev/Takoroka/eden-nx2>, branch `eden-nx2`, GPL-3.0-or-later like Eden),
a fork of Eden that boots Switch 2 titles on PC. Its findings and knobs are in that tree's
`NX2_NOTES.md` (149 numbered rows at the pinned commit) and `NX2_GPU_THREAD_SPLIT.md`.
Nightshade builds on Eden (Eden Emulator Project), ProsperoEden (Swordpdf) and the PS5 work of
BlackBearReloaded; that attribution stays as it is.

No game files, keys or firmware are downloaded, committed or bundled. Testers use their own dumps.

## Goal

Run Switch 2 titles at least as far as, and more playably than, ProsperoEden's PS5 demo (Pokopia
to its intro at 28 FPS, CPU-bound, GPU 16 %, 8.3 of 12.1 GB used), without regressing Switch 1.

## Decision: move the Eden pin to eden-nx2's head

| | Move the pin to eden-nx2 `50fa86bbbc` | Backport nx2's diff onto `67bada77` |
|---|---|---|
| Upstream distance | `67bada77` is an ancestor of nx2's base `26a03d2473`: 28 Eden master commits, then 172 nx2 commits | the 172 nx2 commits rewritten onto a tree 28 commits older |
| Conflicts | Nightshade's 11 patches: 10 apply as they are, 1 has a trivial include conflict | nx2 builds on several of the 28 (multithreading refactor #4254, buffer-cache and NCE readback changes, the reverts): each needs hand merging inside nx2's 16 k changed lines |
| Tested anywhere | the tree nx2 builds and runs on PC (Windows MSVC, Linux GCC) | a tree nobody has built |
| Future updates | follow nx2's head (and Eden master through it) the same way | every nx2 update is another manual backport |

Chosen: **move the pin**. The source is the archive git.eden-emu.dev makes of the commit (no
GitHub mirror of eden-nx2 exists; `eden-emulator/mirror` has only Eden master), checked by sha256 in
`tools/deps.json` and `UPSTREAM.json` like the old one.

Risk of that source: Forgejo builds archives on request and caches them; a server-side change of
git or of its archive cache could give a different gzip for the same tree. If the sha256 ever stops
matching, the fix is to compare the unpacked tree with `git archive` of the commit and re-pin the
hash, or to mirror the commit to a GitHub repository.

## What the move changes

### Eden patches (`tools/eden-patches`)

Rebased with git onto `50fa86bbbc` (each patch a commit on `67bada77`, then `git rebase --onto`).

| Patch | Result |
|---|---|
| 01 fast file reads | applies (context only) |
| 02 cache storage title id | applies |
| 03 local memory bounds | applies |
| 04 CPU reads and reactive flushing | applies (nx2 changed the unmapped-read lambda next to it) |
| 05 post-processing relative | applies |
| 06 ATOM.CAS | applies |
| 07 coherent atomics | applies |
| 08 ATOMS.CAS | applies |
| 09 LDG cache ops | applies |
| 10 coherent storage | applies |
| 11 GPU-written draw args | include conflict in `macro.cpp` (nx2 added `<array>`, `<cstdlib>`, `<unordered_set>`): merged |

None is dropped: upstream fixed none of them in the 28 commits, and nx2's own changes are in other
places (the Ampere lowering feeds Eden's Maxwell translator, where 03 and 06-10 work).

### Derivations

`tools/prepare-vulkan-port.py` and `headless/CMakeLists.txt` (with its `.cmake` includes) copy
Eden sources and edit them at anchors. Checked two ways against the new tree: every anchor
(the Python script run on the patched tree; a configure-only harness that runs the CMake files in
script mode, PS5 and Linux host branches, and reports every `FATAL_ERROR` instead of the first), and
every derived file's additions compared with what the same derivation added to the old tree, so an
unchecked `string(REPLACE)` that silently stopped matching shows up too.

Adapted to eden-nx2's code (same effect as before):

| Where | What changed upstream | Now |
|---|---|---|
| `vk_rasterizer.cpp` `FlushWork` | nx2: dispatch every 32 draws (`CHECK_MASK` 31) | the PS5 block replaces it as before (512, dev-settings interval) |
| `vk_rasterizer.cpp` `PrepareDraw` | nx2 profiling zones, skipped-draw reports | the six timing marks go between nx2's zones |
| `vk_rasterizer.cpp` `DispatchCompute` | nx2 throws for native (Switch 2) compute without a pipeline | the dispatch probe follows nx2's throw |
| `vk_graphics_pipeline.cpp` `FillImageViews` | nx2 profiling zone around it | mark after the zone |
| `vulkan_memory_allocator.cpp` `CreateImage` | nx2 retries outside the budget itself; huge images become unbacked sparse images | its retry is counted and reported like the buffers' (`outside_budget`, pool report on failure) |
| `control/scheduler.cpp` `Push` | nx2 binds the channel outside the lock (or defers it to the split thread) | lock timer covers lookup and bind |
| `physical_core.cpp` `Idle` | nx2: `std::atomic<bool>` flag, optional fixed poll `EDEN_NX2_IDLE_SPIN_US` | Nightshade's adaptive poll replaces nx2's (its notes found no gain from the fixed one); atomic member calls instead of `__atomic_*` builtins; `tools/check-guest-idle.py` models the atomic |
| `k_spin_lock.cpp` | nx2: own spin-then-wait lock (atomic word), 128 pauses | kept, spinning `kernel_spin_iterations` (the PS5-measured count, dev-settings `spins=`) |
| `vfs_real.cpp` open files | nx2: `MaxOpenFiles()` function for Windows | 512 elsewhere, as before |
| `memory.cpp` unmapped reads | nx2 samples the read log itself | Nightshade's sampling (`unmapped_log.h`) replaces it |
| `loader.cpp`, `registered_cache.cpp` | upstream #4552: Eden reads block NCZs, names `.nsz`/`.xcz` as types, rejects solid NCZs | names stay NSP/XCI, `.xcz` updates go to `ProcessXCI` as before, solid NCZs are not rejected (Nightshade reads them, `headless/ncz.h`); Eden's block reader stays behind ours |
| `cpu_features.cpp` TSC | nx2 measures the TSC when the frequency is 0 | one fallback, Nightshade's rule (below 1 GHz) |
| `gpu_thread.cpp` shutdown | nx2 stops the split GPU thread after the worker | the PS5 worker's `Stop()` does the same |

Dropped, because upstream now does it:

- `texture_cache/util.h` `<boost/version.hpp>` include: upstream #4386 removed `FixSmallVectorADL`
  (the heap copy it worked around is gone; two `.inc` files call `FullDownloadCopies` directly).
- `GetPseudoDeviceId` without control data: nx2 (row 18) seeds it from the program id.

Added for nx2 on the PS5:

- `gpu_split.cpp` (PS5 graphics build): an exception on the split's second thread is recorded as a
  GPU failure (the session ends with its message, as on the worker) instead of ending the app,
  and `Stop()` resets the split, because the app runs one session after another.

## Switch 2 titles on the PS5

- **Format.** nx2 boots a Switch 2 title as a loose ExeFS (`exefs/main`, `main.npdm`, `rtld`,
  `sdk`, `subsdk*`) with its RomFS in a sibling `romfs/` folder; it has no Switch 2 NSP/XCI
  support. Nightshade lists such a folder in the games folder as a game.
- **Detection.** A Switch 2 program has the Switch 2 ACID layout in `main.npdm` (nx2's
  `ProgramMetadata::IsSwitch2`); its program id starts with `04`.
- **Defaults nx2 already applies to Switch 2 programs**: 12 GiB memory layout, auto-stubbed
  unknown services, buffer downloads off, the split GPU thread (`EDEN_NX2_SPLIT=2`), Switch 2
  collector sizes.
- **PS5 defaults on top** (see risks): the Turing macro JIT off (the console refuses executable
  memory outside the JIT's aliased mapping, as for Eden's Maxwell macro JIT), transcode threads
  sized for the console.
- **Keys and firmware**: as for Switch 1, the user's own `prod.keys` and firmware. A loose ExeFS
  is already decrypted, so it needs no title keys.

## PS5 risks

- **Memory.** The 12 GiB layout plus nx2's 8 GiB for the graphics server's GPU-only regions is
  20 GiB of guest DRAM. The PS5's pool is ~12.4 GiB for everything. It only works because guest
  RAM is backed where the guest writes (`src/guest_memory_service.inc`), and only while a title
  touches less than what is left after the GPU caches, the JIT and the host heap. Title 1 (Mario
  Kart World) asks gpu 214 for two 5.7 GiB regions.
- **CPU.** Switch 2 titles run 6 guest cores (nx2 folds 4/5 onto 0/1 by default), the split GPU
  thread needs two host threads, and the console's Zen 2 at 3.5 GHz is slower than the PCs nx2 was
  measured on. Expect CPU-bound frame rates, like the demo.
- **Executable memory.** nx2's Turing MME JIT allocates with plain Xbyak (mprotect RX), which the
  console refuses: off on the PS5 (interpreter with the call memo).
- **RADV features.** Shaders are SM86 SASS lowered to Maxwell and compiled by Eden's SPIR-V
  backend, so the features are the ones Switch 1 games use. New: sparse residency images for
  "unbacked" huge textures (2^28+ texels), the `bc_encoder.comp` compute pass (ASTC to BC1/BC3),
  64 MiB VMA blocks. Task/mesh programs are rejected by nx2 itself.
- **Driver budget.** nx2 allocates images past the Vulkan budget when the budget refuses; on the
  console the budget is not the pool's real state, which Nightshade already handles by asking again
  outside it.

## Progress log

- 2026-10-29: worktree `checkouts/nightshade-nx2` on `feature/nx2`; eden-nx2 cloned to
  `checkouts/eden-nx2`; archive sha256 `54c7b3c5fc6c5eb7fe0c212d7e10f8b8b206f6f0eff3fb758d0922574a295b62`
  (12,028,938 bytes, same on two downloads). Patches rebased (table above). Derivation anchors
  listed (above).
- 2026-10-29: pin moved (`tools/deps.json`, `UPSTREAM.json`, `prepare-build.sh`,
  `build-headless-host.sh`; the unpacked tree is `.deps/eden-<commit>/`, and the checks that read
  it look there). Every derivation applies (table above); local checks that do not need a Linux
  build pass on the new tree: guest idle, unmapped log, GC scan, loader message, system font, NCZ,
  TSC fallback, and the other source checks.
