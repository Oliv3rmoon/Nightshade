// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// An update or DLC file left directly in roms/ beside its game, where players put them: it applies
// to its game and is not listed as a game of its own. Eden reads updates/ and a game's own folder
// only, so such a file was never applied (a tester's Ori and the Will of the Wisps started as 1.0.0
// without its 1.2.1 update and stopped on its loading screen). In roms/ such a file is told by its name,
// which holds its title ID as dumps are named ("[01008DD013200800][v196608]"): a game's has its low
// 13 bits clear, its update's and its DLC's do not, and both have the game's once those are cleared
// (Eden's FileSys::GetBaseTitleID). A file named otherwise is a game, as before; updates/ and a
// game's own folder take any name.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

namespace Eden::LooseAddOns {

inline constexpr std::uint64_t kBaseMask = 0xFFFFFFFFFFFFE000;

// The title IDs in a file name: each run of exactly 16 hexadecimal digits.
inline std::vector<std::uint64_t> TitleIds(std::string_view name) {
    const auto hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::vector<std::uint64_t> ids;
    for (std::size_t i = 0; i < name.size();) {
        std::size_t end = i;
        while (end < name.size() && hex(name[end])) ++end;
        if (end - i == 16) {
            std::uint64_t id = 0;
            for (std::size_t k = i; k < end; ++k) {
                const char c = name[k];
                id = id << 4 | static_cast<std::uint64_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            ids.push_back(id);
        }
        i = end == i ? i + 1 : end;
    }
    return ids;
}

// Whether the name says an update or DLC: base = 0 for any game's, else for that game's.
inline bool AddOn(std::string_view name, std::uint64_t base = 0) {
    for (const std::uint64_t id : TitleIds(name))
        if ((id & ~kBaseMask) != 0 && (base == 0 || (id & kBaseMask) == (base & kBaseMask))) return true;
    return false;
}

inline bool PackageName(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string extension{name.substr(dot + 1)};
    for (char& c : extension) c = static_cast<char>(c | 0x20);
    return extension == "nsp" || extension == "xci" || extension == "nsz" || extension == "xcz";
}

// The update and DLC files of the game `title_id` directly in roms (not in its folders), full paths.
// On the console its own reader lists the folder (native_directory.h), as mods.h does.
inline std::vector<std::string> FilesOf(const std::filesystem::path& roms, std::uint64_t title_id) {
    std::vector<std::string> files;
    if (title_id == 0) return files;
    std::error_code error;
#if defined(__PROSPERO__)
    auto entries = Eden::ReadNativeDirectory(roms, error);
    if (error) return files;
#else
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator it{roms, error}, end; !error && it != end; it.increment(error))
        entries.push_back(*it);
#endif
    for (const auto& entry : entries) {
        std::error_code kind;
        const std::string name = entry.path().filename().string();
        if (entry.is_regular_file(kind) && !kind && PackageName(name) && AddOn(name, title_id))
            files.push_back(entry.path().string());
    }
    return files;
}

// The session's own: main.cpp sets them before the game's contents are read, and Eden's
// filesystem service gives them to its external content provider with updates/ (CMakeLists.txt).
inline std::vector<std::string>& SessionFiles() {
    static std::vector<std::string> files;
    return files;
}

} // namespace Eden::LooseAddOns
