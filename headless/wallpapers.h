// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The images Settings > Appearance > Wallpaper lists (docs/WALLPAPERS.md), from two folders:
// wallpapers/ in the game files folder, the folder with keys/, firmware/ and roms/ that a player
// who runs the app from a USB drive copies over from a PC (the "assets" folder of upstream
// request 263), and /data/prosperoeden/wallpapers, filled over FTP. With the game files folder
// left at /data/prosperoeden (the default) the two are one folder.
//
// The settings keep a wallpaper by its file name alone (settings_store.h, ValidWallpaperName), so
// a name is one entry however many folders have it: the game files folder's copy, so that an
// image replaced from a PC is the one shown. Nothing here touches the console; checked on the
// host (wallpapers_check.cpp).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Eden::Wallpapers {

// The images' folder in a folder: its wallpapers/.
inline std::string In(std::string folder) {
    while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    return folder == "/" ? "/wallpapers" : folder + "/wallpapers";
}

// The folders, in the order a name is looked for: the game files folder's wallpapers/, then the
// data folder's (storage_paths.h, WallpapersDir). One when they are the same folder.
inline std::vector<std::string> Folders(const std::string& game_files, std::string data_wallpapers) {
    while (data_wallpapers.size() > 1 && data_wallpapers.back() == '/') data_wallpapers.pop_back();
    std::vector<std::string> folders;
    if (!game_files.empty()) folders.push_back(In(game_files));
    if (!data_wallpapers.empty() && (folders.empty() || folders.front() != data_wallpapers))
        folders.push_back(data_wallpapers);
    return folders;
}

// An image the dialog lists, and the folder it is read from.
struct Image {
    std::string name;
    std::string folder;
};

// The file names in a folder (regular files only); false when it can't be read (a drive not
// plugged in, a folder nobody made): that folder then adds nothing.
using Lister = std::function<bool(const std::string& folder, std::vector<std::string>* files)>;

// Every file name of the folders, each once (from the first folder that has it), sorted without
// regard to case as the console lists a folder (eden_services.cpp, ListEntries).
inline std::vector<Image> List(const std::vector<std::string>& folders, const Lister& files) {
    std::vector<Image> images;
    std::set<std::string> seen;
    for (const std::string& folder : folders) {
        std::vector<std::string> names;
        if (!files(folder, &names)) continue;
        for (std::string& name : names)
            if (!name.empty() && seen.insert(name).second) images.push_back({std::move(name), folder});
    }
    const auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::stable_sort(images.begin(), images.end(), [&](const Image& a, const Image& b) {
        const std::string left = lower(a.name), right = lower(b.name);
        return left != right ? left < right : a.name < b.name;
    });
    return images;
}

inline std::vector<std::string> Names(const std::vector<Image>& images) {
    std::vector<std::string> names;
    names.reserve(images.size());
    for (const Image& image : images) names.push_back(image.name);
    return names;
}

// The file a chosen name is read from: the first folder that has it; empty when none does.
inline std::string Find(const std::vector<std::string>& folders, const std::string& name,
                        const std::function<bool(const std::string& path)>& exists) {
    if (name.empty()) return {};
    for (const std::string& folder : folders) {
        const std::string path = folder + "/" + name;
        if (exists(path)) return path;
    }
    return {};
}

} // namespace Eden::Wallpapers
