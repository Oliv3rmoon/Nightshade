// SPDX-License-Identifier: GPL-3.0-or-later
// A game's mods as Eden's patch manager reads them: one folder that holds the mods of the game's
// own folder in roms/, those of mods/<title ID>/, and the cheats/ folders' cheat files as one more
// mod (mods.h has the layout and the rules; the launcher lists the same with Mods::List). The
// derived bis_factory.cpp returns this for the game's mod folder (headless/CMakeLists.txt).
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "core/file_sys/vfs/vfs_layered.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "assets_dir.h"
#include "mods.h"

namespace Eden::GameFolderMods {

// The romfs/ and exefs/ put straight into the mods folders (Mods::kUnnamedMod), as one more
// layer: <name>/<kUnnamedMod>/romfs/... Each part is the same part of every folder, the first
// folder's files first, whatever the case of its name in each (RomFS/ and romfs/ are one).
inline void AddUnnamed(std::vector<FileSys::VirtualDir>& layers, const std::string& name) {
    std::vector<std::pair<std::string, std::vector<FileSys::VirtualDir>>> parts;
    for (const auto& layer : layers)
        for (auto& part : layer->GetSubdirectories()) {
            if (!Mods::IsModPart(part->GetName())) continue;
            const std::string key = Mods::Lower(part->GetName());
            auto it = std::find_if(parts.begin(), parts.end(), [&](const auto& known) { return known.first == key; });
            if (it == parts.end()) it = parts.insert(parts.end(), {key, {}});
            it->second.push_back(std::move(part));
        }
    if (parts.empty()) return;
    std::vector<FileSys::VirtualDir> merged;
    for (auto& [key, folders] : parts)
        merged.push_back(FileSys::LayeredVfsDirectory::MakeLayeredDirectory(std::move(folders), key));
    auto mod = std::make_shared<FileSys::VectorVfsDirectory>(std::vector<FileSys::VirtualFile>{}, std::move(merged),
                                                             std::string{Mods::kUnnamedMod});
    layers.push_back(std::make_shared<FileSys::VectorVfsDirectory>(
        std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{std::move(mod)}, name));
}

// shared: the game's folder in mods/, or null.
inline FileSys::VirtualDir Root(FileSys::VirtualDir shared, std::uint64_t title_id, std::string name) {
    // The game folder's mods and cheats for the game's own title only (Mods::GameFolderFor).
    const Mods::Places places{Eden::AssetsPath("mods"), Eden::AssetsPath("cheats"), Mods::GameFolderFor(title_id)};
    // Its files keep a reference to the filesystem they came from: one for the whole run.
    static FileSys::RealVfsFilesystem vfs;
    std::vector<FileSys::VirtualDir> layers;
    if (!places.game_folder.empty())
        for (const auto& entry : Mods::ListFolder(places.game_folder))
            if (Mods::IsFolder(entry) && Mods::Lower(entry.path().filename().string()) == "mods")
                if (auto mods = vfs.OpenDirectory(entry.path().string(), FileSys::OpenMode::Read)) layers.push_back(mods);
    if (shared) layers.push_back(std::move(shared));
    AddUnnamed(layers, name);
    std::vector<FileSys::VirtualFile> files;
    for (const auto& path : Mods::CheatFolderFiles(places, title_id))
        if (auto file = vfs.OpenFile(path.string(), FileSys::OpenMode::Read)) files.push_back(std::move(file));
    if (!files.empty()) {
        // <root>/<kCheatsFolder>/cheats/<build ID>.txt, as a mod with a cheats/ folder.
        auto cheats = std::make_shared<FileSys::VectorVfsDirectory>(std::move(files), std::vector<FileSys::VirtualDir>{},
                                                                    "cheats");
        auto mod = std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{std::move(cheats)},
            std::string{Mods::kCheatsFolder});
        layers.push_back(std::make_shared<FileSys::VectorVfsDirectory>(
            std::vector<FileSys::VirtualFile>{}, std::vector<FileSys::VirtualDir>{std::move(mod)}, name));
    }
    if (layers.empty()) return nullptr;
    if (layers.size() == 1) return layers.front();
    return FileSys::LayeredVfsDirectory::MakeLayeredDirectory(std::move(layers), std::move(name));
}

} // namespace Eden::GameFolderMods
