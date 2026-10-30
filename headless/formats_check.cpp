// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Homebrew and the advanced files (game_files.h) as the Library reads them (metadata_bridge.cpp)
// and as Eden's loaders, with the build's loader patches (headless/CMakeLists.txt), identify them,
// on the files tools/check-direct-formats.py makes in the folder it names. Written with assert().
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "game_files.h"
#include "metadata_bridge.h"

#include "common/fs/path_util.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/xts_archive.h"
#include "core/loader/loader.h"

namespace {
std::string folder;
std::string keys;

std::string Path(const std::string& name) { return folder + "/" + name; }

Loader::FileType Identify(const std::string& name) {
    FileSys::RealVfsFilesystem vfs;
    const auto file = vfs.OpenFile(Path(name), FileSys::OpenMode::Read);
    assert(file);
    return Loader::IdentifyFile(file);
}

// A cover's TGA header: 32-bit, its width and height.
void CheckCover(const std::string& cover, int width, int height) {
    std::ifstream in(cover, std::ios::binary);
    unsigned char header[18]{};
    assert(in.read(reinterpret_cast<char*>(header), sizeof(header)));
    assert(header[2] == 2 && header[16] == 32);
    assert(header[12] + 256 * header[13] == width && header[14] + 256 * header[15] == height);
}

int Metadata(const std::string& name, std::string* title, const std::string& cover) {
    char text[513]{};
    std::remove(cover.c_str());
    const int found = eden_extract_game_metadata(Path(name).c_str(), keys.c_str(), cover.c_str(), text, sizeof(text));
    *title = text;
    return found;
}
} // namespace

