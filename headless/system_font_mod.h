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
#if defined(__PROSPERO__)
#include "native_directory.h"
#endif

namespace Eden::SystemFontMod {

// What a shared font file starts with (the console's encrypted BFTTF header, read big-endian), and
// the memory all the shared fonts share (Eden's SHARED_FONT_MEM_SIZE).
inline constexpr std::uint8_t kMagic[] = {0x36, 0xf8, 0x1a, 0x1e};
inline constexpr std::size_t kSharedFontMemory = 0x1100000;

inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// A folder's entries. On the console its own reader lists them (native_directory.h), as mods.h does.
inline std::vector<std::filesystem::directory_entry> List(const std::filesystem::path& folder) {
    std::error_code error;
#if defined(__PROSPERO__)
    auto entries = Eden::ReadNativeDirectory(folder, error);
    if (error) entries.clear();
    return entries;
#else
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator it{folder, error}, end; !error && it != end; it.increment(error))
        entries.push_back(*it);
    return entries;
#endif
}

inline bool IsFolder(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error) && !error;
}

// The entry of folder named name in any letter case, or empty. Folders only when want_folder.
inline std::filesystem::path Child(const std::filesystem::path& folder, const std::string& name, bool want_folder) {
    const std::string wanted = Lower(name);
    for (const auto& entry : List(folder)) {
        if (Lower(entry.path().filename().string()) != wanted) continue;
        std::error_code error;
        if (want_folder ? IsFolder(entry) : entry.is_regular_file(error) && !error) return entry.path();
    }
    return {};
}

// The bytes of the mod's file for this archive, or none. mods: the game files folder's mods/.
// rejected: why a file that was there was left out (not a shared font file, or too large).
inline std::vector<std::uint8_t> Read(const std::filesystem::path& mods, std::uint64_t title_id, const std::string& file,
                                      std::string* rejected = nullptr) {
    char id[17];
    std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(title_id));
    const auto title = Child(mods, id, true);
    if (title.empty()) return {};
    std::vector<std::filesystem::path> romfs_folders;
    if (const auto own = Child(title, "romfs", true); !own.empty()) romfs_folders.push_back(own);
    std::vector<std::filesystem::path> named;
    for (const auto& entry : List(title))
        if (IsFolder(entry) && Lower(entry.path().filename().string()) != "romfs") named.push_back(entry.path());
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
        const char* why = in.bad() ? "it could not be read"
            : bytes.size() < 8 || bytes.size() % 4 != 0 || !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())
                ? "it is not a shared font file (.bfttf)"
            : bytes.size() >= kSharedFontMemory ? "it is larger than the shared fonts' memory" : nullptr;
        if (why == nullptr) return bytes;
        if (rejected) *rejected = path.string() + ": " + why;
    }
    return {};
}

} // namespace Eden::SystemFontMod
