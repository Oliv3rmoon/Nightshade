#!/usr/bin/env python3
"""Exercise the native app's ordinary mounted-storage check on the host."""
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include "mounted_storage.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path root = argv[1];
    const auto data = root / "data";
    assert(Eden::MountedStorageStatus(data.c_str()) == 0);
    assert(fs::is_empty(data));
    std::puts("fresh writable directory: PASS");

    const auto name = ".nightshade-access-" + std::to_string(getpid()) + "-";
    for (int slot = 0; slot < 7; ++slot)
        std::ofstream(data / (name + std::to_string(slot))) << slot;
    assert(Eden::MountedStorageStatus(data.c_str()) == 0);
    assert(!fs::exists(data / (name + "7")));
    std::ofstream(data / (name + "7")) << 7;
    assert(Eden::MountedStorageStatus(data.c_str()) == EEXIST);
    for (int slot = 0; slot < 8; ++slot) {
        int value = -1;
        std::ifstream(data / (name + std::to_string(slot))) >> value;
        assert(value == slot);
    }
    std::puts("setup ignores occupied probe names: PASS");

    const auto file = root / "file";
    std::ofstream(file) << "preserve";
    assert(Eden::MountedStorageStatus(file.c_str()) == ENOTDIR);
    assert(Eden::MountedStorageStatus((root / "missing" / "data").c_str()) == ENOENT);
    assert(!fs::exists(root / "missing"));
    std::puts("missing mount or non-directory: PASS");

    if (geteuid() != 0) {
        const auto fixed_parent = root / "fixed-parent";
        fs::create_directories(fixed_parent / "mount");
        assert(chmod(fixed_parent.c_str(), 0555) == 0);
        const int mounted = Eden::MountedStorageStatus((fixed_parent / "mount").c_str());
        assert(chmod(fixed_parent.c_str(), 0755) == 0);
        assert(mounted == 0);
        assert(fs::is_empty(fixed_parent / "mount"));
        std::puts("writable child of a protected parent: PASS");

        const auto readonly = root / "readonly";
        assert(mkdir(readonly.c_str(), 0555) == 0);
        assert(Eden::MountedStorageStatus(readonly.c_str()) == EACCES);
        assert(fs::is_empty(readonly));
        assert(chmod(readonly.c_str(), 0755) == 0);
        std::puts("read-only permissions: PASS");
    }
}
'''

with tempfile.TemporaryDirectory(prefix="nightshade-mounted-storage-") as temp:
    directory = Path(temp)
    source = directory / "mounted.cpp"
    executable = directory / "check"
    source.write_text(SOURCE)
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "headless"), str(source), "-o", str(executable)], check=True)
    subprocess.run([executable, directory], check=True)
