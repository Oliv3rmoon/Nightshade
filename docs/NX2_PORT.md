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
| 12 Switch 2 only optimizations (new) | eden-nx2 drops same-value 3D register writes and defers the cache invalidation of inline uploads for every title; measured only with Switch 2 titles, both now wait for a Switch 2 driver (`Tegra::g_nx2_ampere`), so Switch 1 games keep Eden's behaviour |

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
| `vk_pipeline_cache.cpp` `CurrentGraphicsPipelineSlowPath` | nx2 reports each new pipeline key (`ReportNewPipelineKey`, `EDEN_NX2_PIPELINE_DIFF`) before building it | v1.010.006's take from the shader cache's load (1.012.001) goes after the report and before the GPU thread's own build |

Dropped, because upstream now does it:

- `texture_cache/util.h` `<boost/version.hpp>` include: upstream #4386 removed `FixSmallVectorADL`
  (the heap copy it worked around is gone; two `.inc` files call `FullDownloadCopies` directly).
- `GetPseudoDeviceId` without control data: nx2 (row 18) seeds it from the program id.

Added for nx2 on the PS5:

- `gpu_split.cpp` (PS5 graphics build): an exception on the split's second thread is recorded as a
  GPU failure (the session ends with its message, as on the worker) instead of ending the app;
  `Stop()` resets the split, because the app runs one session after another; `EDEN_NX2_SPLIT` is
  read once per session (a game's own setting), not once per run.
- `textures/workers.cpp`: Eden's transcode thread count (half the hardware threads) instead of
  eden-nx2's quarter, which was measured on a PC with Switch 2 titles; Switch 1 games decode ASTC
  on these threads on the console.
- `physical_core.cpp`: a static assertion that every emulated core has its own idle poll.

Checks adapted to eden-nx2 (they read the derived files): `check-guest-idle.py` (atomic flag),
`check-tsc-fallback.py` (one fallback), `check-native-gpu-thread.py` (two push sites, the split's
stop), `check-gpu-sync-stop.py` (DrainBack), `check-gc-scan.py`, `check-loader-message.py`,
`check-streamed-game.py`, `check-system-font-mod.py` (the unpacked tree's new name).

## Switch 2 titles on the PS5

What the host does for them (docs/SETTINGS.md has the player's side):

- **Format.** eden-nx2 boots a Switch 2 title as a loose ExeFS (`exefs/main`, `main.npdm`, `rtld`,
  `sdk`, `subsdk*`) with its RomFS in a sibling `romfs/` folder; it has no Switch 2 NSP/XCI
  support. In Nightshade such a game is a game folder in `roms/` holding `exefs/` (with `main` and
  `main.npdm`) and `romfs/`, and goes by the path of its main, `<folder>/exefs/main`
  (`headless/game_files.h`; `ValidRomPath` accepts it, so recent games, the last game and
  forwarders keep it). The Library lists it (`eden_services.cpp` `GameFiles`, `CountGames`), names
  it by its folder or by a `control.nacp` beside `exefs/`, and sizes it by its folder. The
  update/DLC scan skips such folders and every `exefs/`/`romfs/` folder (thousands of files).
  A Switch 1 game dumped the same way plays too (format "ExeFS").
- **Title ID and detection.** The program ID in `main.npdm` (`eden_game_title_id`); a Switch 2
  title is one whose npdm has the Switch 2 ACID layout (`eden_game_is_switch2`, eden-nx2's
  `ProgramMetadata::IsSwitch2`). Settings per game, saves and mods go by that ID as for any game.
- **Keys and firmware**: as for Switch 1, the user's own `prod.keys` and firmware (the launcher asks
  for them before any game). A loose ExeFS is already decrypted, so it needs no title keys.
- **Launcher.** The Library's details say **Switch 2: experimental** (a warning chip) and "Expect
  crashes, missing graphics and low frame rates." (a warning line); the format reads "Switch 2".
  Both texts are in all 30 catalogs.
- **eden-nx2's own defaults, applied by eden-nx2 itself to Switch 2 programs**: 12 GiB memory layout
  plus 8 GiB for the graphics server, auto-stubbed unknown service commands, buffer downloads off,
  Switch 2 collector sizes, six emulated cores.
- **eden-nx2's run settings, applied by `main.cpp` for a Switch 2 session** (eden-cli reads them from
  its config file on a PC): GPU accuracy Low, reactive flushing off, ASTC decoded on the GPU
  (`bc_encoder.comp` keeps it as BC1/BC3 when textures are compressed), the split GPU thread, and the
  12 GiB layout named in the launch report. `"switch2"` in the settings file (global and per game)
  changes `gpu_split`, `exact_gpu` and `reactive_flushing` (`settings_store.h` `Switch2Settings`).
- **PS5 choices on top**: the Turing macro JIT off (`EDEN_NX2_MME_JIT=0` at start, see risks); the
  transcode thread count stays Eden's (half the hardware threads) for Switch 1 games; per-core idle
  polling covers six cores (`performance.h kGuestCores`; a core without its own poll polled the
  longest every time); the split GPU thread's mode is read once per session, not once per run.
- **One process, many sessions.** eden-nx2 keeps Switch 2 GPU state process-wide (`g_nx2_driver`,
  `g_nx2_ampere`, `Nx2GpuMemoryRegistry`, semaphore regions and waiters). A PC runs one game per
  process; this app goes back to the launcher. `headless/nx2_session.h` puts that state back
  before every session, so a Switch 1 game after a Switch 2 one does not run as Ampere.
- **Switch 1 unchanged where eden-nx2 changed it for everyone** (patch 12): same-value register
  write dropping and deferred inline-upload invalidation, both measured only with Switch 2 titles,
  apply only once a Switch 2 driver is seen (`Tegra::g_nx2_ampere`). Kept for everyone (they are
  fixes or keep order): the fence manager's own lock and batched release, the device memory
  translation cache per thread, the spin-then-wait kernel lock (with the console's spin count).

## For a tester with their own Switch 2 game

Nobody here has a Switch 2 dump or keys; the first console runs are the testers'.

1. Dump your own game's ExeFS and RomFS (with your own keys, from your own cartridge or download)
   into a game folder: `roms/<name>/exefs/` with `main`, `main.npdm`, `rtld`, `sdk` and every
   `subsdk*`, and `roms/<name>/romfs/` with the whole RomFS. Splatoon Raiders (`0400380020056000`)
   is the title eden-nx2 gets furthest in on a PC (menus, then its intro); Pokopia
   (`0400A3A01EE4C000`) is the one ProsperoEden's demo showed.
2. Start Nightshade: the Library lists the folder with "Switch 2" under its name; its details say
   **Switch 2: experimental**. Leave Settings as they are (Vulkan renderer).
3. Play it once without changing anything, as far as it goes, then quit with Touchpad + L1.
4. Send `/data/nightshade/logs/` (`eden_log.txt`, `stderr.log`, the launch report and any crash report). What says
   the most: the line `Switch 2 title (experimental): ...` (what the session got), `GPU thread
   split`, `EDEN_GUEST_MEMORY ... bytes=21474836480` (the 20 GiB guest range; `sparse refused` means
   the console would not reserve it), `Draw skipped`, `Native compute shader rejected`, `Using auto
   stub fallback`, and the memory lines of the FPS overlay.
5. If it stops or crashes on the way, try once more with `"switch2": {"gpu_split": false}` in the
   settings file (docs/SETTINGS.md), and say which run was which.

## PS5 risks

- **Memory.** The 12 GiB layout plus nx2's 8 GiB for the graphics server's GPU-only regions is
  20 GiB of guest DRAM. The PS5's pool is ~12.4 GiB for everything. It only works because guest
  RAM is backed where the guest writes (`src/guest_memory_service.inc`), and only while a title
  touches less than what is left after the GPU caches, the JIT and the host heap. Title 1 (Mario
  Kart World) asks gpu 214 for two 5.7 GiB regions.
- **Address space.** The 20 GiB guest range is reserved and mapped to the zero block slot by slot
  (10,240 mappings of 2 MiB, against 4,096 at 8 GiB); a kernel limit on map entries would refuse
  it, and the log then says `EDEN_GUEST_MEMORY sparse refused`.
- **CPU.** eden-nx2 emulates six cores for every title (Switch 1 titles leave 4 and 5 idle; their
  threads sleep), the split GPU thread needs two host threads (its second thread is placed on the
  secondary CPUs, not pinned), and the console's Zen 2 at 3.5 GHz is slower than the PCs nx2 was
  measured on. Expect CPU-bound frame rates, like the demo. `"switch2": {"gpu_split": false}` trades
  the split for one free core.
- **Executable memory.** nx2's Turing MME JIT allocates with plain Xbyak (mprotect RX), which the
  console refuses: off on the PS5 (interpreter with the call memo). With it off the split GPU thread
  runs macros on its second thread (eden-nx2's mode 1), not on the first (mode 2, its measured best).
  Next step: give it Nightshade's JIT allocator (`headless/jit-allocator.h`, aliased RW/RX views)
  with commits as the code grows, then test it on a console.
- **RADV features.** Shaders are SM86 SASS lowered to Maxwell and compiled by Eden's SPIR-V
  backend, so the features are the ones Switch 1 games use. New: sparse residency images for
  "unbacked" huge textures (2^28+ texels), the `bc_encoder.comp` compute pass (ASTC to BC1/BC3),
  64 MiB VMA blocks. Task/mesh programs are rejected by nx2 itself.
- **Sparse images.** Images over 2^28 texels (Mario Kart World's virtual textures) become sparse
  images with nothing bound; that needs sparse residency in the console's RADV. Where it is missing
  the image fails and the session ends with the out-of-memory message.
- **Driver budget.** nx2 allocates images past the Vulkan budget when the budget refuses; on the
  console the budget is not the pool's real state, which Nightshade already handles by asking again
  outside it.
- **Video.** Switch 2 movies are decoded with FFmpeg's VP9 decoder, which the console's FFmpeg build
  has (H.264, VP8, VP9).
- **Fixed costs for every game.** The GPU's device address space grew from 16 to 64 GiB: Eden's
  page-cache counters for it are a dense 16 MiB array (4 MiB before). Its other tables are sparse.
- **Switch 1 behaviour.** Upstream's 28 commits and eden-nx2's changes for every title (six cores,
  the kernel lock, the fence manager's batched release, the per-thread device translation cache)
  reach Switch 1 games too. The two that change GPU state handling are kept to Switch 2 (patch 12);
  the rest is checked only by the existing checks, not on a console.

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
- 2026-10-30: first CI on the pin move (63e4561). Linux host suite (`make test`, launcher
  screens): **green**, <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40975918356>; the
  archive came from git.eden-emu.dev on the runner. PS5 build:
  <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40975862255> compiled and linked Eden
  (eden-nx2) and the app for the console with no compile error, then stopped at
  `check-native-gpu-thread.py`, which expected one dispatch site in the GPU worker (eden-nx2 has
  two); the check follows eden-nx2 since c1154bf.
- 2026-10-30: Switch 2 host support (79f2ba0), settings and path tests, the checks of the generated
  GPU worker (c1154bf: also `check-gpu-sync-stop.py`'s DrainBack, `check-load-failure.py`), the
  preview's Switch 2 sample (45bc4a5: an added sample moved the tour's game positions, so an
  existing one became the Switch 2 title). Locally (macOS, x86-64 syntax checks against the patched
  tree): the derived Vulkan files, `gpu_split.cpp`, `physical_core.cpp`, `k_spin_lock.cpp`,
  `gpu_thread.cpp`, `metadata_bridge.cpp`, `eden_services.cpp`, `library.cpp` and the host path of
  `main.cpp` compile.
- 2026-10-30: 45bc4a5 green on both. PS5 build (`gh workflow run 389325104 --ref feature/nx2`):
  <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40981539705>, success: the release ZIP
  (`Nightshade-v1.010.003.zip`, 77 entries) built and checked, with every check of the native
  build (generated GPU worker, GPU sync stop, TSC selector...). Linux host suite and launcher
  screens on test/nx2: <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40981562851>,
  success (`make test`: settings store, guest idle, kernel spin lock, NCZ, GC scan, streamed game,
  the 30 catalogs...). The preview tour now also shoots a Switch 2 title's details
  (`07b-library-switch2`).
- 2026-10-30: final code commit 9eab461 (the label in the Home tab's details too, after the new
  preview screen showed it only in Canopy's Library). PS5 build: **success**,
  <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40987967896>. Linux host suite and the
  three themes' screens on test/nx2 (c17314b pins 9eab461): **success**,
  <https://github.com/Oliv3rmoon/Nightshade/actions/runs/40987975328>. Nothing has run on a
  console: no Switch 2 dump or keys here.
- 2026-10-30: merged into `release/1.012.000` (v1.012.000, a test release) on top of v1.011.001:
  Nightshade's own title ID and data folder, v1.010.004's shader cache start and `EDEN_MEMORY`, and
  v1.010.005's TOTK fixes and amiibo. The GetSpan fix still applies (eden-nx2 keeps the 16-bit
  mapping number and the same `GetSpan`); amiibo needs nothing new (Eden's input engine and NFC
  service are unchanged but for headers). The memory layout is set once the mods are known
  (v1.010.005): a Switch 2 title gets the 12 GiB layout there, and the TOTK 4 GiB guard is asked
  about Switch 1 titles only (`check-performance-settings.py`).
- 2026-10-30: v1.010.006 (`release/1.011.002`) merged into `release/1.012.001` (v1.012.001). Its
  Eden edits are all in `prepare-vulkan-port.py` and `headless/CMakeLists.txt`; one anchor moved:
  the GPU thread's take from the shader cache's load in `CurrentGraphicsPipelineSlowPath`, where
  eden-nx2 reports each new key first (table above). eden-nx2's `LoadDiskResources`, the builders'
  `CreateGraphicsPipeline`/`CreateComputePipeline`, the pipelines' constructors, `LoadPipelines`
  and the ASTC recompression in `util.cpp` are Eden's as they were (eden-nx2 picks BC1 for opaque
  ASTC through the same `bpp_div` the layer loop uses). Checked as for the pin move: the merged
  script run on the patched eden-nx2 tree, and every derived file's additions compared with what
  v1.010.006 added on `67bada77`: the same in all 32 files. The build while the game plays stays on
  for Switch 2 titles: the builders touch none of the GPU thread's caches, the loader is locked,
  only the thread that owns the rasterizer (the split's back thread) takes from it, the builders
  read no Switch 2 process-wide state (cached native programs are already lowered), and the load
  ends in `~PipelineCache`, after `GpuSplit::Stop` and before `Nx2::ResetForSession`. Not measured:
  the builders share the secondary CPUs with `GPUBack` while the game plays.
