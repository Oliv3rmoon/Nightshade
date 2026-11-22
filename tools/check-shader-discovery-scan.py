#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the production shader discovery scan and its cache-record boundary."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('--benchmark', action='store_true')
args = parser.parse_args()
source = args.source.read_text()


def implementation(signature):
    start = source.index(signature)
    end = source.index('\n}\n', start)
    return source[start:end + 2]


adapter = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

using u32 = std::uint32_t;
using u64 = std::uint64_t;
using GPUVAddr = u64;
constexpr size_t INST_SIZE = sizeof(u64);
constexpr size_t block_bytes = 4096;
constexpr size_t block_words = block_bytes / INST_SIZE;
constexpr u64 nop = 0x50b0000000070f00ULL;
constexpr u64 exit_inst = 0xe30000000007000fULL;
constexpr u64 self_branch = 0xe2400fffff87000fULL;
namespace Shader {
struct InvalidArgument : std::runtime_error {
    InvalidArgument(const char* message, u32) : std::runtime_error(message) {}
};
namespace Maxwell {
struct Location {
    u32 start;
    u32 Offset() const { return start + (start % 32 == 0 ? 8 : 0); }
};
}
}
namespace Common {
u64 CityHash64(const char* data, size_t size) {
    u64 value = 0;
    for (size_t i = 0; i < size; ++i) {
        value = value * 31 + static_cast<unsigned char>(data[i]);
    }
    return value;
}
}
struct Memory {
    std::vector<u64> words;
    size_t blocks{};
    void ReadBlock(GPUVAddr address, void* destination, size_t size) {
        assert(address == blocks * block_bytes && size == block_bytes);
        ++blocks;
        std::memset(destination, 0, size);
        const size_t bytes = words.size() * INST_SIZE;
        if (address < bytes) {
            const auto offset = static_cast<size_t>(address);
            const auto* input = reinterpret_cast<const char*>(words.data()) + offset;
            std::memcpy(destination, input, std::min(size, bytes - offset));
        }
    }
};
struct GenericEnvironment {
    Memory* gpu_memory;
    GPUVAddr program_base{};
    u32 start_address{}, cached_lowest{}, cached_highest{}, initial_offset{};
    bool is_proprietary_driver{};
    std::vector<u64> code;

    explicit GenericEnvironment(Memory& memory) : gpu_memory{&memory} {}
    u32 CodeStartAddress() const { return start_address + initial_offset; }
    std::optional<u64> Analyze();
    std::optional<u64> TryFindSize();
    size_t CachedSizeBytes() const noexcept;
    size_t CachedSizeWords() const noexcept;
};
'''
methods = '\n\n'.join(implementation(signature) for signature in (
    'std::optional<u64> GenericEnvironment::TryFindSize()',
    'std::optional<u64> GenericEnvironment::Analyze()',
    'size_t GenericEnvironment::CachedSizeBytes()',
    'size_t GenericEnvironment::CachedSizeWords()'))
cases = r'''
size_t ScanEmpty(Memory& memory) {
    memory.blocks = 0;
    GenericEnvironment env{memory};
    try {
        (void)env.Analyze();
        assert(false);
    } catch (const Shader::InvalidArgument&) {
        assert(memory.blocks == 1);
        return env.code.size() * INST_SIZE;
    }
}

int main(int argc, char**) {
    Memory empty;
    if (argc == 2) {
        constexpr size_t runs = 20000;
        const auto start = std::chrono::steady_clock::now();
        size_t bytes = 0;
        for (size_t i = 0; i < runs; ++i) {
            bytes += ScanEmpty(empty);
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("%zu empty scans took %.3f ms; %zu initialized scratch bytes per scan\n",
                    runs, elapsed, bytes / runs);
        return 0;
    }
    assert(ScanEmpty(empty) == block_bytes);

    Memory ready{{0, nop, exit_inst, 37}};
    GenericEnvironment prefix{ready};
    const auto hash = prefix.Analyze();
    assert(hash == Common::CityHash64(reinterpret_cast<const char*>(ready.words.data()), 3 * INST_SIZE));
    assert(prefix.CachedSizeBytes() == 4 * INST_SIZE && prefix.code == ready.words && ready.blocks == 1);

    Memory graphics;
    graphics.words.resize(12);
    graphics.words[10] = nop;
    graphics.words[11] = self_branch;
    GenericEnvironment fragment{graphics};
    fragment.initial_offset = 10 * INST_SIZE;
    assert(fragment.Analyze() && fragment.CachedSizeBytes() == 12 * INST_SIZE);
    assert(fragment.code == graphics.words && graphics.blocks == 1);

    Memory boundary{std::vector<u64>(block_words, nop)};
    boundary.words.back() = exit_inst;
    GenericEnvironment first_block{boundary};
    assert(first_block.Analyze() && first_block.CachedSizeBytes() == block_bytes + INST_SIZE);
    assert(first_block.code.size() == block_words + 1 && first_block.code.back() == 0);
    assert(boundary.blocks == 1);

    Memory multi{std::vector<u64>(2 * block_words, nop)};
    multi.words[block_words + 12] = self_branch;
    GenericEnvironment second_block{multi};
    assert(second_block.Analyze() && multi.blocks == 2);
    assert(second_block.code.size() == block_words + 13 && second_block.code.back() == self_branch);

    Memory gap{std::vector<u64>(3 * block_words, 0)};
    gap.words[1] = nop;
    gap.words[2 * block_words + 1] = exit_inst;
    GenericEnvironment with_gap{gap};
    assert(!with_gap.Analyze() && gap.blocks == 2);
    std::puts("Shader discovery: one-block empty retry, graphics header, cache tail and early control-flow fallback PASS");
}
'''

with tempfile.TemporaryDirectory(prefix='nightshade-discovery-scan-') as directory:
    executable = Path(directory) / 'scan'
    flags = ['-O3'] if args.benchmark else ['-O1', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    *flags, '-x', 'c++', '-', '-o', str(executable)],
                   input=adapter + methods + cases, text=True, check=True)
    subprocess.run([str(executable), *(['--benchmark'] if args.benchmark else [])], check=True)
