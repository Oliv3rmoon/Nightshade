#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the generated Vulkan cache write path with a changing driver cache."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

port = Path(sys.argv[1]) / 'vk_pipeline_cache_cost.cpp'
source = port.read_text()
start = source.index('void PipelineCache::SerializeVulkanPipelineCache(')
end = source.index('vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(', start)
code = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

using u32 = std::uint32_t;
enum VkResult { VK_SUCCESS, VK_INCOMPLETE, VK_ERROR_DEVICE_LOST };
namespace vk {
struct PipelineCache {
    const std::vector<char>& bytes;
    mutable unsigned incomplete_reads = 0;
    bool query_fails = false;
    bool read_fails = false;
    explicit operator bool() const { return true; }
    VkResult Read(std::size_t* size, void* data) const {
        if (!data) {
            *size = bytes.size() - (incomplete_reads != 0);
            return query_fails ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        }
        if (read_fails) {
            return VK_ERROR_DEVICE_LOST;
        }
        std::memcpy(data, bytes.data(), *size);
        if (incomplete_reads != 0) {
            --incomplete_reads;
            return VK_INCOMPLETE;
        }
        return VK_SUCCESS;
    }
};
}
namespace Eden::Performance {
struct Timer { ~Timer() {} };
Timer VulkanTimer(int) { return {}; }
}
namespace Common::FS {
std::string PathToUTF8String(const std::filesystem::path& path) { return path.string(); }
}
#define LOG_INFO(category, pattern, ...) (void)(__VA_ARGS__)
#define LOG_WARNING(category, pattern) (void)0
#define LOG_ERROR(category, pattern, ...) (void)0
constexpr std::array<char, 8> VULKAN_CACHE_MAGIC_NUMBER{'y','u','z','u','v','k','c','h'};
struct PipelineCache {
    static void SerializeVulkanPipelineCache(const std::filesystem::path&, const vk::PipelineCache&, u32);
};
// CACHE_SERIALIZER

std::vector<char> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{file}, {}};
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::filesystem::path path{argv[1]};
    const auto save = [&](const vk::PipelineCache& cache) {
        PipelineCache::SerializeVulkanPipelineCache(path, cache, 7);
    };
    const std::size_t header = VULKAN_CACHE_MAGIC_NUMBER.size() + sizeof(u32);
    std::vector<char> bytes(48 * 1024 + 7, 'a');
    save({bytes});
    const auto initial = Read(path);
    assert(initial.size() == header + bytes.size());
    assert(std::equal(initial.begin() + header, initial.end(), bytes.begin()));

    const auto earlier = std::filesystem::last_write_time(path) - std::chrono::hours(1);
    std::filesystem::last_write_time(path, earlier);
    const auto marked = std::filesystem::last_write_time(path);
    save({bytes});
    assert(std::filesystem::last_write_time(path) == marked);

    bytes.back() = 'b';
    save({bytes, 3});
    save({bytes, 0, true});
    save({bytes, 0, false, true});
    assert(Read(path) == initial);
    assert(std::filesystem::last_write_time(path) == marked);

    save({bytes, 1});
    auto updated = Read(path);
    assert(updated.size() == initial.size() && updated.back() == 'b');
    assert(std::filesystem::last_write_time(path) != marked);
    updated.push_back('x');
    std::ofstream(path, std::ios::binary).write(updated.data(), updated.size());
    save({bytes});
    assert(Read(path).size() == initial.size());
    assert(!std::filesystem::exists(path.string() + ".part"));
}
'''

with tempfile.TemporaryDirectory(prefix='nightshade-vulkan-cache-') as folder:
    binary = Path(folder) / 'cache-save'
    subprocess.run(
        [os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror',
         '-x', 'c++', '-', '-o', str(binary)],
        input=code.replace('// CACHE_SERIALIZER', source[start:end]), text=True, check=True)
    subprocess.run([str(binary), str(Path(folder) / 'vulkan_pipelines.bin')], check=True)
print('Vulkan driver cache: unchanged writes skipped, incomplete reads preserve previous cache, growth retried PASS')
