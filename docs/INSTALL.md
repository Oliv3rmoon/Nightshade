# Game files, app data and updates

Where ProsperoEden looks for your keys, firmware and games, where it keeps its own data, and how it is updated. The install steps themselves are in the [README](../README.md#install).

## Game files folder

Only these subfolders matter; the folder itself can have any name and location.

```text
<game files folder>/                    # e.g. /data/prosperoeden, /mnt/ext1/eden, /mnt/usb0/eden
├── keys/
│   ├── prod.keys
│   └── title.keys                      # optional
├── firmware/
│   └── *.nca                           # extracted firmware NCAs
├── roms/
│   └── <Game>/                         # one folder per game
│       ├── <game>.nsp                  # the game (NSP or XCI, or NSZ or XCZ: see Compressed games)
│       ├── <game> update.nsp           # optional: its update and DLC files, beside it
│       ├── <game> DLC.nsp
│       ├── mods/                       # optional: its mods, one folder each (see Mods and cheats)
│       │   └── <mod name>/
│       │       ├── exefs/              # code patches: *.pchtxt, *.ips
│       │       ├── romfs/              # replacement game files
│       │       └── romfs_ext/          # files added to the game's own
│       ├── cheats/                     # optional: its cheats
│       │   └── <build ID>.txt          # one file per version of the game
│       └── reshade/                    # optional: its post-processing presets and effects
│           ├── <preset>.fxp            # offered in this game's settings
│           └── <effect>.fx             # and the .fxh files and Textures/ they use
├── save-import/                        # optional: saves to import, one folder per title ID
├── ryujinx/                            # optional: a Ryujinx data folder to import saves from
├── save-export/                        # written by "Export a copy"
├── wallpapers/                         # optional: images for Settings > Appearance > Wallpaper (WALLPAPERS.md)
└── .remote-downloads/                  # downloads that are not finished yet (see Download sources)
```

Each game sits in a folder of its own in `roms/`, with everything that belongs to it: its update and DLC files, its mods, its cheats and its post-processing presets. ProsperoEden tells the game from its update and DLC by what each file holds, not by its name, so the files can have any names. Folders inside a game folder are not searched for games, and folders whose names start with a dot are skipped. A game can also sit directly in `roms/` without a folder; its update and DLC then go in `updates/`, or beside it in `roms/` when their names hold their title IDs, as dumps are named (`[01008DD013200800][v196608]`): such a file applies to its game and is not listed as a game of its own. **Delete from console** also deletes the update and DLC files in the game's folder, and the folder once it is empty.

The folder browser shows how many keys, firmware files, and games each folder holds, so you can check a folder before selecting it. Moving your files later only needs a new selection in Settings; saved settings, covers, and recently played games carry over.

**Games on a drive of their own.** The games can be kept apart from the rest: `roms/` and `updates/` in one folder (a USB drive, say), keys, firmware, mods and cheats in the game files folder on the console. Name that folder as `"games_folder"` in `/data/prosperoeden/config/prosperoeden.json`, next to `"game_files"`:

```json
{ "game_files": "/data/prosperoeden", "games_folder": "/mnt/usb0/games" }
```

It applies from the next start, and the Library, the home page, downloads and forwarders then use its `roms/` and `updates/`; the GAMES and UPDATES lines in Settings > Game files and in About show where they are (the folder browser still counts each folder's own `roms/`). When the folder is not there at start (a drive not plugged in), the Library lists no games from it and downloads wait; the log says `games folder: Not found`. Without it, or when it does not name an absolute folder, the games are in the game files folder as before. Whatever the folders, the shader caches, saves and everything else written during play stay in `/data/prosperoeden` on the console (see App data).

### Compressed games (NSZ and XCZ)

Play your compressed library as it is: no converting, no extra copies of games that do not need one.

`NSZ` · `XCZ` · updates and DLC too · compressed with [nsz](https://github.com/nicoboss/nsz)

NSZ and XCZ files are NSPs and XCIs whose larger contents nsz compressed. Put them in `roms/` like any other game. How a game plays depends on how nsz compressed it:

| | **Block** (`nsz -B`) | **Solid** (`nsz -S`) |
|---|---|---|
| nsz's default for | XCZ | NSZ |
| First start | at once | unpacked once, which takes a while for a large game |
| Later starts | at once | at once, from the unpacked copy |
| Space on the console | the file itself | the file, plus the last played game uncompressed |

**Block compression** can be read anywhere in the file. These games play straight from the compressed file.

**Solid compression** can only be read from its start. The first time such a game starts, ProsperoEden unpacks its compressed contents once into `/data/prosperoeden/user/cache/nsz`. That needs about as much free space as the game uncompressed; `stderr.log` shows how far it is. Only the game you play last keeps its unpacked copy: unpacking another one removes it. Touchpad + L1 while it unpacks stops it, and it is done again the next time the game starts.

**In the Library,** a solid-compressed game or update is read at the end of its data when it is first looked at, which takes a few seconds per GB. What that finds is kept beside the unpacked copies (up to 16 MiB per game or update, usually about 5), so it is quick from then on.

**Short on space?** When the console's storage is too full to unpack a game, the log says so, and the game is read without unpacking instead: far too slow to play, so free some space first. To play a solid-compressed game without ever unpacking it, compress it again with block compression on a computer:

```text
nsz -D <game>.nsz
nsz -C -B <game>.nsp
```

The folder `/data/prosperoeden/user/cache/nsz` can be deleted whenever no game is running; it is made again as needed.

## App data

ProsperoEden keeps its own data in `/data/prosperoeden`, separately from the game files folder:

```text
/data/prosperoeden/
├── config/prosperoeden.json            # every profile's settings, and the game files folder
├── config/remote/                      # download sources and save sync, when you use them:
│   ├── sources.json                    #   the sources and how to reach them (written by you)
│   ├── queue.json                      #   the download queue
│   ├── <source>/catalog.json           #   each source's game list, kept for the next start
│   ├── save-sync.json                  #   where each profile syncs its save data (filled in by you)
│   └── save-sync/                      #   what the save sync keeps: the console as a device of
│                                       #   each profile's server, the games still to be synced
├── backup/save-sync/                   # save data the save sync replaced, the last three per game
├── covers/                             # cached game covers
├── logs/                               # current and previous session logs, crash reports
├── previous-version/PPSA99008-backup/  # the version the last self-update replaced
└── user/                               # every profile's saves, and emulator user data
    ├── cache/nsz/                      # solid-compressed NSZ games: the last one played, unpacked
    └── lossless/Lossless.dll           # optional: your copy, for frame generation
```

The app itself stays in `/data/homebrew/PPSA99008`; see [Updating](#updating).

ProsperoEden does not include keys, firmware, games, or other copyrighted console data. Dump these files from hardware and software you own. Do not download or redistribute them.

This is the only place it keeps them. If ProsperoEden cannot get access to the console's storage when it starts, the home screen says so and no game can be started; your data is untouched, and closing ProsperoEden and opening it again is the first thing to try. Earlier versions kept a second set of settings and saves in the app's own sandbox in that case, which made saves and settings seem to disappear on consoles where access came and went.

Keys, firmware and games that an early version kept inside the app's folder (`/data/homebrew/PPSA99008/assets`) are moved to `/data/prosperoeden` the first time this version starts, so that replacing the app cannot remove them.

## Backing up to a USB drive

**Settings > Diagnostics > Back up to USB** copies ProsperoEden's data to a USB drive in one go: the
settings, the profiles and every profile's save data, with the rest of `/data/prosperoeden` that the
app can't make again by itself. A PC reads the drive when it is formatted as exFAT or FAT32.

- **Before it starts** the dialog shows how much there is to copy, and a row for each USB drive
  connected (`/mnt/usb0` to `/mnt/usb7`). The PS5 doesn't tell an app how much room a drive has, so
  on the console a drive shows "Free space unknown": use one with more free space than the size
  shown. When the drive fills up, the backup stops and removes what it wrote. (Where the room is
  known, a backup that doesn't fit is refused before anything is written.)
- **Where it goes:** `<drive>/nightshade-backup/<YYYYMMDD-HHMMSS>/`, named after the console's date
  and time, with the same folders as `/data/prosperoeden` and two files of its own:
  `backup.json` (the app's version, the date, how many files and bytes, and what was left out) and
  `SHA256SUMS` (every file's SHA-256, as `sha256sum` writes them). In the backup's folder on a PC,
  `sha256sum -c SHA256SUMS` (Linux) or `shasum -a 256 -c SHA256SUMS` (macOS) checks the copy.
- **What is left out** is what the app makes again by itself, and the games: `cache/` (the
  driver's shader cache), `covers/`, `logs/`, `previous-version/`, and Eden's `user/cache/` (which
  also holds the unpacked NSZ games), `user/shader/`, `user/log/`, `user/crash_dumps/` and
  `user/dump/`; and `roms/`, `updates/` and `.remote-downloads/` (the downloads not finished yet)
  when the game files folder is `/data/prosperoeden`, with what the game folders in `roms/` hold
  (their mods, cheats and presets). Its `keys/`, `firmware/` and `mods/` are copied then. A game
  files folder elsewhere is not part of the backup.
- **A backup is whole, or its name says it isn't.** The files are copied one at a time into
  `<YYYYMMDD-HHMMSS>.incomplete`, which takes its name only once every file, `SHA256SUMS` and
  `backup.json` are on the drive. Circle cancels, and what was written is removed; so is a backup
  that fails. If it can't be removed (the drive was pulled out), the folder keeps `.incomplete` in
  its name: delete it. Nothing on the drive is written over, and a second backup in the same
  second is named `-2`. Leave the drive in until the dialog says the backup is done.

Nightshade writes what it found and did under "backup" in its log.

### Restoring a backup

Nightshade doesn't restore a backup itself, so nothing can replace your saves by accident. To put
one back by hand:

1. Close Nightshade.
2. Copy everything in the backup's folder except `backup.json` and `SHA256SUMS` into
   `/data/prosperoeden`, replacing the files there: over FTP from a PC, or from the USB drive with
   a file manager on the console. A folder whose name ends in `.incomplete` is not a whole backup.
3. Start Nightshade. What was left out (the shader caches, the covers) is made again as needed.

Files in `/data/prosperoeden` that the backup doesn't have stay as they are. To bring back one
game's save data only, copy just its folder,
`user/nand/user/save/0000000000000000/<profile ID>/<title ID>/`.

## Updating

Close ProsperoEden first. Your settings, saves, covers and logs are in `/data/prosperoeden`, outside the app, so an update keeps them. Afterwards the About screen shows the version that is running.

Once each time it opens, ProsperoEden asks [homebrew.page](https://homebrew.page/ps5) which release of it is listed there. The request carries the app's title ID and nothing else. If the listed release is newer than the one running, a dialog offers it, with its version and download size:

- **Update now** downloads the release ZIP from its GitHub release, checks it against the SHA-256 in the catalog's signed list, and unpacks it beside the app. A ring shows how far it is and the time left; Circle cancels, and nothing has changed until the end. Then ProsperoEden closes, the update helper (`self-updater.elf`, sent to the console's payload loader on port 9021) replaces the app's files, and the console shows a notification. Open ProsperoEden again to use the new version. The version it replaced is kept; see [Going back to the previous version](#going-back-to-the-previous-version). The helper replaces the files only once the app has closed: when its connection to the app has ended and the app's sandbox in `/mnt/sandbox` is gone, or has stayed 10 s longer, as the system can leave it until a restart. It writes what it did to `/data/prosperoeden/logs/updater.log`; send that file when an update did not take.
- **What's new** (when the release has notes) shows what changes in it before you decide: the release notes as homebrew.page lists them. Up/Down scroll, L1/R1 jump a page, Cross updates and Circle goes back to the offer. Triangle opens them from the offer too.
- **Skip** keeps the version you have. The dialog shows again the next time ProsperoEden opens, not when you come back from a game.

A build made with a GitHub repository set (`EDEN_UPDATE_REPOSITORY`, see [Building](BUILDING.md#make-targets)) asks that repository's newest release on GitHub instead (`api.github.com`; the request names the app's title ID too). The dialog, the notification and what follows here work the same, except that their texts say GitHub where they say homebrew.page, **What's new** shows the release's own notes, and **Update now** checks the ZIP against the SHA-256 in the release's own `SHA256SUMS` file. Nothing signs that answer, as the catalog signs its list: it is the same trust as downloading the release from its GitHub page by hand. Pre-releases are not offered. Nightshade builds ask [Oliv3rmoon/Nightshade](https://github.com/Oliv3rmoon/Nightshade/releases).

To stay on a version without being asked at every start, turn off **Settings > Diagnostics > Update notices** (`"system": { "update_notices": false }` in the settings file). ProsperoEden still asks homebrew.page, but neither the dialog nor the notification below shows; the About screen names the newer version beside the one running. It applies the next time ProsperoEden opens.

If the download or the unpacking fails, the dialog says why and offers **Try again**; the app stays as it was. Without a network, or without an answer, nothing is shown. When ProsperoEden cannot install the release itself (no payload loader, or an install it cannot find), the menu says **Update available** at the top right for ten seconds instead, and the steps below still work:

- **Folder install.** Copy the `PPSA99008` folder from the new release ZIP over `/data/homebrew/PPSA99008`, replacing the files it has, then start ProsperoEden. Files you put there yourself, such as `language.txt`, stay.

## Going back to the previous version

When ProsperoEden updates itself, the files of the version it replaces are kept in `/data/prosperoeden/previous-version/PPSA99008-backup`, until the next update replaces them in turn. If a new version misbehaves, you can put the previous one back:

1. Close ProsperoEden.
2. Copy everything inside `/data/prosperoeden/previous-version/PPSA99008-backup` over the app's folder (`/data/homebrew/PPSA99008`), replacing the files it has, for example over FTP.
3. Start ProsperoEden. The About screen shows the version that is running.

The update will be offered again the next time ProsperoEden opens; choose **Skip** to stay on the version you put back. Settings, saves and game files are not part of this: they stay as they are. An update done by hand, by copying a release over the app's folder, keeps no previous version.

## Nightshade's apps

**Settings > Apps** gets, updates and opens Nightshade TV (`PPSA99003`), Nightshade Stream
(`PPSA99002`) and Nightshade Station (`PPSA98510`), Nightshade's own release lines of
[ProsperoTV](https://github.com/ThatGuyKalvin/ProsperoTV),
[ProsperoLight](https://github.com/LedyBacer/ProsperoLight) and
[SwanStationPS5](https://github.com/darkxex/SwanStationPS5). The list is
`headless/prosperoeden/pe/ui/apps_list.hpp`; it names each repository where it is now.

**What is installed.** When Settings opens, Nightshade looks for each app: where ShadowMountPlus
recorded it mounted the app from (`/user/app/<title ID>/mount.lnk`), then the folders beside
Nightshade's own and in `/data/homebrew`, whatever their name, by the title ID in their
`sce_sys/param.json`; the version shown is that file's `contentVersion`. An app ShadowMountPlus
mounts from an image is shown, and opens, but is not updated here: replace the image. This needs
the filesystem access Nightshade asks for when it starts; without it the dialog says it can't tell
what is installed, offers **Get** for every app with a release and **Open** on Square.

**What is offered.** The first time the dialog opens in a session (and on Triangle), Nightshade asks
GitHub for the newest release of each app's Nightshade repository (`releases/latest`). While that
repository has no release, the newest release of the project the app is based on is offered
instead, and the row says "of the original project". That is one request to GitHub's API for an app,
two while it has no release of its own, of the 60 an hour GitHub allows an address without an
account; a repository GitHub says was renamed costs one more request for each move it sends the
request on, up to three. A renamed repository is followed only to the same account: one that moved
to another account is not offered, and the row says so, so nothing is installed from an account the
list does not name. The release must have the file `<title ID>.zip`, whose top folder
`<title ID>/` holds `eboot.bin` and `sce_sys/param.json`. Its SHA-256 is the one the release's
`SHA256SUMS` lists for it; a release without `SHA256SUMS` is checked against the SHA-256 GitHub
keeps of each file it holds. A release with neither, or whose two differ, is not offered, and the
row says why; a `SHA256SUMS` that can't be downloaded counts as GitHub not answering. A release is an update when its tag names a higher version than the one installed: a
tag such as `01.000.090` or `v1.005.100` is the version, and the ZIP must hold that version; `1.5.9`
is compared as `01.005.009`, and then the ZIP's own version is installed. A tag that names no
version is always offered.

**Get and Update.** The ZIP is downloaded from GitHub, checked against its SHA-256 as it arrives,
and streamed to the update helper (`self-updater.elf`, sent to the payload loader on port 9021, as
for [Nightshade's own updates](#updating)), which checks it again and unpacks it. Circle cancels
until the files are put in place; nothing changes before. A new app goes in a folder named after its
title ID beside Nightshade's (`/data/homebrew/PPSA99003` when Nightshade is in
`/data/homebrew/PPSA99008`); the helper refuses when a folder of that name is there already, or the
app is installed somewhere else. It never installs over the app the console is running, which it
asks the console (not `/mnt/sandbox`, where the system can keep a closed app's sandbox until a
restart, so an app you used before needs no restart to be updated), and when the console can't say,
it installs nothing. An update replaces the entries the release has in the app's folder
and keeps the rest, and the files it replaced are kept in
`/data/prosperoeden/previous-version/<title ID>-backup`, as for Nightshade, once the helper has told
Nightshade the update is in place. Nightshade stays open, and the console shows a notification too. A new app appears on the home screen after the homebrew
loader's (ShadowMountPlus's) next scan, or after a restart. The helper writes what it did to
`/data/prosperoeden/logs/updater.log`, which keeps the last update or install. To install an app by
hand instead, copy the `<title ID>` folder from its release ZIP to `/data/homebrew`.

**Open.** A PS5 app cannot start another one itself. Nightshade sends `app-launch.elf`, from its own
folder, to the payload loader on port 9021 and tells it which app to open, and waits up to five
seconds for its answer. Once it has taken the app, Nightshade closes, and the payload waits for it to
be gone (three looks in a row that find another app running, within 30 seconds) and starts the app
as the PS5's foreground user. It refuses an app the console doesn't list yet (a new one before the
homebrew loader's scan), and Nightshade then stays open and says so. With no payload loader
listening, Nightshade says that too: start the app from the home screen. The payload writes each
step to `/data/prosperoeden/logs/app-launch.log`. It is based on the Game Forwarder's launch payload
of Mihawk's [PS5 RetroArch](https://github.com/mihawk-99/PS5_RetroArch) (`forwarder/launch.c`).

Nightshade writes what it found and did under "apps" in its log.

## Upgrading from an earlier alpha

Earlier versions read everything from `/data/homebrew/PPSA99008/assets/`. That folder keeps working until you choose a game files folder, and settings are migrated automatically on first launch. To move to the new layout, move `assets/keys`, `assets/firmware` and `assets/roms` into any folder, then select it in **Settings > Game files**. The release ZIP contains no user files, so copy its app files over your installation without deleting your own data.
