// SPDX-License-Identifier: GPL-3.0-or-later
// A home screen forwarder's launch arguments (docs/FORWARDER.md): what main.cpp reads, and the
// arguments that start the same game in a new process. The bridge (headless/bridge) passes them on
// to PPSA64448 when an old forwarder opened PPSA99008, and Nightshade passes them to itself when it
// starts again after the move from ProsperoEden's data folder (migrate.h). No other headers: the
// bridge has no emulator.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace Eden {

struct ForwardedArgs {
    std::string rom;
    bool exit_after_game = false;
};

inline ForwardedArgs ParseForwardedArgs(int argc, char** argv) {
    ForwardedArgs result;
    if (argc <= 0 || argv == nullptr) return result;
    constexpr std::string_view rom_flag = "--rom";
    constexpr std::string_view rom_prefix = "--rom=";
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr) break;
        const std::string_view arg{argv[i]};
        if (arg == "--exit-after-game") {
            result.exit_after_game = true;
        } else if (arg == rom_flag) {
            if (i + 1 < argc && argv[i + 1] != nullptr) result.rom = argv[++i];
        } else if (arg.starts_with(rom_prefix)) {
            result.rom = std::string{arg.substr(rom_prefix.size())};
        }
    }
    return result;
}

// The launch arguments that start the forwarded game again in a new process, for a launch call's
// list: "--from=<who>" first, then "--rom" and the game, and "--exit-after-game" when it was
// given. Whether the system hands a list on as it is or takes its first entry for the program's
// name, the first one is only a note (ParseForwardedArgs skips what it doesn't know) and the game
// is read all the same. Empty when no game was forwarded: nothing to pass on.
inline std::vector<std::string> ForwardedArgv(const ForwardedArgs& args, std::string_view from) {
    if (args.rom.empty()) return {};
    std::vector<std::string> out{"--from=" + std::string{from}, "--rom", args.rom};
    if (args.exit_after_game) out.emplace_back("--exit-after-game");
    return out;
}

// A path inside `from` named inside `to` (a game file of ProsperoEden's data folder after it was
// moved to Nightshade's): /data/prosperoeden/roms/Game.nsp is /data/nightshade/roms/Game.nsp.
// Empty when the path is not inside `from`.
inline std::string RebasedPath(std::string_view path, std::string_view from, std::string_view to) {
    while (from.size() > 1 && from.back() == '/') from.remove_suffix(1);
    while (to.size() > 1 && to.back() == '/') to.remove_suffix(1);
    if (from.empty() || to.empty() || path.size() <= from.size() + 1 || !path.starts_with(from) ||
        path[from.size()] != '/')
        return {};
    return std::string{to} + std::string{path.substr(from.size())};
}

}
