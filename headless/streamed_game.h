// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A game played where it lies on a download source, without downloading it first
// (Swordpdf/ProsperoEden#111, docs/DOWNLOADS.md): a game of a source whose entry in sources.json
// says "stream": true. The launcher hands back its path, "stream:<source>/<game>"
// (remote/remote.h, StreamPath), in place of a file in roms/. That path is no file: the session
// opens the game's file on its source (Open) before anything reads the game, keeps it here while
// the game runs and lets it go when the game ended (Close). Eden's GetGameFileFromPath
// (src/core/core.cpp, patched in headless/CMakeLists.txt), the metadata bridge
// (metadata_bridge.cpp) and the session (main.cpp) find the game's file here by that path.
//
// What the session does with a game's file otherwise does not apply to this one: it has no game
// folder in roms/ (no mods from one, and no updates or DLC from one; those in updates/ apply as to
// any game), it goes into no recent games, and its title ID is the one its source knows, when it
// knows one.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "core/file_sys/vfs/vfs_types.h"

namespace Eden::StreamedGame {

// How such a path begins (as remote/remote.cpp writes it).
inline constexpr std::string_view kScheme = "stream:";
inline bool IsPath(std::string_view path) { return path.starts_with(kScheme); }

struct Game {
    std::string path;           // "stream:<source>/<game>"
    std::string source;         // its source, as the menu names it
    std::string name;           // the game, as its source names it
    std::string file_name;      // its game file's name on the source ("Name [0100...][v0].nsp")
    std::uint64_t title_id = 0; // as its source knows it; 0 when it does not
    std::string cover;          // the cover the launcher keeps of it; empty without one
    FileSys::VirtualFile file;  // the game's file, read from the source
    // Why the game's reads fail, once one did because the source failed for good (English);
    // empty while they work.
    std::function<std::string()> failed;
    // The game runs (headless/main.cpp, as it starts it): from now on a read the source can no
    // longer answer waits for stop instead of handing the game what did not come, and lost is told
    // at once, from that read's thread (stream_vfs.h).
    std::function<void(std::function<void()> lost)> hold;
    // Ends every read, waiting or to come, at once with what is kept, and the transfer: the game is
    // being stopped, and a network that stalled must not hold it.
    std::function<void()> stop;
};

namespace Detail {
inline std::mutex& Lock() {
    static std::mutex lock;
    return lock;
}
inline std::shared_ptr<const Game>& Current() {
    static std::shared_ptr<const Game> current;
    return current;
}
} // namespace Detail

// The game played so under this path; null for another path.
inline std::shared_ptr<const Game> Find(std::string_view path) {
    if (!IsPath(path)) return nullptr;
    const std::lock_guard lock(Detail::Lock());
    const std::shared_ptr<const Game>& current = Detail::Current();
    return current != nullptr && current->path == path ? current : nullptr;
}
// Its file; null for another path.
inline FileSys::VirtualFile File(std::string_view path) {
    const auto game = Find(path);
    return game != nullptr ? game->file : nullptr;
}
// The game played so from now on (one at a time); null: none.
inline void Set(std::shared_ptr<const Game> game) {
    std::shared_ptr<const Game> before;
    {
        const std::lock_guard lock(Detail::Lock());
        before = std::exchange(Detail::Current(), std::move(game));
    }
    // The one before is let go here, without the lock: its file waits for its transfer to end.
}
// The game ended: it is let go here, and its file once Eden let go of it too.
inline void Close() { Set(nullptr); }

// Why such a game did not start or stopped, for the player: its source failed for good (reason:
// Game::failed). The session ends with it (headless/main.cpp), and does not start the game again.
inline std::string LostMessage(const Game& game, const std::string& reason) {
    return "The game's download source " + game.source + " stopped sending it (" + reason +
           "). Check the network and the server, then start the game again";
}

// Opens the game of such a path on its source for its session (remote/remote.h, OpenStream) and
// keeps it here: true, or false with *error, a sentence in English for the player. Only in builds
// with the launcher and the download sources (streamed_game.cpp).
bool Open(const std::string& path, std::string* error);

} // namespace Eden::StreamedGame
