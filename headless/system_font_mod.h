// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A font a mod puts in place of one of the console's shared fonts: a translation mod brings its
// own standard font for letters the console's has none of (Vietnamese in Link's Awakening,
// Swordpdf/ProsperoEden#248). Atmosphere takes it from contents/0100000000000811/romfs/; here it
// is the same folder in mods/: mods/<title ID>/romfs/<file>, or mods/<title ID>/<mod>/romfs/<file>
// (the first mod by name that has it). Folder and file names may be in any letter case. Eden's
// font loader asks for each of the shared fonts (patched in headless/CMakeLists.txt).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace Eden::SystemFontMod {

inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// The entry of folder named name in any letter case, or empty. Folders only when folder is true.
inline std::filesystem::path Child(const std::filesystem::path& folder, const std::string& name, bool want_folder) {
    std::error_code error;
    const std::string wanted = Lower(name);
    for (std::filesystem::directory_iterator it{folder, error}, end; !error && it != end; it.increment(error)) {
        const bool is_folder = it->is_directory(error);
        if (is_folder == want_folder && (want_folder || it->is_regular_file(error)) &&
            Lower(it->path().filename().string()) == wanted)
            return it->path();
    }
    return {};
}

// The bytes of the mod's file for this archive, or none. mods: the game files folder's mods/.
inline std::vector<std::uint8_t> Read(const std::filesystem::path& mods, std::uint64_t title_id, const std::string& file) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
    const auto title = Child(mods, id, true);
    if (title.empty()) return {};
    std::vector<std::filesystem::path> romfs_folders;
    if (const auto own = Child(title, "romfs", true); !own.empty()) romfs_folders.push_back(own);
    std::vector<std::filesystem::path> named;
    std::error_code error;
    for (std::filesystem::directory_iterator it{title, error}, end; !error && it != end; it.increment(error))
        if (it->is_directory(error) && Lower(it->path().filename().string()) != "romfs") named.push_back(it->path());
    std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) {
        return Lower(a.filename().string()) < Lower(b.filename().string());
    });
    for (const auto& mod : named)
        if (const auto romfs = Child(mod, "romfs", true); !romfs.empty()) romfs_folders.push_back(romfs);
    for (const auto& romfs : romfs_folders) {
        const auto path = Child(romfs, file, false);
        if (path.empty()) continue;
        std::ifstream in(path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad() || bytes.size() < 8 || bytes.size() % 4 != 0) continue;  // not a shared font file
        return bytes;
    }
    return {};
}

} // namespace Eden::SystemFontMod
