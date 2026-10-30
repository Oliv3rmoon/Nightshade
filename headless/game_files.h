// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The game files ProsperoEden plays: NSP and XCI, and NSZ and XCZ, the same compressed by nsz
// (headless/ncz.h reads what they hold back as it was).
//
// And a game folder in roms/ that holds a game's ExeFS and RomFS as folders (roms/<game>/exefs/main
// with main.npdm beside it, roms/<game>/romfs/): how Switch 2 titles are played (experimental,
// eden-nx2, docs/NX2_PORT.md). Such a game goes by the path of its main, "<game>/exefs/main".
#pragma once
#include <cctype>
#include <string_view>

namespace Eden {
inline bool EndsWithNoCase(std::string_view name, std::string_view suffix) {
    if (name.size() < suffix.size()) return false;
    name.remove_prefix(name.size() - suffix.size());
    for (std::size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(name[i])) != suffix[i]) return false;
    return true;
}
// Read as an NSP: a package of NCAs.
inline bool IsPackageFile(std::string_view name) {
    return EndsWithNoCase(name, ".nsp") || EndsWithNoCase(name, ".nsz");
}
// Read as an XCI: a game card's image.
inline bool IsCardImageFile(std::string_view name) {
    return EndsWithNoCase(name, ".xci") || EndsWithNoCase(name, ".xcz");
}
inline bool IsGameFile(std::string_view name) { return IsPackageFile(name) || IsCardImageFile(name); }

// The main of a game folder's ExeFS: "<game>/exefs/main", or a full path ending so ("exefs" in any
// case; "main" as Eden names it). Its folder's name is the game's.
inline bool IsExeFsMain(std::string_view path) {
    if (!EndsWithNoCase(path, "/exefs/main")) return false;
    path.remove_suffix(std::string_view{"/exefs/main"}.size());
    return !path.empty() && path.back() != '/';
}
// The game folder of an ExeFS main ("<game>" of "<game>/exefs/main"; its full path for a full path).
inline std::string_view ExeFsGameFolder(std::string_view path) {
    if (!IsExeFsMain(path)) return {};
    path.remove_suffix(std::string_view{"/exefs/main"}.size());
    return path;
}
// What the Library plays: a game file, or a game folder's ExeFS main.
inline bool IsPlayablePath(std::string_view path) { return IsGameFile(path) || IsExeFsMain(path); }
} // namespace Eden
