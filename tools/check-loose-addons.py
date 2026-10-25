#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check updates and DLC left directly in roms/ beside their game (headless/loose_addons.h).

Compiles the header and reads names as dumps name them: a game's title ID is not an add-on, its
update's and DLC's are, and only the game's own are its. Lists a folder of such files. And checks
that a session gives its own to Eden's external content provider, that the Library does not list
them as games but counts them for their game, and that a game's files to delete include them.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
cmake = (root / 'headless/CMakeLists.txt').read_text()
services = (root / 'headless/prosperoeden/eden_services.cpp').read_text()
bridge = (root / 'headless/metadata_bridge.cpp').read_text()

assert 'Eden::LooseAddOns::SessionFiles() = Eden::LooseAddOns::FilesOf(Eden::GamesPath("roms"), game_title);' in main
assert main.index('LooseAddOns::SessionFiles().clear();') < main.index('system.GetFileSystemController().CreateFactories('), \
    'the session\'s files are set before Eden opens its content folders'
assert 'for (const auto& path : ::Eden::LooseAddOns::SessionFiles())' in cmake
assert 'Creating ExternalContentProvider with {} opened directories' in cmake
assert '!(Eden::LooseAddOns::AddOn(name) && AddOnInGameFolder(JoinPath(roms, name)))' in services, \
    'the Library leaves out a loose update or DLC (named and holding one)'
assert 'Eden::LooseAddOns::FilesOf(Eden::GamesPath("roms"), title_id)' in services, 'deleting a game takes its loose add-ons'
assert 'Eden::LooseAddOns::AddOn(file->GetName())' in bridge, 'the Library counts loose add-ons for their game'

code = r'''
#include "loose_addons.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
using namespace Eden::LooseAddOns;
int main(int, char** argv) {
    const std::uint64_t ori = 0x01008DD013200000;
    assert(TitleIds("Ori [01008DD013200000][v0][Base].nsp") == std::vector<std::uint64_t>{ori});
    assert(TitleIds("x 01008dd013200800 y 0123456789abcdef0 z").size() == 1);  // 17 digits: no ID
    assert(!AddOn("Ori and the Will of the Wisps [01008DD013200000][v0][Base].nsp"));
    assert(AddOn("Ori and the Will of the Wisps [01008DD013200800][v196608][UPD].nsp"));
    assert(AddOn("Ori [01008DD013200800][v196608].nsp", ori));
    assert(AddOn("Ori DLC [01008DD013201001][v0].nsp", ori));       // DLC: the game's + 0x1000 + n
    assert(!AddOn("Other [0100F2C0115B6800][v65536].nsp", ori));    // another game's update
    assert(!AddOn("Game without an ID.nsp") && !AddOn("Game [v196608].nsp"));
    assert(PackageName("a.NSP") && PackageName("a.xcz") && !PackageName("a.txt") && !PackageName("nsp"));
    const std::filesystem::path roms = argv[1];
    for (const char* name : {"Ori [01008DD013200000][v0][Base].nsp", "Ori [01008DD013200800][v196608][UPD].nsp",
                             "Ori [01008DD013201001][DLC].nsz", "Other [0100F2C0115B6800][v65536].nsp",
                             "Ori [01008DD013200800] notes.txt"})
        std::ofstream(roms / name) << "x";
    std::filesystem::create_directory(roms / "Folder [01008DD013200800]");
    auto files = FilesOf(roms, ori);
    std::sort(files.begin(), files.end());
    assert(files.size() == 2);
    assert(files[0].ends_with("Ori [01008DD013200800][v196608][UPD].nsp"));
    assert(files[1].ends_with("Ori [01008DD013201001][DLC].nsz"));
    assert(FilesOf(roms, 0).empty() && FilesOf(roms / "missing", ori).empty());
    std::puts("Loose add-ons: PASS a game's update and DLC beside it in roms/ are told by their names, its own only");
}
'''

with tempfile.TemporaryDirectory() as scratch:
    source = Path(scratch) / 'loose_addons_check.cpp'
    binary = Path(scratch) / 'loose_addons_check'
    roms = Path(scratch) / 'roms'
    roms.mkdir()
    source.write_text(code)
    compiler = os.environ.get('CXX', 'c++')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    f'-I{root / "headless"}', str(source), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary), str(roms)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
