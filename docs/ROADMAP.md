# Roadmap

What is planned for Nightshade, and where each piece stands. Each item, or each part of one, carries a status:

- **Built** - in the code and passing its tests on a computer. Not yet confirmed on a PS5.
- **In progress** - being designed or written now.
- **Planned** - decided, not started.
- **Investigating** - it first has to be found out whether, or how, it can be done.

Nothing on this page counts as working until a tester has run it on a console. Nightshade runs Switch games today. Switch 2 support is in experimental test builds, and no Switch 2 game has run on a PS5 yet (see the [README](../README.md)). Every other system on this page is in development or planned.

## Nightshade 2: one library for every system

### Library and launcher

- **Navigation that does what the thumb did** - one push of the stick is one step, even when the stick rests right at the edge of a press, and a held direction repeats on a fixed beat that a slow frame cannot rush or drag. The D-pad and L1/R1 keep the same beat. *Built.*
- **Menu sounds that keep up** - moving fast restarts a tick with a short fade instead of stacking copies of it, a sound that has to make room for another fades out instead of clicking, and a sound asked for twice in one frame plays once. *Built.*
- **Smoother motion** - movement with its own weight for each kind of element, timed to the display's refresh so it does not judder. Reduce motion will turn all of it down. *Planned.*
- **Cover art that appears by itself** - optional, and off until you turn it on. Covers are matched to the game itself, not to what its file happens to be called, fetched over an encrypted connection and size-checked before they are opened. A cover you chose yourself will always win over a fetched one. *Planned.*
- **Game cases in 3D** - the case in true perspective with its own spine, on the shelf and in a game's details. *Planned.*
- **Collections** - your own named groups of games across every system, next to favourites and recents. *Planned.*
- **Other launchers** - a different launcher can be installed as a pack, with the built-in one always kept as the way back. *Built, and in releases as an experimental feature since v1.003.* A ready-made alternative front end offered as a download from inside the app, checked against a published checksum and resumable if the connection drops. *Investigating.*
- **Controls shown on the controller** - button mapping for every system drawn on a DualSense of Nightshade's own, in the theme's colours. *Planned.*
- **System icons** - in the library, systems are named in plain text today. A set of simple hardware outlines is being considered; no console maker's logo will be used. *Investigating.*
- **Every line in your language** - a release build stops when a launcher line is untranslated. *Built.* New screens, the in-game menu included, translated before they ship, and a check that stops a build that mixes spellings. *Planned.*

### Setup and settings

- **A setup check for every system** - one screen that says what each system still needs and names the exact file and folder. It checks that a key or firmware file is the right one, not just that something is there, and a missing Switch key never holds up another system. *Planned.*
- **Folders and defaults for every system** - choose each system's games folder in Settings, and set defaults per system with per-game changes on top that can always be reset. *Planned.* Per-game core settings before launch. *Built.*

### Playing

- **One pause menu on every system** - the same menu on the same button (Touchpad + L1), with save states and game settings. *Built for the classic systems; planned for Switch.* Showing whether the game has saved and whether it is safe to quit, and button remapping inside the menu. *Planned.*
- **Core options** - every option a core offers, stored per game. *Built.* Defaults per system, and changes that take effect without restarting the game where the core allows it. *Planned.*
- **Achievements** - RetroAchievements on the classic systems. Progress is kept inside save states instead of being reset by loading one, the sign-in is stored safely, and hardcore mode switches off save states and cheats. *Planned.*
- **Picture for the classics** - whole-number scaling, a CRT look and a handheld LCD look, each adjustable instead of fixed, set per system and changeable per game. *Planned.*
- **NES HD packs** - high-resolution graphics packs for NES games, loaded from a folder beside the game. *Planned.*
- **Game Boy, Game Boy Color and Game Boy Advance** - a fault that could stop these games at start-up on a PS5 is fixed, and the engine no longer looks for files outside its own folders. *Built.* These systems stay switched off in release builds until a console confirms them, and will be labelled in development once systems carry a status in the app.
- **Nintendo 64** - a build fault that would have left the engine's memory unallocated on a PS5 is fixed, and the build now stops if it ever comes back. *Built.* The engine runs on a computer only so far. *In progress.*
- **Switch 2** - Switch 2 games to be started from the same library through a second runtime, [EglantinePS5](https://github.com/heydemoura/EglantinePS5) by Heyde Moura, with Nightshade's own loading screen and performance overlay. Off by default and labelled in development. A game that fails returns to the library and is never restarted on its own, and no game is listed as running until a console has run it. *In progress.*
- **GameCube, Wii, Wii U and 3DS** - each as its own program that Nightshade starts and returns from, one system at a time. *Planned.*

### Trust

- **A compatibility list that fills itself** - an optional report when a game ends. You see each report before it is sent and the answer under the cursor is "Not now". A report carries nothing that ties your sessions together, games are identified by their contents and never by file or folder names, and the list shows how many reports stand behind each result, so a single report is never presented as proof. *Planned.*
- **Builds that check themselves** - a package is refused when its program image is more than twice the size of the largest Nightshade build known to have run on a console (a cautious limit, not a measured PS5 one) or when its files are incomplete, and an engine is refused at build time when it was linked in a way that would break it silently. *Built.*
- **A status on every system** - every system in the library says whether it runs on PS5, is in development or is planned, in the app itself. *Planned.*

## Switch

These items carry over from the earlier roadmap and have not been given a status yet.

- **FPKG support** - install Nightshade as a fake package, alongside the current homebrew folder install.
- **More performance** - CPU and GPU work to keep demanding games at their target frame rate: the short stutter when a game reaches new areas (the block list, **Compile ahead** in **Settings > Performance**, once it has run in more games and can be on by default), heavy cutscenes, and games that run slower in Docked mode than in Handheld.
- **More reliable game loading** - a game that hangs while it loads can now be left with Touchpad + L1; the hangs themselves still need a log from a game that does it.
- **Game names in the menu's language** - a game carries a name for each language; the Library shows the first one that is text. Show the one in the menu's language, and the name from an installed update when it has one.
- **Real Joy-Cons and Pro Controllers (investigation)** - play with the original controllers connected to the PS5. The PS5 does not pair them itself, so this first needs to find out whether a homebrew app can read them: a wired Pro Controller over USB looks more likely than Joy-Cons, which only connect over Bluetooth.
- **Keyboard as a controller** - map a USB keyboard's keys to the game's buttons, like the DualSense's. Text entry itself is done: it uses the PS5's on-screen keyboard.
- **Multiplayer with Eden on other systems (investigation)** - join Eden's multiplayer rooms from the PS5 and play with Eden players on PC, Linux and Android, entering a room's address by hand, including a room hosted on your local network (no PSN needed). Rooms only accept the same Eden version on every side, so each release would name the matching PC version.
- **DualShock 4 controllers** - use DualShock 4 controllers as players too, next to the DualSense, so multiplayer games do not need four DualSense controllers.
- **Controller type for each player** - choose, for each of the four connected controllers, which controller the game sees: a Pro Controller, a pair of Joy-Cons, a single left or right Joy-Con, a GameCube controller, or Handheld mode, as Eden does on other systems.
- **Better OpenGL performance** - make the OpenGL renderer faster, and add tuning options for it.
- **More game compatibility** - validate more games on the PS5, and fix what keeps them from running well, such as games that crash at launch.
