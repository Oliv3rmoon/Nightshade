// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The game files ProsperoEden plays: NSP and XCI, and NSZ and XCZ, the same compressed by nsz
// (headless/ncz.h reads what they hold back as it was). Beside them the Library lists homebrew (an
// NRO) and, as advanced files, a program on its own as Eden's loaders take one: an NSO, a Program
// NCA, the same NCA encrypted for an SD card (NAX), a KIP, and an unpacked ExeFS chosen by its
// "main". Whether one of those runs depends on what it needs beside it (docs/INSTALL.md).
#pragma once
#include <cctype>
#include <string>
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
// A game with its title, update and DLC: an NSP or an XCI.
inline bool IsGameFile(std::string_view name) { return IsPackageFile(name) || IsCardImageFile(name); }

// Homebrew: an NRO, with its name and icon in its assets (ASET) when it has them.
inline bool IsHomebrewFile(std::string_view name) { return EndsWithNoCase(name, ".nro"); }
// A program on its own (advanced): an NSO module, a Program NCA, a NAX (an NCA as an SD card keeps
// it, encrypted), or a KIP.
inline bool IsProgramFile(std::string_view name) {
    return EndsWithNoCase(name, ".nso") || EndsWithNoCase(name, ".nca") || EndsWithNoCase(name, ".nax") ||
           EndsWithNoCase(name, ".kip");
}
// An unpacked ExeFS (main, main.npdm and the modules beside them) as the Library names it: by its
// main, in a folder of roms/ ("<folder>/main"). The folder itself must hold main.npdm too.
inline bool IsExeFsMain(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash != std::string_view::npos && slash > 0 && path.substr(slash + 1) == "main";
}
// What the Library lists: games, homebrew and programs.
inline bool IsLibraryFile(std::string_view name) {
    return IsGameFile(name) || IsHomebrewFile(name) || IsProgramFile(name) || IsExeFsMain(name);
}
// The advanced ones: a program alone, which runs only with what it needs beside it.
inline bool IsAdvancedFile(std::string_view name) { return IsProgramFile(name) || IsExeFsMain(name); }
// The format the Library shows for a file: its extension in capitals, "EXEFS" for an unpacked
// ExeFS.
inline std::string LibraryFormat(std::string_view name) {
    if (IsExeFsMain(name)) return "EXEFS";
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || name.find('/', dot) != std::string_view::npos) return {};
    std::string format{name.substr(dot + 1)};
    for (char& c : format) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return format;
}
} // namespace Eden
