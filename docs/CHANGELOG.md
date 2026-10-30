# Changelog

What changed in each release of ProsperoEden, newest first. A release's notes on GitHub are its section here.

## v1.012.000

**Experimental test release: Switch 2 games, and a newer Eden under every game.** Nightshade now builds on Takoroka's [eden-nx2](https://git.eden-emu.dev/Takoroka/eden-nx2) (Eden master with experimental Switch 2 support, commit `50fa86bbbc`) instead of Eden `67bada77`, with everything in v1.011.001. No Switch 2 game has been run on a PS5 yet, and Switch 1 games have not been played on the newer Eden on a console either. This release is for testing: if v1.011.001 plays your games well, keep it for playing.

### Switch 2 games (experimental)

- **A Switch 2 game is a game folder with its ExeFS and RomFS.** Dump your own game into `roms/<the game's name>/exefs/` (`main`, `main.npdm`, `rtld`, `sdk` and every `subsdk*`) and `roms/<the game's name>/romfs/` (the whole RomFS). Switch 2 games as NSP or XCI files are not supported. The Library lists the folder as a game with **Switch 2** as its format, and its details say **Switch 2: experimental** and "Expect crashes, missing graphics and low frame rates." Keys and firmware are your own, as for Switch 1 games. See [Switch 2 titles](SETTINGS.md#switch-2-titles-experimental).
- **None has been run on a console.** On a PC, eden-nx2 takes some titles to their menus, intros or first races. The PS5 has less memory and a slower CPU than the PCs it was measured on.
- **A Switch 2 game starts with eden-nx2's own settings**, not those of **Settings > Performance**: the 12 GiB memory layout plus 8 GiB for its graphics server (the console backs only the memory the game writes), Eden's lowest GPU accuracy, no reactive flushing, ASTC textures decoded on the GPU, six emulated CPU cores and the split GPU thread. `"switch2"` in the settings file changes three of them (`gpu_split`, `exact_gpu`, `reactive_flushing`). The `Switch 2 title (experimental):` line in `stderr.log` says what the game got.
- **Tears of the Kingdom's 4 GiB (v1.010.005) is for its Switch 1 version only.** A Switch 2 game always gets the 12 GiB layout, whatever its title ID.

### The newer Eden, for Switch 1 games too

eden-nx2 brings 28 newer commits of Eden master and changes of its own, and some of them apply to every game:

- **Six emulated CPU cores instead of four.** Switch 1 games leave the two new ones idle; their threads sleep.
- **A new kernel lock**: it spins first and then waits, spinning as many times as was measured on the console.
- **The GPU's fences are released in batches**, under the fence manager's own lock.
- **About 12 MiB more fixed memory for every game**: the GPU's address space grew from 16 to 64 GiB, and Eden's page counters for it with it.

Two of eden-nx2's GPU changes that were measured only with Switch 2 games (dropping register writes of the same value, deferring the cache invalidation of inline uploads) apply to Switch 2 games only; Switch 1 games keep Eden's behaviour there. Nightshade's own fixes to Eden all carry over, and so do v1.010.005's: Tears of the Kingdom's 4 GiB, the 1 GiB held for the game's memory, the buffer reads in long sessions, and amiibo.

### What to send

- **Switch 2 games.** Play once with the settings as they are, as far as it goes, then quit with Touchpad + L1. Send `/data/nightshade/logs` (`eden_log.txt`, `stderr.log` and any crash report) with the game's name and title ID. If it stopped or crashed, try once more with `"switch2": {"gpu_split": false}` in `/data/nightshade/config/prosperoeden.json` and say which run was which. The lines that say the most: `Switch 2 title (experimental)`, `GPU thread split`, `EDEN_GUEST_MEMORY` (`sparse refused` means the console would not reserve the game's memory), `Draw skipped`, `Native compute shader rejected` and `Using auto stub fallback`.
- **Switch 1 games.** Play what you played on v1.011.001. For a game that now plays worse, starts slower or stops where it didn't, send `/data/nightshade/logs` from this version and say how it went on v1.011.001.

### Install

From 1.005.102 or later, **Update now** applies it (from a Nightshade before v1.011.000, open the old tile once afterwards). If it doesn't, send `/data/nightshade/logs/updater.log` (`/data/prosperoeden/logs/updater.log` from a Nightshade before v1.011.000). By hand, copy this ZIP's `PPSA64448` folder to `/data/homebrew/PPSA64448`.

## v1.011.001

**Nightshade's own app with v1.010.005's Tears of the Kingdom fixes and amiibo.** Everything in v1.011.000 (its own title ID, the bridge from the old tile) and everything in v1.010.005 below. With its own data folder, amiibo dumps go in `amiibo/` in the game files folder or in `/data/nightshade/amiibo/`.

## v1.011.000

**Nightshade has its own app now, and an earlier install moves over by itself.** Nightshade runs under its own title ID, `PPSA64448`, with its own data folder, `/data/nightshade`, beside the official ProsperoEden app (`PPSA99008`, `/data/prosperoeden`) instead of in its place. An install of an earlier Nightshade updates as usual and then brings you to the new app.

### Moving over

