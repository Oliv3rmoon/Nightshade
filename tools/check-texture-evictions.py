#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the log line for textures freed while memory is short (#273; headless/texture_evictions.h).

Usage: check-texture-evictions.py EDEN_SRC BUILT_TEXTURE_CACHE_H

EDEN_SRC is Eden's src folder at the pinned revision, BUILT_TEXTURE_CACHE_H the texture_cache.h
tools/prepare-vulkan-port.py writes (vulkan-cache/video_core/texture_cache/texture_cache.h in the
build's headless folder). This checks that the pinned source has the two places the port hooks
(once each), that the built header counts a texture where the collector frees it, after every
reason to keep it and before it is gone, and ends the frame's window where the texture cache ends
its frame. Then it compiles those two hooks as the build wrote them, with the header, and runs
them: textures freed in the collector's other passes are not counted, one line every 10 s at most
and only after textures were freed, with the most conversions waiting and the pool's figures.
"""
from pathlib import Path
import subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
pinned = (Path(sys.argv[1]) / 'video_core/texture_cache/texture_cache.h').read_text()
built = Path(sys.argv[2]).read_text()

def function(text, signature):
    begin = text.index(signature)
    return text[begin:text.index('\ntemplate <class P>', begin)]

free = '        UnregisterImage(image_id);\n        DeleteImage(image_id, image.scale_tick > frame_tick + 5);'
frame = '    sentenced_images.Tick();\n'
assert pinned.count(free) == 1 and function(pinned, 'void TextureCache<P>::RunGarbageCollector() {').count(free) == 1
assert pinned.count(frame) == 1 and function(pinned, 'void TextureCache<P>::TickFrame() {').count(frame) == 1

assert built.count('#include "texture_evictions.h"') == 1
collector = function(built, 'void TextureCache<P>::RunGarbageCollector() {')
cleanup = collector[collector.index('    const auto Cleanup = [&, this](ImageId image_id) {'):]
cleanup = cleanup[:cleanup.index('\n    };\n')]
hook_begin = cleanup.index('        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {\n'
                           '            ::Eden::TextureEvictions::Freed(')
freed_hook = cleanup[hook_begin:cleanup.index('        }\n', hook_begin) + len('        }\n')]
# After the reasons to keep a texture and its copy back, before it is unregistered.
assert cleanup.index('if (True(image.flags & ImageFlagBits::IsDecoding))') < hook_begin
assert cleanup.index('(!DirtyEvictions() && must_download)) {\n            return false;') < hook_begin
assert cleanup.index('SwizzleImage(') < hook_begin < cleanup.index(free)
assert built.count('::Eden::TextureEvictions::Freed(') == 1
tick = function(built, 'void TextureCache<P>::TickFrame() {')
frame_begin = tick.index('    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {\n'
                         '        ::Eden::TextureEvictions::Frame(')
frame_hook = tick[frame_begin:tick.index('    }\n', frame_begin) + len('    }\n')]
assert tick.index('RunGarbageCollector();') < frame_begin < tick.index(frame)
assert built.count('::Eden::TextureEvictions::Frame(') == 1

code = r'''
#include "texture_evictions.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
// What the two hooks name: the port's figures (performance.h), Eden's flags, the runtime.
namespace Eden::Performance {
inline std::atomic<unsigned long long (*)()> graphics_memory_free{nullptr};
inline std::atomic<unsigned long long (*)()> graphics_memory_held{nullptr};
}
namespace Vulkan { struct TextureCacheRuntime {}; }
enum class ImageFlagBits : unsigned { Converted = 1u << 13 };
constexpr bool True(unsigned flags) { return flags != 0; }
constexpr unsigned operator&(unsigned flags, ImageFlagBits bit) { return flags & unsigned(bit); }
struct Image { unsigned flags = 0; };
struct AsyncDecodeContext {};
template <class Runtime>
struct Cache {
    std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;
    unsigned unregistered = 0;
    void UnregisterImage(int) { ++unregistered; }
    // Cleanup's end, as the build writes it, for one texture.
    void Free([[maybe_unused]] Image& image, [[maybe_unused]] bool aggressive_mode,
              [[maybe_unused]] bool must_download) {
        const int image_id = 0;
''' + freed_hook + r'''
        UnregisterImage(image_id);
    }
    void TickFrame() {
''' + frame_hook + r'''
    }
};
static unsigned long long FreeBlock() { return 702ull << 20; }
static unsigned long long Held() { return 6120ull << 20; }
namespace E = Eden::TextureEvictions;
// FrameAt with a file of its own: whether it wrote the line, and what it wrote.
static std::string written;
static bool Frame(long long now, std::size_t waiting, unsigned long long (*free_block)(),
                  unsigned long long (*held)()) {
    std::FILE* const file = std::tmpfile();
    assert(file != nullptr);
    const bool line = E::FrameAt(now, waiting, free_block, held, file);
    std::rewind(file);
    written.assign(1 << 12, '\0');
    written.resize(std::fread(written.data(), 1, written.size(), file));
    std::fclose(file);
    assert(line == !written.empty());
    return line;
}
int main() {
    constexpr long long s = 1'000'000'000;
    // The header alone: nothing until 10 s have gone, and then only after textures were freed.
    E::window = {};
    assert(!Frame(1, 0, &FreeBlock, &Held));
    E::Freed(false, true, false);                    // another pass: loaded again in place
    assert(!Frame(11 * s, 3, &FreeBlock, &Held));
    assert(E::window.begin_ns == 11 * s && E::window.freed == 0);
    E::Freed(true, false, false);
    E::Freed(true, true, false);
    E::Freed(true, true, true);
    E::Freed(false, false, true);                    // written by the GPU: counted in any pass
    assert(!Frame(15 * s, 9, &FreeBlock, &Held));
    assert(!Frame(20 * s, 2, &FreeBlock, &Held));
    assert(Frame(21 * s, 0, &FreeBlock, &Held));
    assert(written == "[ProsperoEden] graphics memory: textures freed while memory is short, in 10 s: 4, "
                      "2 of them ASTC kept as BC3, 2 written by the GPU; up to 9 conversions waiting; "
                      "the pool's largest free block is 702 MiB, the allocator holds 6120 MiB\n");
    assert(E::window.freed == 0 && E::window.waiting == 0 && E::window.begin_ns == 21 * s);
    assert(!Frame(31 * s, 0, &FreeBlock, &Held));    // a window with nothing freed: no line
    E::Freed(true, false, false);
    assert(Frame(50 * s, 1, nullptr, nullptr));      // a long frame: the window's own length
    assert(written == "[ProsperoEden] graphics memory: textures freed while memory is short, in 19 s: 1, "
                      "0 of them ASTC kept as BC3, 0 written by the GPU; up to 1 conversions waiting\n");
    // The hooks as the build writes them, in the Vulkan cache.
    E::window = {};
    Eden::Performance::graphics_memory_free = &FreeBlock;
    Eden::Performance::graphics_memory_held = &Held;
    Cache<Vulkan::TextureCacheRuntime> cache;
    cache.async_decodes.resize(5);
    Image astc{unsigned(ImageFlagBits::Converted)}, plain{};
    cache.Free(astc, true, false);
    cache.Free(plain, false, false);
    cache.Free(plain, false, true);
    assert(cache.unregistered == 3 && E::window.freed == 2 && E::window.converted == 1 && E::window.written == 1);
    cache.TickFrame();
    assert(E::window.waiting == 5 && E::window.begin_ns != 0);
    // Not the OpenGL cache.
    Cache<int> other;
    other.Free(astc, true, true);
    assert(E::window.freed == 2);
    std::puts("texture evictions: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'evictions'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', '-I' + str(root / 'headless'), '-x', 'c++', '-',
                    '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True, timeout=60)
print('Texture evictions: PASS hooks pinned and placed (after the reasons to keep a texture, before it goes; '
      'at the end of the frame), one line per 10 s at most and only after textures were freed while memory '
      'was short, the collector\'s other passes not counted')
