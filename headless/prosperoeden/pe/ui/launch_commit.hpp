// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe/ui/services.hpp"

namespace pe::ui {
// What playing a game starts: its file on the console; for one on download sources only, the
// path it is played from where it lies when a source of it sends it so (Game::stream); else
// nothing, and it is downloaded first.
inline std::string play_file(const Game& game) { return game.remote ? game.stream : game.file; }
inline bool launch_available(Services& services, const std::string& file) {
    return services.game_exists(file) && services.can_launch_game();
}
// Both launchers resolve paths through the same backend and register play exactly once.
inline std::string commit_launch(Services& services, const std::string& file) {
    if (!launch_available(services, file)) return {};
    const auto path = services.game_path(file);
    if (path.empty()) return {};
    services.will_play(file);
    return path;
}
}