- **Update now turns the old tile into a bridge.** The release ZIP carries Nightshade as `PPSA64448/` and a small bridge as `PPSA99008/`, so an earlier Nightshade's own update takes the bridge. Opened, the old tile says "Nightshade has its own app now", installs Nightshade's own app from the same release when the console doesn't list it yet (the ZIP from GitHub, checked against the release's `SHA256SUMS`, put beside the old folder by the update helper, as **Settings > Apps** installs an app), waits for the homebrew loader to list it, and opens it. When something fails, it says what, with **Try again** and **Close**. It leaves `/data/prosperoeden` alone. See [INSTALL.md](INSTALL.md#coming-from-a-nightshade-before-its-own-title-id).
- **Your data comes along at the first start.** The first time Nightshade starts and finds `/data/prosperoeden`, it asks once: **Move** it (at once, with your games), **Copy** it (both apps keep working; your games stay where they are) or **Start fresh**. Nothing in `/data/prosperoeden` is ever deleted. See [INSTALL.md](INSTALL.md#moving-from-prosperoedens-folder).
- **Forwarders made for the old title ID keep working.** The bridge passes `--rom` and `--exit-after-game` on to the new app. When that is its first start, the question about your data comes first, and the game starts after it: once Nightshade has started again after **Move** or **Copy**, at once after **Start fresh**. A forwarder's game inside `/data/prosperoeden` is found in `/data/nightshade` after a move.
- **Afterwards the old tile can go.** Delete it, or use it for the official ProsperoEden app again; keep it while you use forwarders made for the old title ID.

### Look

- **The launcher says NIGHTSHADE.** The home screen's header, the menu's header and the opening no longer show ProsperoEden's name.

### Install

From 1.005.102 or later, **Update now** applies it: then open the old tile once. If it doesn't, send `/data/prosperoeden/logs/updater.log`. By hand, copy this ZIP's `PPSA64448` folder to `/data/homebrew/PPSA64448` and open Nightshade from its new tile once the homebrew loader lists it.

## v1.010.005

**Tears of the Kingdom: the crashes after 20 to 75 minutes, and amiibo.** A tester's TOTK ended five times on v1.010.002 to v1.010.004, each the same way: after 20 to 75 minutes of normal play the game suddenly wrote gigabytes of memory it had never used, until the console's memory ran out (the app closed) or the game stopped by itself ("jumped to an unmapped address"). All five ran with **Settings > Performance > Memory** at 8 GiB and no mods. Not confirmed on a console yet: play TOTK as you did, and send `/data/prosperoeden/logs` if it still ends.

### Fixes

- **Tears of the Kingdom without mods plays with 4 GiB, whatever Memory says.** The larger memory is for mods that need it. A game told it has 8 GiB is handed 6.4 GiB, twice what a Switch gives it, and TOTK fills it: in the four sessions where the memory ran out, the same piece of the game's own code was writing 0.4 to 1.8 GB of new memory within seconds (on v1.010.004, 1.8 GB of free memory was gone within 5 seconds), and in the fifth the game wrote 3.4 GB and then stopped. With 4 GiB it has the memory it was made for. With one of its mods switched on, the memory you chose stands, since the mod may need it; the `Memory layout:` line in `stderr.log` says when this applies.
- **The game's own memory has 1 GiB that nothing else can take.** Nightshade kept 1 GiB of the console's memory free for the game's sudden needs, but only on paper: the graphics, the emulator's own memory and the driver could still take it, or leave it in pieces too small for the game (the crashes found pieces of 1 or 2 MiB). Nightshade now holds that 1 GiB itself, fills it up again ten times a second, and the graphics start giving memory back as soon as the game has used half of it, before the game has to wait. `stderr.log` shows what it holds (`reserve=` in the `EDEN_MEMORY` and `EDEN_CRAM` lines).
- **The system reads the game's data from the right place in long sessions.** When a game hands the system a buffer (a sound to play, a request, a file to save), the emulator reads it where it lies if it is in one piece, and decided that by a number each piece of mapped memory gets. The numbers run out after 65,536 of them, 43 minutes into TOTK, and from then on part of a buffer could be read from the wrong memory. Every page of the buffer is now checked.

### New

- **Amiibo.** Player 1's controller now has an NFC reader, so a game that asks for an amiibo shows its own scan screen instead of error 2115-0064, and you can cancel it as on a Switch. To use one, put its dump (a 540-byte `.bin`) in `amiibo/` in your game files folder: each scan is shown the next dump there, in name order, a second after the game starts looking, and a game that writes to the amiibo writes to that file. With the console's amiibo key (`key_retail.bin` in `keys/`) a game reads everything on it; without it, what the game needs to know the figure. Nightshade includes no amiibo. See [Amiibo](https://github.com/Oliv3rmoon/Nightshade/blob/v1.010.005/docs/INSTALL.md#amiibo).

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.010.004

**Faster starts for games with a big shader cache.** Every start of a large game builds its saved pipelines before it plays: Tears of the Kingdom's 14,000 took 552 s at a first start and 208 s at later ones. Not measured on a console yet: `stderr.log` has the new times.

### Fixes

- **The shader cache builds on every CPU at start.** Eden builds pipelines on 15 threads, but every background thread is placed on the console's five spare CPUs, so the builders shared five while the game's own cores and the GPU thread's sat idle before the game began. While the cache is built they may now use every CPU the app has (`EDEN_WORKER_PLACED name=VkPipelineBuilder ... startup=1` in `stderr.log`), and once the game runs each goes back to the spare CPUs, so play is as before.
- **`stderr.log` shows what holds the console's memory, every 5 seconds** (`EDEN_MEMORY`): the game's own memory, Eden's heap and tables, the graphics allocator, the largest free block, and how the game's writes that had to wait for memory ended. Tears of the Kingdom still ran out of memory 45 minutes in on v1.010.003, and the next log will show which of these grows until it does.

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.010.003

**Tears of the Kingdom: no more crash when the game's memory runs out.** A tester's TOTK ended 23 minutes in on v1.010.002, when the game took 1.8 GB of new memory within seconds; this release gives the game that memory instead of ending the session. Not confirmed on a console yet: send `/data/prosperoeden/logs` after playing.

### Fixes

- **The game's memory comes first.** Nightshade gives the emulated console its memory as the game writes it. When none was left, the write was refused and the game closed: the graphics caches hold most of the console's memory and free textures only at the end of each frame, too late for a game that loads a big area at once. Now the write waits up to 2 seconds while the caches give memory back (they see none free and free at their hardest), and only then gives up (`EDEN_CRAM` lines in `stderr.log` say when it waited).
- **1 GiB kept free for the game's own memory.** The graphics caches now treat 1 GiB less as free than there is, so they give textures back sooner and a game that loads a big area at once (its own memory and the area's textures together) has room. A second TOTK crash, 79 minutes in with ASTC textures compressed, came from exactly that.
- **`stderr.log` says how many threads build the shader cache** (`EDEN_PIPELINE_WORKERS`). A large game's first start is still long (TOTK's 14,000 pipelines take minutes): the builders share the console's spare CPUs while the game's own cores sit idle at that point, and letting them use those too is the next step.
- **ASTC textures at full quality take four times the memory.** If a large open-world game crashes after a while, set **Settings > Video > ASTC textures** back to the default first.

### Under the hood

- **Built with Belladonna,** Nightshade's own fork of Mihawk's PS5 Mesa (RADV) driver. The driver is the same as before, so the shaders your games already built stay built.

### Apps

- **Settings > Apps lists the companion apps by their new names:** Physalis (was Nightshade TV), Datura (was Nightshade Stream) and Mandrake (was Nightshade Station). Same apps, same title IDs; their settings stay.

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.010.002

**The likely cause of Final Fantasy Resonance stopping the GPU, fixed: draws made from arguments the GPU hadn't finished writing.** Every stop in Oliv3rmoon/Nightshade#1, in the opening scene and after it, came right after draws asking for 27 million to 2 billion copies of a model (one count read as the float 1.0): far more than any game draws, so the GPU ran them until it was reset. Not confirmed on a console yet.

### Fixes

- **Draw arguments the GPU wrote are read once its newest write is in memory.** Some games have the GPU work out how many copies of a model to draw. Eden marks memory the GPU wrote so it fetches it before the CPU reads it, but a fetch that finished could clear the mark of a range the GPU had written again since, and Eden then took the older contents as current. Such a range now stays marked until its newest contents arrive (Eden patch 11). The `submit_sync` switch made this rarer by waiting for the GPU after every submission; it is no longer needed for it.
- **Arguments that came from GPU memory are read again before use at every accuracy level**, not only at High.
- **A draw that can't be real is dropped instead of stopping the GPU.** If a draw the CPU makes from such arguments still asks for more than about 4 billion vertices after they are read again, Nightshade skips it and says so in `stderr.log` (`EDEN_INDIRECT_DRAW_DROPPED`).

### For testing a game that stops the GPU

- **The stall report shows an indirect draw's arguments** (its count and copies, and how many draws a multi draw makes), read from memory, so a stop on a draw the GPU makes itself shows whether its arguments were wrong too.

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.010.001

**Closer to the GPU stop in Final Fantasy Resonance: shaders read memory the way the game asks, switches to test a game that stops the GPU, and a stall report that names the shader.** Final Fantasy Resonance still stops in its opening scene on v1.010.000 (Oliv3rmoon/Nightshade#1); this release gives the next test runs what they need to find where, and one more fix that may be it. Not confirmed on a console yet.

### Fixes

- **Loads and stores that the game marks "don't cache" reach memory.** A Switch shader can ask that a read come from memory rather than a cache (`LDG.CG`, `LDG.CV`) or that a write go straight through (`STG.CG`, `STG.WT`); Eden dropped that request, so on the PS5's GPU such a read could keep answering from a stale cached copy. Nightshade now keeps it. Shaders that don't ask compile as before.

### For testing a game that stops the GPU

- **When the GPU stops, `stderr.log` says where.** Besides what the stuck submission carried, it now names the first GPU wait that reads as unmet, the last draws and dispatches before it with how much work each asked for and the address of each shader, and the pipeline each address belongs to; Eden logs every pipeline it builds (`EDEN_PIPELINE`), so a report links the stop to the game's own shader.
- **Switches per game in the settings file** (`"gpu_debug"` in the game's entry): `coherent_storage` (`"compute"`: every compute shader, `"all"`: every shader reads memory, not a cached copy, not only those with atomics), `robustness2` (reads outside a buffer or image give zeroes, writes there are dropped), `submit_sync`, `hcr`, `compute_barriers` and `conditional_rendering`. `stderr.log` says which are in effect (`EDEN_GPU_DEBUG`). See [For testing a game that stops the GPU](https://github.com/Oliv3rmoon/Nightshade/blob/v1.010.001/docs/SETTINGS.md#for-testing-a-game-that-stops-the-gpu).

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.010.000

**Nightshade's biggest release: a likely fix for games that stop the GPU, three Nightshade apps beside it, and backups to USB.** Built and checked by this repository's release workflow and test suite (the Linux host tests and the launcher's screens in all three layouts), and the new shader code was checked with Eden's shader compiler on a PC; the GPU fixes have not been confirmed on a console yet, so tell me how your games do.

### Fixes

- **Games that stop the GPU while they wait on another part of the picture.** On the PS5's GPU an ordinary read of a game's memory can be answered from a cache that another part of the GPU's writes never reach, so a shader that waits for that write waits forever, the GPU stops and the console with it; the Switch's GPU doesn't keep those reads, so games wait that way. In shaders that use atomics, Nightshade now reads that memory from where the writes go. The same applies to how Eden adds floating-point numbers atomically (a retry that read a stale copy could retry forever), which any game with float atomics uses. Final Fantasy Resonance still stops in its opening scene on a tester's console (some runs right after New Game, some a few seconds into the scene); that is still being worked on in Oliv3rmoon/Nightshade#1. Other games that freeze the console may be helped too (Swordpdf/ProsperoEden#277, Swordpdf/ProsperoEden#52, Swordpdf/ProsperoEden#233), not confirmed yet.
- **Compare-and-swap in a shader's shared memory (`ATOMS.CAS`)** is translated: a game whose log says `Instruction ATOMS (cas) is not implemented` had that shader skipped, as Final Fantasy Resonance's `ATOM (cas)` was before v1.005.105.
- **When the GPU stops, Nightshade closes** instead of starting again: an app started again found the GPU still stuck and didn't respond either. Confirmed on a console: the console stays responsive, with no need to pull the power cord. The next start says where the crash report is, as before.

### New

- **Settings > Apps: Nightshade TV, Nightshade Stream and Nightshade Station.** Three apps in Nightshade's black and red, each its own release on GitHub, installed over the app it comes from (same title ID and data, so your channels, paired PCs and memory cards stay):
  - [Nightshade TV](https://github.com/Oliv3rmoon/Nightshade-TV): ProsperoTV's IPTV client, now with **Plex** (sign in on the TV with a code, your servers and the ones shared with you, movies and shows, resume).
  - [Nightshade Stream](https://github.com/Oliv3rmoon/Nightshade-Stream): ProsperoLight, the Moonlight client, for playing your PC's games on the PS5.
  - [Nightshade Station](https://github.com/Oliv3rmoon/Nightshade-Station): SwanStationPS5, the PS1 emulator.

  **Settings > Apps** shows which are installed and their version. **Get** and **Update** download the newest release, check it against its `SHA256SUMS`, and install it with the update helper; **Open** closes Nightshade and starts the app (with a payload loader running, as for updates). See [Apps](https://github.com/Oliv3rmoon/Nightshade/blob/v1.010.000/README.md#apps).
- **Back up to USB.** **Settings > Diagnostics > Back up to USB** copies the settings, profiles and every save to `nightshade-backup/<date-time>/` on a USB drive, with a `SHA256SUMS` to check them on a PC; caches, logs and games stay out. To restore, copy the folder back with the app closed. See [Backing up to a USB drive](https://github.com/Oliv3rmoon/Nightshade/blob/v1.010.000/docs/INSTALL.md#backing-up-to-a-usb-drive). (Swordpdf/ProsperoEden#157)
- **Wallpapers from the game files folder.** Images in `wallpapers/` of the game files folder you copy from a PC are listed in **Settings > Appearance > Wallpaper** too, so a new wallpaper needs no FTP. (Swordpdf/ProsperoEden#263)

### Also

- The release build waits out GitHub's rate limit when it downloads its dependencies, and the update helper builds cleanly with GCC.

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.005.105

**A shader Final Fantasy Resonance needs in its opening cinematic now runs.** A few seconds into the opening cinematic after New Game, the game hung the GPU and took the console with it: controller and power button stopped responding (Oliv3rmoon/Nightshade#1). One of its compute shaders uses a compare-and-swap instruction (`ATOM.CAS`) that Eden did not translate, so that shader never ran, and the GPU waited for its work forever. The instruction is now translated. It is not yet confirmed on a console that the game plays on past that point: if it still stops, send the new crash log. Any other game whose log says `Instruction ATOM (cas) is not implemented` gets the same fix.

### Install

From 1.005.102 or later, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.005.104

**A mod copied straight into the mods folder works.** A mod's `romfs` or `exefs` folder put directly in `mods/<title ID>/` (or a game folder's `mods/`), without a folder of its own, applied nothing: Eden reads a mod's files one folder down, so it took `romfs` for the mod's name and found nothing in it. A tester's Ori mod at `mods/01008DD013200000/romfs/Data/` did not load. Such files are now one mod, **Unnamed mod**, with its own switch in the game's Mods list (in all 30 languages). Mods in folders of their own work as before. See [Mods and cheats](https://github.com/Oliv3rmoon/Nightshade/blob/v1.005.104/docs/MODS.md).

### Install

From 1.005.102 or 1.005.103, **Update now** applies it. If it doesn't, send `/data/prosperoeden/logs/updater.log`. From an older version, copy this ZIP's `PPSA99008` folder over yours.

## v1.005.103

**Cross on a game from your server plays it, on the home screen too.** In the Nightshade theme (and Slate) the home screen's grid is the Library, but Cross on a game that is only on a download source put it in the download queue, and on one already downloading cancelled it. Now it downloads the game and starts it, as Cross does in the Library (Square still only queues it), and so does Play on the game panel. Game settings on such a game says why it has none yet.

This is the first update that 1.005.102's fixed update helper installs: **Update now** should apply it. If it doesn't, send `/data/prosperoeden/logs/updater.log`.

### Also

- The launcher's own preview walks the Nightshade and Slate layouts as it walks Canopy's, and all of its checks pass in all three.

## v1.005.102

**Update now works.** The in-app update said it had finished, but the old version opened again and the update was offered again. The update helper waits for the app to close before it replaces its files, and took the app's sandbox in `/mnt/sandbox` for the app itself; the system can leave that sandbox behind after the app has closed, until the console restarts (Swordpdf/ProsperoEden#232), so the helper gave up after two minutes. Now the app's connection to the helper ending is the app closing: a sandbox that stays 10 s longer is taken for the system's leftover, and the files are replaced. The helper also writes what it did to `/data/prosperoeden/logs/updater.log`.

An update runs the helper of the version it updates from, so this one has to be copied by hand once from 1.005.100 or 1.005.101 (copy this ZIP's `PPSA99008` folder over yours). From 1.005.102 on, **Update now** applies the next one.

### Also

- The two frame generation texts that showed in English in every language (Frame gen method, and the message when MihawkGen's weights are missing) are translated in all 30.
- A release build now stops on an untranslated text, and runs the update helper's own tests.
- A game that keeps reading memory that isn't there no longer fills `eden_log.txt` in seconds: each session logs its first 64 such reads and writes, then one at each power of two with the count, so the log keeps what came before.

## v1.005.101

**A game's update or DLC left in `roms/` beside it now applies.** A tester's Ori and the Will of the Wisps sat on "Starting the game" for minutes: it started as 1.0.0 without its 1.2.1 update, and 1.0.0 stopped there. Nightshade read updates from `updates/` and game folders only, so an update left in `roms/` beside its game was never applied. Now an update or DLC file directly in `roms/` whose name holds its title ID, as dumps are named (`[01008DD013200800][v196608]`), applies to its game, the Library no longer lists it as a game of its own but counts it for its game, and **Delete from console** deletes it with the game. `updates/` and game folders work as before. See [Install](https://github.com/Oliv3rmoon/Nightshade/blob/v1.005.101/docs/INSTALL.md).

### Also

- **A start that shows no picture says why.** If "Starting the game" lasts 45 s, `stderr.log` gets an `EDEN_START_WAIT` line (whether the graphics were busy, and when the game last asked the system for something), and the game's last 64 service calls; again at 90 and 180 s. Send that log with a report.
- The loading screen says Nightshade under the game's cover.

### Install

Nightshade v1.005.100 offers this update when it starts: choose **Update now**. Or copy this ZIP's `PPSA99008` folder over your current one, as before.

## v1.005.100

**Nightshade: Oliv3rmoon's own release of ProsperoEden.** It's the official 1.005.000 with my fixes and features on top: everything from my test builds 2 and 3, every open community fix, a new name and a dark look of its own, and updates from this repository. It installs over the official app (same title ID, same `/data/prosperoeden`), so your games, settings, saves and covers stay. Built and checked by this repository's release workflow and test suite; not every change has been run on a console yet, so tell me how it goes.

### New name, new look

- **Nightshade.** The PS5 home screen and the launcher call it Nightshade, with its own icon (a red nightshade flower with its black berries), home background and launch picture.
- **Nightshade theme (the new default).** Black and red, clean and dark: tabs at the top, a quiet home screen, settings as a list, a near-black sky with a low red glow. Accent presets: Red, Crimson, Rose, White, Violet, Ice. Canopy and Slate are still in **Settings > Appearance > Theme**, and a theme you chose before stays.

### New

- **Updates from Nightshade's own releases.** At the start, Nightshade asks [Oliv3rmoon/Nightshade](https://github.com/Oliv3rmoon/Nightshade/releases) for a newer release instead of the official catalog, shows its notes, and installs it with **Update now** after checking the ZIP against the release's `SHA256SUMS`. From this release on, the app itself offers the next one. See [Updating](https://github.com/Oliv3rmoon/Nightshade/blob/v1.005.100/docs/INSTALL.md#updating).
- **Play a game from your server without downloading it (experimental).** A download source with `"stream": true` in `sources.json` plays its games where they lie: Cross starts one at once, Square still downloads it. Your network decides loading and stutter. See [Playing a game where it lies](https://github.com/Oliv3rmoon/Nightshade/blob/v1.005.100/docs/DOWNLOADS.md#playing-a-game-where-it-lies). (Swordpdf/ProsperoEden#111)
- **Games on a USB drive, everything else on the console.** `"games_folder"` in the settings file names a folder of its own for `roms/` and `updates/`. (Swordpdf/ProsperoEden#50, Swordpdf/ProsperoEden#149)
- **Fonts for translation mods.** A mod's replacement system font goes in `mods/0100000000000811/romfs/`. (Swordpdf/ProsperoEden#248)
- **Pokémon Legends: Z-A:** a per-game `"vertex_input_dynamic": false` for the flashing colour after the train station. (Swordpdf/ProsperoEden#59, Swordpdf/ProsperoEden#112)
- **Compressed games:** NSZ and XCZ play as they are. (Swordpdf/ProsperoEden#285)
- **library.json for forwarder makers:** the Library's games, with their paths, names and covers, for other homebrew to read. (Swordpdf/ProsperoEden#292)
- **Mihawk's newest PS5 graphics driver** (2026-10-13). Each game builds its shaders again the first time you play it.

### Fixes

- **Long sessions crashing on a write to the game's own memory** (Dragon Quest VII and others): tested on a PS5 Pro, an hour where it crashed at 15 minutes. (Swordpdf/ProsperoEden#246, Swordpdf/ProsperoEden#296)
- **Big mods (UltraCam) stopping at the app's 3 GiB heap:** it can grow to 6 GiB. (Swordpdf/ProsperoEden#246)
- **"Audio PCM queue capacity exceeded" crash:** the oldest audio is dropped instead. Tested: 30 minutes, no crash, no skips. (Swordpdf/ProsperoEden#270, Swordpdf/ProsperoEden#289)
- **Metroid Prime 4 out of graphics memory:** staging buffers sized to their transfer. (Swordpdf/ProsperoEden#195)
- **Controls dead after rest mode or the home menu:** the controller is opened again. (Swordpdf/ProsperoEden#97, Swordpdf/ProsperoEden#269)
- **"Loader status 28":** now says which key your prod.keys is missing. (Swordpdf/ProsperoEden#206, Swordpdf/ProsperoEden#279, Swordpdf/ProsperoEden#295)
- **Game folders:** a folder's mods no longer break its DLC (Swordpdf/ProsperoEden#288), forwarders find games moved into folders (Swordpdf/ProsperoEden#290), the home page shows them (Swordpdf/ProsperoEden#286), the newest of several updates is used whole (Swordpdf/ProsperoEden#287), and Settings counts them (Swordpdf/ProsperoEden#291).
- **Idle cores no longer spin a CPU at 100%, and the overlay shows real load** (Kirby: tested, 30 FPS held). (Swordpdf/ProsperoEden#284, Swordpdf/ProsperoEden#275, Swordpdf/ProsperoEden#293)
- **Smash 13.0.4 Skyline mods and post-processing paths.** (Swordpdf/ProsperoEden#276)

### For bug reports

When a game hits one of these, the logs now say why: GPU stalls (`radv/ps5 stall:`), error 2010-0212 (`EDEN_IPC_RING`), a refused write to guest memory, textures freed under memory pressure, an update that finds the app still running. See the issues on Swordpdf/ProsperoEden for what to send.

### Install

Close ProsperoEden (or Nightshade), copy this ZIP's `PPSA99008` folder over your current one (usually `/data/homebrew/PPSA99008`), and start it. To go back, copy the official [v1.005.000](https://github.com/Swordpdf/ProsperoEden/releases/tag/v1.005.000) over it the same way.

### Thank you

To BlackBearReloaded and everyone behind ProsperoEden, Mihawk for the PS5 graphics driver, and the testers who ran my builds: uzzielchaidez, pankosoknap, pnvnd, yokozuna47 and RuiNelson. Requests and bug reports: [Discord](https://discord.gg/pSqgRU3YP9). Support: [Buy me a coffee](https://buymeacoffee.com/oliv3rmoon).

## v1.005.000

This alpha keeps each game in a folder of its own with everything that belongs to it, gives cheats their own list, adds a speed limit and a second frame generation method, and fixes a lost graphics device and black boxes that spread over the picture. Performance and compatibility still vary by game.

### New and improved

- **One folder per game.** A game can sit in a folder of its own in `roms/`, together with its update and DLC files, its mods (`mods/`), its cheats (`cheats/`) and its own post-processing presets (`reshade/`). ProsperoEden tells the game from its update and DLC by what each file holds, so the files can have any names. **Delete from console** removes the folder's update and DLC too. Games downloaded from a source now go into a folder of their own the same way. Your files stay where they are and keep working. See [Game files folder](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/INSTALL.md#game-files-folder).
- **Cheats in a list of their own.** A game's settings have a **Cheats** row next to **Mods**: every cheat of the game with its own switch, and an **All cheats** switch apart from the Mods one. **Mods** now lists patches and replacement files only. Cheats you chose before stay on.
- **A game's own post-processing presets.** Presets and effects in a game folder's `reshade/` are offered in that game's **Post-processing** list, after the shared ones.
- **Speed limit (new Performance setting).** **Settings > Performance > Speed limit**, and the same row in a game's own settings: 100% (as before), 150%, 200%, 300% or Unlimited. A game locked to 30 FPS can run faster when the console has time to spare; for a game whose logic follows its frame rate, the whole game runs faster.
- **MihawkGen frame generation.** **Frame gen method** in **Settings > Video**, and per game, chooses between MihawkGen, a neural network that comes with the app, and Lossless Scaling with your own copy of it.
- **Update notices (new setting).** **Settings > Diagnostics > Update notices** turns the update dialog at start off. **About** still says when a newer version is on homebrew.page.

### Fixes

- **A lost graphics device while a game loads a new area.** Long runs of uploads and copies could grow past what the console's GPU takes in one submission, and the game stopped. The graphics driver now splits them.
- **Black boxes that grew until they covered the screen.** A few pixels whose colour was not a number spread through a game's glow and blur effects. Such a colour is now written as black, and stays one pixel.
- **Games that ask for a large GPU address space start** instead of stopping at once.
- **No update offered for the version already installed**, when the app's files named an older version than the program.

### Thank you

Thanks to everyone whose contributions landed since v1.004.000:

- [bawsdeep](https://github.com/bawsdeep) — MihawkGen frame generation in the app.
- [mihawk-99](https://github.com/mihawk-99) — MihawkGen itself, the neural frame generation it uses.
- [BlackBearReloaded](https://github.com/blackbearreloaded) — game folders, the Cheats list, a game's own presets, the speed limit, update notices, the graphics fixes, translations and integration.

Thanks also to everyone testing and reporting problems on Discord and GitHub, the **Eden developers** for the emulator, and **Mihawk** and the graphics contributors for the platform work this port builds on.

### Before updating

Back up your saves and settings. The package contains no games, firmware or keys; keep your existing user-provided files. This remains an experimental pre-release.

## v1.004.000

This alpha adds post-processing looks and wallpapers of your own, frees more memory, and makes games with many shaders start and start again faster. Performance and compatibility still vary by game.

### New and improved

- **Post-processing (new setting).** **Settings > Video > Post-processing**, and the same row in a game's own settings, draws a look over the picture with ReShade FX effects: Off, or one of six presets that come with the app (Anime, Cinematic, Clean, Dreamy, Retro, Vivid). Presets of your own go in `/data/prosperoeden/user/post_presets/` and their effects in `/data/prosperoeden/user/post_shaders/`. It works with the Vulkan renderer; see [Post-processing](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/SETTINGS.md#post-processing).
- **A wallpaper of your own (new setting).** **Settings > Appearance > Wallpaper** shows an image from `/data/prosperoeden/wallpapers` behind the launcher's screens instead of the theme's artwork: PNG or JPEG (also BMP, GIF, TGA and DDS), put in the folder over FTP and listed when the dialog opens. A game's own cover still takes the place where the theme shows one. An image of another shape is resized to fit the screen whole, with the theme behind what it leaves over. See [Wallpapers](WALLPAPERS.md).
- **More free memory in 64-bit games.** The emulated console's memory is now taken from the PS5 only where a game writes it, and given back where the game's console clears or frees it, or where it holds only zeroes again. Two games measured on the console held 644 MiB and 1,956 MiB less, at the same frame rate. After "A Compressed RAM Service" (Gregory Price, Linux Plumbers 2026).
- **Compact memory (new Performance setting, off by default).** Gives back memory a game wrote but left mostly empty, 16 KiB at a time: 100 to 300 MiB more in the same two games. A game that keeps rewriting such memory can stutter; leave it off unless a game runs short of memory.
- **Games with many shaders start, and start faster the next time.** A game with thousands of pipelines could stop during its loading screen because the locks that guard pipeline builds ran out; they are now shared. The graphics driver keeps its compiled shaders between starts on every setup, and its pipeline cache is kept when a write is cut short, so a large game compiles them once instead of at every start. A line at launch says where the cache is kept.
- **A game's language from its update.** A game whose languages come from a packed update (NSP or XCI) is offered them again, instead of falling back to English.
- **Crash reports name the allocation that failed** when a game runs out of memory.
- **Public rooms.** The project's own rooms are listed first, the list scrolls, and L1/R1 move a page at a time.
- **Launcher packs update themselves.** A custom launcher pack can name an update feed: ProsperoEden checks it, downloads a newer pack after you confirm, verifies its size and SHA-256, and can go back to the previous version. Libraries load faster in packs. See [Runtime launcher packs](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/RUNTIME_LAUNCHERS.md).
- **Slate theme polish** and the launcher texts of the new settings in all 30 languages.

### Thank you

Thanks to everyone whose contributions landed since v1.003.000:

- [Oliv3rmoon](https://github.com/Oliv3rmoon) — games with many pipelines start, the pipeline cache kept when a write is cut short, the refused allocation in crash reports, and a game's language from a packed update.
- [bawsdeep](https://github.com/bawsdeep) — wallpapers of your own and Slate theme polish.
- [kasobos67](https://github.com/kasobos67) — launcher pack updates from a feed and a faster library for packs.
- [BlackBearReloaded](https://github.com/blackbearreloaded) — post-processing, more free memory and Compact memory, the shader cache between starts, public rooms, translations and integration.

Thanks also to everyone testing and reporting problems on Discord and GitHub, the **Eden developers** for the emulator, and **Mihawk** and the graphics contributors for the platform work this port builds on.

### Before updating

Back up your saves and settings. The package contains no games, firmware or keys; keep your existing user-provided files. This remains an experimental pre-release.

## v1.003.000

This alpha focuses on mod stability, controller support, graphics correctness and lower emulation overhead. Performance and compatibility still vary by game; these changes do not guarantee a particular frame rate.

### Fixes and improvements

- **TOTK + UltraCam startup crash fixed.** Large modded RomFS rebuilds could overflow the filesystem thread stack during cleanup and close the whole app. Cleanup is now iterative. Tested with TOTK **1.1.1** and an older UltraCam mod: the game reached its menu, and the user confirmed the free camera works in gameplay. Game/mod versions still need to match; this is not a claim that every UltraCam version or TOTK update works.
- **Memory size per game.** Choose 4, 6 or 8 GiB of emulated console memory under **Settings > Performance > Memory**, with game-specific overrides for demanding mods. This uses the PS5's existing shared memory pool; it does not add physical RAM. See [Console memory](SETTINGS.md#console-memory).
- **DualSense audio haptics.** HD rumble can use the controller's audio haptics path, with lifecycle and failure handling fixes.
- **Touchpad and controls.** Split touchpad clicks are supported, shortcuts use the left half, and transient upright Joy-Con button events are suppressed. Core menu actions can be remapped, with translated menu keybinding labels.
- **Clearer performance overlay.** An optional Vulkan readout shows the game's own render resolution, rather than the TV output resolution. CPU-load integration and a stray HUD accent line were corrected.
- **Launcher polish and customization.** Slate theme and accent presets, persistent accent selection, corrected status layout, and experimental external launcher packs. See [Runtime launcher packs](RUNTIME_LAUNCHERS.md).
- **Startup and diagnostics.** Font access is detected correctly behind sandbox data mounts; OnionHEN app-jailbreak support supplements Lapy elevation. The launcher exposes the crash-report location.
- **Graphics fixes.** Vulkan dynamic vertex input fixes address scenes rendered as one flat colour. A RADV geometry-shader path no longer reads through null pointers. Texture updates can upload only changed block rows in large textures.
- **CPU and GPU efficiency.** JIT page-table and instruction-generation improvements, safer batched code-cache commits, checked-fastmem improvements, reduced GPU cache-lock contention, faster register-command processing, adjusted guest scheduling/vsync, and a larger OpenGL command queue. The video core uses ThinLTO targeting Zen 2; worker topology is detected once per process.
- **Build and language support.** Malay translation, fixes for host checks and symlinked checkouts, and refreshed contributor credits.

### Thank you

Thanks to everyone whose contributions landed since v1.002.000:

- [Rockiu0](https://github.com/Rockiu0) — CPU/JIT, graphics, scheduling, elevation and build improvements.
- [Tiang-88](https://github.com/Tiang-88) — per-game memory layouts and the render-resolution overlay.
- [GuilhermeAmado](https://github.com/GuilhermeAmado) — DualSense audio haptics.
- [kasobos67](https://github.com/kasobos67) — custom runtime launcher support.
- [bawsdeep](https://github.com/bawsdeep) — remappable menu actions.
- [cippy-9](https://github.com/cippy-9) — Malay translation.
- [BlackBearReloaded](https://github.com/blackbearreloaded) — integration, testing, launcher/controller fixes, font access and the RomFS crash fix.

Thanks also to [zhangmu34](https://github.com/zhangmu34) for the font-access issue report, everyone testing and reporting problems, the **Eden developers** for the emulator, and **Mihawk** and the graphics contributors for the platform work this port builds on.

### Before updating

Back up your saves and settings. The package contains no games, firmware or keys; keep your existing user-provided files. This remains an experimental pre-release. Long gameplay sessions and all game/mod combinations have not been exhaustively tested.

## v1.002.000

- **More free memory for demanding games.** The console's memory is one pool shared by the emulated console, the emulator itself and the graphics, and part of it was held without being used. The emulated console's memory, the code caches and a reserve on the system's side now take memory only when they need it. A large open-world game at 2x has about 3.5 GiB free during play instead of 2.2 GiB, at the same frame rate, so a long session runs out of memory far later.
- **PS5 Pro memory mode.** On a PS5 Pro, ProsperoEden asks for the larger memory pool the console offers: 13.5 GiB instead of 12 GiB. A line at launch (`memory:` in `stderr.log`) says whether it is active. Contributed by [OverkillLabs](https://github.com/OverkillLabs3).
- **"Ran out of graphics memory" less often above 1x.** The part that frees textures no game is using could stop looking before it had freed anything, so graphics memory filled up although much of it could be freed. Contributed by [mzet97](https://github.com/mzet97).
- **Vibration strength (new setting).** **Settings > Controls > Vibration strength** sets how strong the DualSense vibrates: 25%, 50%, 75% or 100% of what a game asks for. The controller answers each change with a short vibration at the new strength, so the level is chosen by feel.
- **A game without your language starts in English.** With a language chosen that a game does not have, the game started in the next language of the console's own order: with Brazilian Portuguese that was Spanish. It now gets the chosen language or a variant of it when it has one, and English otherwise.
- **The FPS overlay is a small system monitor.** Turned on in **Settings > Video > FPS overlay** (or in a game's own settings), it shows over the game: the graphics backend, the frame rate against the game's own, the frame time, each of the four emulated CPU cores, the GPU's busy time measured by the GPU itself, the memory taken of the pool and the graphics' part of it, for example `VLK 30/60FPS 34.6MS CPU 92% 45% 38% 3% GPU 71% MEM 7.7/12.1G GFX 4.1G`. **Settings > Video > Overlay options** turns each piece on or off, sets its size, position and colours, and with **CPU by core** off shows only the busiest core; a game's settings can give it its own size and position. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Variable refresh rate on older system software.** On system software 6.02 the console did not know the way ProsperoEden declared a variable refresh rate and listed the app without one. It is now declared the way older system software knows.

**Thanks** to the people whose work is in this release:

- [OverkillLabs](https://github.com/OverkillLabs3), for PS5 Pro memory mode, measured on a PS5 Pro.
- [Bauls Deep](https://github.com/bawsdeep), for the FPS overlay's new look as a system monitor, with its options and the settings per game.
- [mzet97](https://github.com/mzet97), for finding and fixing why the texture collector stopped freeing graphics memory.
- [Mihawk](https://github.com/mihawk-99), for the measurements of the console's memory pools that the memory work builds on, and for the Vulkan driver all of this runs on.
- [tonysantosl](https://github.com/tonysantosl), for reporting the language that games fell back to, and [zh522130](https://github.com/zh522130), for asking for a vibration that can be turned down.

And to everyone who tests ProsperoEden on their own console and reports what they find.

## v1.001.000

- **A new launcher.** The menu was rebuilt. Five sections sit on a rail at the left (Home, Library, Multiplayer, Settings, About): L1 and R1 go from one to the next, and Left from a screen's edge opens the rail. Home is built around the game to continue, with its cover and what it runs with. The Library shows your games as covers, with the chosen game's details beside them. A game's settings are a page of their own, and Settings is a board of tiles.
- **Themes.** The launcher's look is a theme, chosen in **Settings > Theme**. This version comes with one, Canopy; [docs/THEMES.md](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/THEMES.md) says what a theme is made of.
- **An opening.** ProsperoEden appears out of the console's own launch picture without a cut: its mark comes forward over the picture, then goes to its place as the menu arrives. Any button skips it, and it plays only when the app is started.
- **A loading screen with the game's cover.** While a game starts, its cover stands in the middle with a line of light going round it as the start advances, in front of the cover's own colours. Under it are the step the start is at, the shaders built so far, and the seconds a slow step has lasted.
- **The controller's buttons are drawn.** **Settings > Controls** and the button mapping show Touchpad, L1, R1 and the other buttons as pictures instead of names.
- **Game settings from the home screen.** The button beside **Play**, and Triangle on the game to continue or on a recent game, open that game's settings.
- **Demanding games keep their frame rate in heavy scenes.** A game that times its own frames could fall from 30 to 20 frames a second whenever its frames came a few percent late. The clock the game reads now gives by up to 15% in those moments: the game keeps its frame rate and runs a little slower instead. Sound, the display and timeouts keep the real clock.
- **Full speed on newer system software.** On consoles where the system does not tell the processor's cores apart, the emulator's threads were left to share cores and games ran markedly slower. The cores are now found by a timing, and each thread gets its own.
- **No more drops to 10 or 15 frames a second in new scenery.** Textures in a format the PS5's GPU cannot read (ASTC) were converted inside the draw that first used them, which held up the whole frame. In play they are now converted in the background; on loading screens, menus and title screens they are still converted at once, so those pictures appear complete.
- **ASTC textures (new setting).** **Settings > Video > ASTC textures**, also in each game's settings. **Compressed** (the default) keeps such textures as BC3, a quarter of the graphics memory they need decoded; **Full quality** keeps them decoded.
- **"Ran out of graphics memory" far less often.** Four causes are fixed. The count of graphics memory in use read twice what was held, so games were refused memory that was free. ASTC textures took four times the memory they need. The reserve that memory is freed for was smaller than a game can ask for at once. And memory the app keeps for its whole life sat in the middle of the pool, so every game after the first had less room in one piece.
- **Fewer freezes and long frames.** Freeing textures when graphics memory ran short could stop the picture for half a second. A game that read unmapped memory in bursts stalled for up to a second on its own log lines. And the graphics driver held up one frame every ten seconds, in every game, to write a line of statistics. All three are gone.
- **A crash with "jumped to an unmapped address" is fixed.** It came from where the console happened to place the game's memory, so it struck some starts and not others.
- **RNG seed per game (new).** In a game's settings under **System**. With a seed the game is loaded at the same place in memory at every start, which some mods need. Contributed by [EIREXE](https://github.com/EIREXE).
- **Games that open the microphone no longer crash.** Contributed by [rpromas](https://github.com/rpromas).
- **Faster CPU emulation** no longer turns off the fast path for a game's exclusive memory accesses when it is switched on. Contributed by [OverkillLabs](https://github.com/OverkillLabs3).
- **PS5 OpenGL SDK 1.0.2** for the launcher and the OpenGL renderer (it was 1.0.0). A game whose shaders are compiled inside its first frames is no longer ended on its first run with the OpenGL renderer, the menu no longer loses a frame of about 180 ms every few seconds while **Write logs at once** is on, and drawing fixes from [Mihawk](https://github.com/mihawk-99) and [mpereiraesaa](https://github.com/mpereiraesaa) are in.
- **A single Joy-Con held upright.** A game that gives a player one Joy-Con to hold upright is played with the whole DualSense: the shapes are that Joy-Con's four buttons, the D-pad and either stick are its stick, and L1/R1 and L2/R2 are its shoulder buttons.

**Thanks** to the people whose work is in this release:

- [Bauls Deep](https://github.com/bawsdeep), for the texture collector's lookahead, where the fix for the half-second freezes began, and for the development timers and the recorded-input replay.
- [EIREXE](https://github.com/EIREXE), for the RNG seed per game.
- [rpromas](https://github.com/rpromas), for finding and fixing the crash in games that open the microphone.
- [OverkillLabs](https://github.com/OverkillLabs3), for the fix that keeps the fast path for exclusive memory accesses with Faster CPU emulation on.
- [Mihawk](https://github.com/mihawk-99) and [mpereiraesaa](https://github.com/mpereiraesaa), for the drawing fixes in the OpenGL SDK, and Mihawk for the Vulkan driver all of this runs on.

And to everyone who tests ProsperoEden on their own console and reports what they find.

## v1.000.110

- **Multiplayer through Eden rooms (new, experimental).** ProsperoEden can join an Eden room, so a game's Local Wireless mode reaches other players over the internet: other PS5 consoles running ProsperoEden, and Eden on a PC. Open **Multiplayer** (a section of the menu: R1 twice from the home screen), join a room, then start the game and choose its Local Wireless mode. The home screen shows when a room is connected, the game overlay shows the delay to the room in milliseconds, and when the connection is lost the game closes and the launcher says why. See the [setup guide](https://github.com/blackbearreloaded/ProsperoEden/blob/main/docs/MULTIPLAYER.md).
- **Private room.** Enter a room's address, UDP port, a nickname and its password. They are saved as you type them, and joining a public room does not replace them. Enter the room's IPv4 address: looking up a hostname does not work on the console yet.
- **Public rooms.** A list of the rooms in Eden's public directory, with each room's game, how many players it has and whether it asks for a password. Rooms you can join come first, the busiest at the top; Left and Right move a page at a time and Triangle refreshes. Rooms that use another protocol version are marked and cannot be joined.
- **For a stable multiplayer session**, everyone needs the same game and the same update of it, and the player who hosts the lobby should use 1x resolution with frame generation off. A host that cannot hold full speed ends the session for everyone.
- **Connection problems are explained.** When a room cannot be joined, a panel says what happened and what to try: a wrong password, a full room, a room that did not answer, a name already in use. **Settings > Diagnostics** has the detailed log to send with a report.
- **Self-update keeps the version it replaces.** After an update, the previous version's files stay in `/data/prosperoeden/previous-version/PPSA99008-backup` until the next update. See [Going back to the previous version](INSTALL.md#going-back-to-the-previous-version).
- **One place for your data.** Settings, saves and covers are only ever in `/data/prosperoeden`. When ProsperoEden could not get access to the console's storage at a start, it used to carry on with empty settings and no saves, which looked as if they had been lost; it now says so on the home screen and starts no game. Keys, firmware and games that an early version kept inside the app's folder are moved to `/data/prosperoeden` at the first start, so that replacing the app cannot remove them.
- **120 Hz holds on a display with a variable refresh rate.** With **Refresh rate** at 120 Hz, such a display was read as not having switched and the output went back to 60 Hz.
- **No more lost graphics device in shader-heavy scenes.** Long scenes could stop a game with `VK_ERROR_DEVICE_LOST`; draws are now handed to the GPU in smaller batches. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Eden updated** to its revision of 8 October (`67bada77`), together with the Vulkan driver (Mesa), its tools and the Payload SDK.
- **The helper that gives ProsperoEden access to storage** no longer reads a title's kernel data after that title has exited during its start.

## v1.000.100

- **Frame generation (new).** Between two pictures a game draws, ProsperoEden can show one made up from them, so a 30 FPS game looks like 60. It is in **Settings > Video** and in each game's own settings, works with the Vulkan renderer, and needs your own copy of Lossless Scaling: without its `Lossless.dll` the settings are greyed out. See [Frame generation](SETTINGS.md#frame-generation). Contributed by [gyn7561](https://github.com/gyn7561).
- **A new loading screen.** It lists the steps of a game's start (reading the game, starting graphics, building shaders, starting the game) with a progress bar; while shaders are built it counts them. A step that lasts gets its seconds shown, so a start that is stuck can be told from one that is slow.
- **Cheats in a file of their own work.** A cheat file placed straight in a game's mods folder (`cheat_<name>.txt`) was listed and switched on, but changed nothing in the game. It does now. The session log also names the game's build ID, the name a cheat file for that version of the game has.
- **A game no longer fails every start after a crash.** A crash, or a console that went down, could leave a game's shader cache file damaged; that game then crashed at every start on the same renderer. The damaged part is now cut off and the rest is kept.
- **No more stutter from log writes.** Games dropped frames every few seconds while the logs were written. Logs are now written in the background once a second; **Settings > Diagnostics > Write logs at once** brings the old behaviour back, to find a crash. Contributed by [matschi95](https://github.com/matschi95).
- **The FPS overlay with frame generation.** It shows the game's own rate (N) beside the rate on screen (F) while frames are being added, and its usual single figure otherwise. Contributed by [gyn7561](https://github.com/gyn7561).
- **The log says why frame generation is not running**, in lines that start with `framegen:`. Contributed by [gyn7561](https://github.com/gyn7561).
- **A Nix shell for building.** `shell.nix` sets up the whole build environment. Contributed by [Bauls Deep](https://github.com/bawsdeep).
- **Issues are open on GitHub**, with forms for bug reports and game reports; they ask for the save data and the debug logs. See [Issues and reports](../README.md#issues-and-reports).

## v1.000.095

A bug fix for v1.000.090. If you are on v1.000.090, update.

- **Stutter and crashes while playing are fixed.** In v1.000.090 every button press could make a game bring up its controller screen again: play stuttered, and some games crashed after a minute or two. A second, handheld controller was being connected beside the player's own at the first press. It is now only used when **Handheld** is the game's Controller type, or the only controller the game takes.

## v1.000.090

- **Download sources (new, early).** Games on a [RomM](https://github.com/rommapp/romm) server in your network appear in the Library and are downloaded to the console when you want to play them; see [Download sources](DOWNLOADS.md#download-sources). Contributed by [matschi95](https://github.com/matschi95).
- **Save sync (new, early).** Each profile can keep its save data in step with a RomM server, synced before a game starts and after it ends, and paired by scanning a QR code; see [Save sync](SAVES.md#save-sync). Replaced save data is kept as a backup first. Contributed by [matschi95](https://github.com/matschi95).
- **Controller type per game.** **Library > Game settings > Controls > Controller type** gives a game a Pro Controller, the Handheld controller, Dual Joy-Cons, a Left Joy-Con or a Right Joy-Con, for games that take a Pro Controller and then only work with another.
- **Typing text.** When a game asks for text, the PS5's own on-screen keyboard opens, and what you enter goes to the game.
- **Firmware 7.40 and 7.61: games no longer end at their first picture.** Contributed by [v0ltfault](https://github.com/v0ltfault).
- **Game names without question marks.** A name with a look-alike character (a sequel's Roman numeral, full-width letters) is shown with plain letters, a name that is not text is skipped, and characters no font has are left out.
- **System language in the start-up trace**, with the menu's language and the console fonts found, for reports from other consoles.
- **Builds on GitHub.** Releases are now built by GitHub Actions from the tagged source.

## v1.000.080

- **Single Joy-Con games with the DualSense held as usual.** A game that has a single Joy-Con held sideways no longer needs the DualSense turned sideways: the stick, the four buttons and the motion sensors are turned to match, for each player. L1 and R1 stay SL and SR.
- **What's new in the update dialog.** When an update is offered, **What's new** shows that release's notes before you decide.
- **Start a game directly.** ProsperoEden takes `--rom <file>` and `--exit-after-game` as launch arguments, so a home screen forwarder can open one game; see [docs/FORWARDER.md](FORWARDER.md).
- **Filesystem access on firmware 13.60.** The bundled helper is updated with a fix for that firmware.
- **Release ZIP opens with the right permissions.** Its files are stored open to all, as the console wants an app's files; unpacked with their permissions kept, they could give "Can't start the game or app" (CE-107750-0).

## v1.000.070

- **ProsperoEden starts wherever it is installed.** On a USB drive, extended storage, an etaHEN games folder or any other place ShadowMountPlus mounts it from, it could not find its own files and closed after a moment. It now reads its files from where the PS5 mounts the running app.
- **Updates from the menu work on those installs too.** The update helper updates the folder ShadowMountPlus mounted the app from; an app installed as an image is not changed, and the dialog says to replace the image.
- **Clearer error when the menu cannot start**, naming the file and the reason.
- **Start-up trace for bug reports.** Each start-up step is written to the kernel log (`[ProsperoEden diag]`) and to `/data/prosperoeden/logs/boot-trace.txt`.

## v1.000.060

- **Updates from the menu.** When a newer release is listed on homebrew.page, ProsperoEden offers it each time it opens. **Update now** downloads and checks it, unpacks it beside the app, then closes so the update helper can replace its files; **Skip** keeps the current version until the next time it opens. Your saves, settings and own files in the app folder are kept. See [Updating](INSTALL.md#updating).
- **Lapy elevation included.** ProsperoEden carries its own one-shot helper built from upstream Lapy and sends it over the local ELF loader on port 9021; loading Lapy separately is no longer needed.
- **The elevation helper no longer crashes when ProsperoEden starts.** Its start-up runs on one thread, and it is built from a corrected upstream release.

## v1.000.050

- **Profiles.** **Settings > Profiles** lists the people who play on the console, up to eight. Each profile keeps its own save data, its own settings (everything under Settings, and each game's own) and its own recently played games. See [Profiles](SETTINGS.md#profiles). If you update from an earlier version, your saves, settings and games played become the first profile's, with nothing to do.
- **Button mapping.** Choose which DualSense button presses each of the game's buttons, for every controller, in **Settings > Controls**, or for one game in its settings. See [Button mapping](SETTINGS.md#button-mapping).
- **Settings per game.** A game's settings now have Video, Performance, Audio, Controls and Language; each value follows Settings until you choose another. See [Settings per game](SETTINGS.md#settings-per-game).
- **Performance settings.** **Settings > Performance** has seven switches that trade accuracy for speed, for every game or for one. See [Performance settings](SETTINGS.md#performance-settings).
- **Cheats chosen one by one.** A mod that lists several cheats shows each under it with its own switch, and only the chosen ones run. See [Mods](MODS.md#mods).
- **Update notice.** When a newer release is listed on homebrew.page, the menu says so at the top right for ten seconds. Nothing is downloaded or installed. See [Updating](INSTALL.md#updating).
- **Leaving a game takes ten seconds at most.** If a game has not stopped ten seconds after Touchpad + L1, or after it ended by itself, ProsperoEden starts again and opens at the menu. Some games took minutes to close.
- **Touchpad + L1 works while a game loads.** A load that hangs can be left; before, the only way out was closing ProsperoEden.
- **A game's controller screen always gets an answer.** A game that takes only the handheld controller gets it in Docked mode too, and a game that names no controller gets a Pro Controller. Both used to leave the game waiting.
- **Removed games leave the menu.** A game whose file leaves the game files folder leaves Continue Playing, Recently Played and the Library within two seconds, and is not started.
- **Touchpad by its name.** The menu and this document say Touchpad + L1 and Touchpad + R1.
- **The graphics driver's shader cache works again**, so shaders compiled in earlier sessions load when a game starts.
- **A crash when a game with cheats stops is fixed.**
- **Games start a little sooner**: the instruction decoder's tables are built from one reading of each pattern.
- The graphics driver is built from Mihawk's current PS5 Mesa and Vulkan.

## v1.000.040

- **The launcher in your language.** It follows the language the PS5 is set to, in 29 languages. Arabic, Chinese, Greek, Japanese, Korean and Thai are drawn with the console's own fonts. Text that runs longer in another language makes its own room instead of being cut.
- **Accessibility.** **Settings > Accessibility** adds **Larger text**, **High contrast** and **Reduce motion**. Warnings carry a mark as well as a colour.
- **Save data in and out.** Import a game's save from a folder or from a Ryujinx data folder, and export a copy. See [Moving save data](SAVES.md#moving-save-data).
- **A ten-second slowdown with the AMD FSR filter is fixed.** In a large open-world game, gameplay could start at 3-9 FPS for about ten seconds, and frames of about a tenth of a second kept coming afterwards. The texture cache was throwing away images the GPU had drawn, with a wait for the GPU each time, as soon as memory use passed a mark that the FSR filter's own images pushed it over. It now keeps them until graphics memory is really short.
- **Smoother heavy scenes.** The renderer hands its work to the Vulkan worker in larger batches, and the emulated cores wait less on the GPU caches' locks. In the heaviest area of a test walk this removed drops to 26-28 FPS and cut the GPU thread's work by about a quarter.
- **Touchpad as Minus in games.** A tap of the touchpad presses the game's Minus button, and a longer press holds it. The Touchpad + L1 and Touchpad + R1 shortcuts never reach the game as a press.
- **A crash a few seconds into some games is fixed.** The motion sensors' updates could reach a part of the controller service that was not set up yet.
- **Games' own system screens.** A game's error dialog, profile picker and similar screens use Eden's built-in versions, and its error dialog now answers the game instead of leaving it waiting. The firmware's versions could end a session with an out-of-memory error.
- **More graphics memory.** The PS5 gives an app one pool of memory that the CPU and the GPU share, and ProsperoEden held about 3 GiB of it without using it: its heap took 3 GiB at start and the emulated console's page table 1 GiB. Both now take memory as they need it, which gives graphics about 2.6 GiB more in the largest game tested. That game ran out of graphics memory while loading at 2x; it now runs at 2x with the AMD FSR filter at a steady 30 FPS, with about 2 GiB to spare. The texture cache also measures its memory use from what is really left of the pool, instead of a driver figure that counted every allocation twice.
- **A clearer message when a game runs out of graphics memory.** If a game still needs more than there is at a high resolution, give it its own lower one: Triangle on it in the Library, then **Resolution**.
- **Games that accept only single Joy-Cons** now get one, and L1 and R1 are its SL and SR buttons, which those games ask for on their controller screen.
- **Mods.** A game's patches, replacement files and cheats load from `mods/<title ID>/` next to `roms/`, and the game's settings list them with a switch each. `.ips` and text `.pchtxt` patches, which the emulator core could not apply, now work. The Library has one switch per game for all of its mods. See [Mods](MODS.md#mods).
- **120 Hz output.** **Refresh rate** in **Settings > Video**, and in each game's settings, asks the display for 120 Hz while a game runs. A display that cannot show it stays at 60 Hz, and the menu always runs at 60 Hz. After a game at 120 Hz the menu takes about five seconds to come back while the display changes rate.
- **Output resolution.** **Settings > Video** has a new **Output resolution** row: 1080p (as before), 1440p or 2160p. It is the size of the picture ProsperoEden puts out, for the menu and for games. Until now a game's picture was made at 1080p and enlarged, whatever its internal resolution; at 2160p the upscaling filter scales the game straight to a 4K picture. A larger picture needs more graphics memory.
- **3x and 4x resolution.** The internal resolution now goes up to 4x. These need far more graphics memory than 2x; a game that runs out says so, and can be given its own lower resolution.
- **Patches for more frames than the display shows.** A game patched to run faster than the output refreshes keeps its pace: the frames the display has no refresh for are left out.
- **Crash reports.** If ProsperoEden stops because of an error, it writes `crash-<date>-<time>.txt` to `/data/prosperoeden/logs`, starts again and says on the home screen where the report is. That session's logs are kept beside it, and the five newest reports stay. A report holds what failed, where in the app's code, and what was running; it never holds the contents of memory.
- The launcher and the OpenGL renderer use the PS5 OpenGL 4.6 SDK 1.0.0.
- The graphics driver's shader cache moved to `/data/prosperoeden/cache`, so a read-only package install keeps it.
- **Experimental: block list.** With an empty file named `block-list.txt` in `/data/homebrew/PPSA99008`, ProsperoEden saves which code a 64-bit game compiled and compiles it again on a spare CPU when the game next starts. In a repeat session of a large open-world game, the emulated cores then compiled 426 blocks during play instead of 138,262, and gameplay started at 30 FPS instead of 22. It is off by default until more games have run with it.

## v1.000.030

- **New launcher.** Every screen is redrawn with OpenGL: smooth transitions between screens, text that stays sharp, panels that blur the artwork behind them, and sound effects for moving, selecting and going back. **Settings > Audio > Menu sounds** sets their level.
- **Controllers on the home screen.** Four controller icons show which controllers are connected, and change as one joins or leaves.
- **Loading screen.** An animated scene shows while a game starts, on both renderers.
- **The Library opens at once.** The game list is read in the background, and the home screen shows each game's own name instead of its file name.
- **New music** on the PS5 home screen.
- **Smoother first-time gameplay.** The emulated CPU cores now share the code they compile, so each part of a game is compiled once instead of once per core. In large open-world games this halves the compile work and removes most of the stutter when gameplay starts or a new area loads.
- **Faster compiling.** Compiling a game's code now takes about 40% less CPU time, which shortens the remaining stutter when gameplay starts or a new area loads.
- **Startup hang fixed.** A game could stop for good right after starting, because the emulator's memory allocator could leave high-priority threads waiting on each other forever. Development builds also report where a slow start is stuck.
- **Games close in about a second.** Touchpad + L1 used to take 10-25 seconds to return to the library; logging no longer waits on the console's storage, and the emulator skips needless teardown work.
- **Up to four controllers.** Each signed-in PS5 user's controller becomes the next player (player 1 is whoever launched the game); controllers can join or leave during a game, and games that ask for controllers connect every one in use.
- The Touchpad + L1 and Touchpad + R1 shortcuts work from any controller.
- **Vibration and motion.** DualSense rumble for games that use it (turn it off in **Settings > Controls**), and the controller's gyro and accelerometer for motion controls.
- **Resolution and upscaling.** **Settings > Video** now sets the internal rendering resolution (0.5x to 2x) and the filter that scales it to the TV: Bilinear, AMD FSR, Bicubic or Nearest.
- **Per-game settings.** Press Triangle on a game in the Library to give it its own renderer, resolution and upscaling filter.
- **Language setting.** **Settings > Language** picks the language games use (18 languages), with the console region that goes with it. It applies when a game starts.
- **Game updates and DLC.** Put update and DLC files (NSP or XCI) in the `updates` folder next to `roms`; the newest update and all DLC apply when the game starts, and the game details show them.
- **Build it yourself.** `make` fetches every dependency at its pinned revision and builds the release. The OpenGL renderer now uses the published PS5 OpenGL 4.6 SDK 0.6.0.

## v1.000.020

- **Vulkan is now the recommended and default renderer.** OpenGL remains available in Settings.
- **Choose where your game files live.** The new **Settings > Game files** browser selects any folder, including external drives and USB devices. The old `assets/` folder keeps working until you choose one.
- **App data moved to `/data/prosperoeden`.** Config, logs, covers, and saves now live outside the app folder, so updating the app never touches them.
- **Settings are now stored in one JSON file.** Earlier text settings are migrated automatically on first launch.
- **Faster Library navigation**, and covers and recent games that survive folder moves.
- **New icons** for controller actions, pages, and the Handheld / Docked mode.

**Known issues:** Ending a game with Touchpad + L1 can take several seconds. Some games can still hang on the loading screen, and some demanding games remain slow. This is a testing pre-release, not a compatibility guarantee.
