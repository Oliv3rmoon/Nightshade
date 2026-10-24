#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a mod's shared font takes the place of the console's (headless/system_font_mod.h).

A translation mod brings its own standard font, as Atmosphere's contents/0100000000000811/romfs/
(Swordpdf/ProsperoEden#248). This compiles the header and runs it over a mods/ folder laid out
both ways, in any letter case, checks that headless/CMakeLists.txt hands Eden's font loader the
file it finds, and, when the pinned Eden source is in .deps, that the loader's line it patches is
there.
"""
from pathlib import Path
import glob, json, os, subprocess, tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
anchor = '        const auto font_fp = extracted_romfs->GetFile(font.second);\\n'
assert f'eden_patch(font_source "{anchor}"' in cmake
assert '::Eden::SystemFontMod::Read(\\n                Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir)' in cmake
assert 'font_fp = std::make_shared<FileSys::VectorVfsFile>(std::move(mod_font), font.second);' in cmake
assert 'list(REMOVE_ITEM core_sources hle/service/ns/platform_service_manager.cpp)' in cmake
# The mods folder Eden's load directory points at is the game files folder's (main.cpp).
assert 'Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, mods);' in (root / 'headless/main.cpp').read_text()

manifest = json.loads((root / 'UPSTREAM.json').read_text())
loaders = glob.glob(str(root / f'.deps/mirror-{manifest["eden_commit"]}/src/core/hle/service/ns/platform_service_manager.cpp'))
if loaders:
    source = Path(loaders[0]).read_text()
    assert source.count(anchor.replace('\\n', '\n')) == 1
    header = Path(loaders[0]).with_suffix('.h').read_text()
    assert 'std::make_pair(FontArchives::Standard, "nintendo_udsg-r_std_003.bfttf")' in header
    print('System font mod: the pinned font loader has the line the patch takes')
else:
    print('System font mod: SKIPPED the pinned font loader (no .deps Eden source)')

code = r'''
#include "system_font_mod.h"
#include <cassert>
#include <fstream>
namespace fs = std::filesystem;
using Eden::SystemFontMod::Read;
static void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}
int main(int, char** argv) {
    const fs::path mods = fs::path(argv[1]) / "mods";
    constexpr std::uint64_t standard = 0x0100000000000811;
    const std::string name = "nintendo_udsg-r_std_003.bfttf";
    // No folder for the archive: nothing, and the console's font stays.
    assert(Read(mods, standard, name).empty());
    // A mod folder of its own, names in other letter cases.
    Write(mods / "0100000000000811" / "Vietnamese" / "ROMFS" / "Nintendo_UDSG-R_std_003.BFTTF", "mod-font");
    assert(Read(mods, standard, name) == std::vector<std::uint8_t>(std::begin("mod-font"), std::end("mod-font") - 1));
    // Atmosphere's layout, romfs/ straight in the title's folder, goes first.
    Write(mods / "0100000000000811" / "romfs" / name, "atmosphere12");
    assert(Read(mods, standard, name).size() == 12);
    // Another font of the archive, or another archive: not this file.
    assert(Read(mods, standard, "nintendo_udjxh-db_zh-cn_003.bfttf").empty());
    assert(Read(mods, 0x0100000000000810, name).empty());
    // A file that cannot be a shared font (shorter than its header, or not whole words) is skipped
    // for the next mod's.
    Write(mods / "0100000000000811" / "romfs" / name, "bad");
    Write(mods / "0100000000000811" / "a-first" / "romfs" / name, "first-ok");
    assert(Read(mods, standard, name).size() == 8 && Read(mods, standard, name)[0] == 'f');
    std::puts("ok");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'system-font-mod'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe), tmp], check=True, capture_output=True)
print('System font mod: PASS a mod in mods/0100000000000811/ replaces the console\'s standard font')