int main(int argc, char** argv) {
    assert(argc == 2);
    folder = argv[1];
    keys = Path("keys");
    using Loader::FileType;

    // Eden's loaders by contents: homebrew, the lone NSOs (raw, LZ4, ZBIC), a KIP.
    assert(Identify("homebrew.nro") == FileType::NRO);
    for (const char* nso : {"homebrew.nso", "homebrew-lz4.nso", "homebrew-zbic.nso"})
        assert(Identify(nso) == FileType::NSO);
    assert(Identify("homebrew.kip") == FileType::KIP);
    // An unpacked ExeFS is chosen by its main: Eden identifies the folder, not its main as a lone NSO
    // (the build's IdentifyFile patch). A main without a main.npdm beside it stays an NSO.
    assert(Identify("Fixture/main") == FileType::DeconstructedRomDirectory);
    assert(Identify("Lone/main") == FileType::NSO);

    // What the Library lists, as Eden's loader for each kind identifies the file.
    const auto kind = [](const std::string& name) { return eden_program_kind(Path(name).c_str(), keys.c_str()); };
    assert(kind("homebrew.nro") == EDEN_PROGRAM_NRO);
    assert(kind("homebrew.nso") == EDEN_PROGRAM_NSO && kind("homebrew-zbic.nso") == EDEN_PROGRAM_NSO);
    assert(kind("homebrew.kip") == EDEN_PROGRAM_KIP);
    assert(kind("Fixture/main") == EDEN_PROGRAM_EXEFS);
    assert(kind("Lone/main") == EDEN_PROGRAM_NONE);        // no main.npdm: no ExeFS
    assert(kind("not-homebrew.nro") == EDEN_PROGRAM_NONE); // an NSO named .nro
    assert(kind("broken.kip") == EDEN_PROGRAM_NONE);
    assert(kind("garbage.nca") == EDEN_PROGRAM_NONE);      // no keys read it
    assert(kind("0123456789abcdef0123456789abcdef.nax") == EDEN_PROGRAM_NONE);
    char control[256] = "x";
    assert(!eden_program_control_nca(Path("garbage.nca").c_str(), keys.c_str(), control, sizeof(control)) && !control[0]);

    // The title ID each gives itself: an NRO's control data, a KIP's header, an ExeFS's main.npdm.
    assert(eden_game_title_id(Path("homebrew.nro").c_str()) == 0x0100F00DCAFE0000ULL);
    assert(eden_game_title_id(Path("homebrew.kip").c_str()) == 0x0100F00DCAFE1000ULL);
    assert(eden_game_title_id(Path("Fixture/main").c_str()) == 0x0100F00DCAFE2000ULL);
    assert(eden_game_title_id(Path("homebrew.nso").c_str()) == 0);
    assert(eden_rom_content(Path("homebrew.nro").c_str(), keys.c_str()) == EDEN_CONTENT_UNKNOWN);

    // Name, icon (a JPEG in the NRO's assets, icon_AmericanEnglish.dat in the ExeFS folder) and
    // languages, as a game's control data gives them.
    std::string title;
    const std::string cover = Path("cover.tga");
    assert(Metadata("homebrew.nro", &title, cover) == (EDEN_METADATA_TITLE | EDEN_METADATA_COVER));
    assert(title == "Nightshade Fixture");
    CheckCover(cover, 16, 16);
    assert(eden_game_supported_languages(Path("homebrew.nro").c_str(), keys.c_str()) == 0x3);
    assert(Metadata("Fixture/main", &title, cover) == (EDEN_METADATA_TITLE | EDEN_METADATA_COVER));
    assert(title == "Fixture ExeFS");
    CheckCover(cover, 16, 16);
    assert(Metadata("homebrew.kip", &title, cover) == EDEN_METADATA_TITLE && title == "Fixture");
    assert(Metadata("homebrew.nso", &title, cover) == 0 && title.empty());
    assert(Metadata("plain.nro", &title, cover) == 0); // an NRO without assets: named by its file

    // A NAX copied off an SD card, named by its content ID, is read with the keys its path on the
    // card gives (the build's patch): without the SD keys it fails on those, not on its path.
    {
        FileSys::RealVfsFilesystem vfs;
        const FileSys::NAX named(vfs.OpenFile(Path("0123456789abcdef0123456789abcdef.nax"), FileSys::OpenMode::Read));
        const FileSys::NAX other(vfs.OpenFile(Path("renamed.nax"), FileSys::OpenMode::Read));
        std::printf("NAX by content ID: status %u; by another name: %u\n", static_cast<unsigned>(named.GetStatus()),
                    static_cast<unsigned>(other.GetStatus()));
        assert(other.GetStatus() == Loader::ResultStatus::ErrorBadNAXFilePath);
        assert(named.GetStatus() != Loader::ResultStatus::ErrorBadNAXFilePath &&
               named.GetStatus() != Loader::ResultStatus::ErrorBadNAXHeader &&
               named.GetStatus() != Loader::ResultStatus::ErrorIncorrectNAXFileSize &&
               named.GetStatus() != Loader::ResultStatus::Success);
    }

    // The Library's names for them (game_files.h).
    assert(Eden::IsHomebrewFile("App.NRO") && !Eden::IsAdvancedFile("App.nro"));
    assert(Eden::IsAdvancedFile("main.nso") && Eden::IsAdvancedFile("x.NCA") && Eden::IsAdvancedFile("x.nax") &&
           Eden::IsAdvancedFile("x.kip") && Eden::IsAdvancedFile("Game/main"));
    assert(!Eden::IsExeFsMain("main") && !Eden::IsExeFsMain("Game/main.npdm") && !Eden::IsExeFsMain("/main"));
    assert(Eden::LibraryFormat("Game/main") == "EXEFS" && Eden::LibraryFormat("a.nro") == "NRO" &&
           Eden::LibraryFormat("Game/b.Nca") == "NCA" && Eden::LibraryFormat("c.nsz") == "NSZ");
    assert(Eden::IsLibraryFile("c.xcz") && !Eden::IsLibraryFile("notes.txt") && !Eden::IsLibraryFile("x.ncz"));
    std::puts("Homebrew and advanced files: Eden's identification (ExeFS by its main), the Library's kinds, title IDs, "
              "names, icons and languages, a NAX by its content ID PASS");
    return 0;
}
