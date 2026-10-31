#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The copy engine's constant fill (Eden patch 13, tools/eden-patches/13-dma-constant-fill.diff).

A game clears memory with the copy engine: a remapped copy whose components are constants. Eden
wrote line_length_in * component_size bytes of it, an element of one component, to guest memory
only unless the component was 4 bytes, and copied from offset_in for a multi-line fill. A counter
cleared as four 1-byte components kept three of its four bytes, the GPU's copy of it kept all of
them, and the culling shader that then added to it gave the draw an instance count such as
0x3F800001 (an old float 1.0 plus one): Final Fantasy Resonance stopped the GPU on draws asking
for 27 million to 2.7 billion instances (Oliv3rmoon/Nightshade#1).

The fill reaches guest memory and the buffer cache's copy (the inline write of each partly covered
page, then whole pages invalidated at once), but not the texture cache: an image a fill covers only
in part would take guest memory's older copy of the rest at its next upload. Its bytes are built a
word at a time, or by copying one element over and over, not byte by byte (16 MiB took 11 ms that
way on a Mac).

This applies every Eden patch, in order, to the pinned archive's files; builds the fill's helpers on
their own and checks what they write (the case above among them) and how fast; and compiles the
patched files that this build compiles with their own compile commands
(tools/build-headless-host.sh passes the build folder).

    check-dma-fill.py [build folder]
"""
from pathlib import Path
import json
import os
import shlex
import subprocess
import sys
import tarfile
import tempfile

root = Path(__file__).resolve().parents[1]
# EDEN_ARCHIVE: another copy of the same pinned archive (a checkout without make deps).
archive = Path(os.environ.get('EDEN_ARCHIVE', root / '.deps/eden-67bada77.tar.gz'))
patches = sorted((root / 'tools/eden-patches').glob('*.diff'))
fill_patch = root / 'tools/eden-patches/13-dma-constant-fill.diff'
assert fill_patch in patches, fill_patch
build = Path(sys.argv[1]) if len(sys.argv) > 1 else None

touched = sorted({line[6:].strip() for p in patches for line in p.read_text().splitlines()
                  if line.startswith('+++ b/')})

with tempfile.TemporaryDirectory(prefix='eden-dma-fill-') as folder:
    tree = Path(folder) / 'eden'
    tree.mkdir()
    # The touched files from the pinned archive, as tools/prepare-build.sh restores them.
    with tarfile.open(archive) as tar:
        members = {m.name.split('/', 1)[1]: m for m in tar.getmembers() if '/' in m.name}
        for name in touched:
            assert name in members, f'{name} is not in {archive.name}: a patch may only change its files'
            member = members[name]
            data = tar.extractfile(member).read()
            (tree / name).parent.mkdir(parents=True, exist_ok=True)
            (tree / name).write_bytes(data)
    for patch in patches:
        subprocess.run(['patch', '-d', str(tree), '-p1', '-N', '-s', '-i', str(patch)], check=True)

    dma = (tree / 'src/video_core/engines/maxwell_dma.cpp').read_text()
    helper = dma.split('// constant fill: begin', 1)[1].split('\n', 1)[1].split('// constant fill: end', 1)[0]
    # The fill runs before the multi-line copy.
    launch = dma.split('void MaxwellDMA::Launch() {', 1)[1].split('\n}\n', 1)[0]
    assert launch.index('LaunchConstantFill()') < launch.index('if (launch.multi_line_enable) {')
    body = dma.split('bool MaxwellDMA::LaunchConstantFill() {', 1)[1].split('\n}\n', 1)[0]
    assert 'accelerate.BufferClear(dst, line_bytes / 4, *word)' in body
    assert 'u64{regs.line_length_in} * fill->element_size' in body
    # Otherwise: partly covered pages (and fills that keep bytes) a piece at a time, into the
    # buffer cache's copy in order (BufferWritten), with the query and shader caches; whole pages
    # written and then invalidated at once. Never the texture cache, never byte by byte.
    assert 'AccelerateInlineToMemory' not in body
    assert 'CacheType::TextureCache' not in body and 'CacheType::All' not in body
    assert body.count('memory_manager.InvalidateRegion(') == 2 and body.count('VideoCommon::CacheType::') == 3
    assert 'accelerate.BufferWritten(*device,' in body
    assert ('memory_manager.InvalidateRegion(at, piece, VideoCommon::CacheType::QueryCache |\n'
            '                                                               VideoCommon::CacheType::ShaderCache);') in body
    assert 'const auto [whole_begin, whole_end] = WholePages(dst, end, PIECE);' in body
    assert body.index('fill_pieces(dst, whole_begin);') < body.index(
        'FillBytes(*fill, whole_begin - dst, write_buffer.data(), chunk);') < body.index(
        'memory_manager.InvalidateRegion(whole_begin, whole_end - whole_begin,\n'
        '                                            VideoCommon::CacheType::NoTextureCache);') < body.index(
        'fill_pieces(whole_end, end);')
    assert 'const u64 chunk = (PIECE * 16) / fill->element_size * fill->element_size;' in body
    assert '% fill->element_size];' not in body, 'a byte at a time again'
    assert 'EDEN_DMA_FILL elements=%u' in dma
    # The copy engine's buffer-only write, in each renderer: the buffer cache's copy in order
    # where the GPU wrote the page, else an upload of it; nothing of the texture cache's.
    interface = (tree / 'src/video_core/engines/maxwell_dma.h').read_text()
    assert 'virtual void BufferWritten(DAddr address, std::span<const u8> data) = 0;' in interface
    for renderer in ('vulkan/vk_rasterizer', 'opengl/gl_rasterizer'):
        source = (tree / f'src/video_core/renderer_{renderer}.cpp').read_text()
        written = source.split('void AccelerateDMA::BufferWritten(DAddr address, std::span<const u8> data) {', 1)[1].split('\n}\n', 1)[0]
        assert written.strip() == ('std::scoped_lock lock{buffer_cache.mutex};\n'
                                   '    if (!buffer_cache.InlineMemory(address, data.size(), data)) {\n'
                                   '        buffer_cache.WriteMemory(address, data.size());\n    }'), renderer
        assert 'void BufferWritten(DAddr address, std::span<const u8> data) override;' in (
            tree / f'src/video_core/renderer_{renderer}.h').read_text(), renderer
    assert 'void BufferWritten(DAddr, std::span<const u8>) override {}' in (
        tree / 'src/video_core/renderer_null/null_rasterizer.h').read_text()
    # DMAClear decides as upstream does (a clear it refuses is the fill's to finish, without one
    # buffer made of the whole range); it only says when its quarter-sized check refused one.
    cache = (tree / 'src/video_core/buffer_cache/buffer_cache.h').read_text()
    clear = cache.split('bool BufferCache<P>::DMAClear(', 1)[1].split('\n}\n', 1)[0]
    assert 'const bool dest_dirty = IsRegionRegistered(*cpu_dst_address, amount);' in clear
    assert clear.index('IsRegionRegistered(*cpu_dst_address, amount * sizeof(u32))') < clear.index(
        'return false;\n    }\n\n    const size_t size = amount * sizeof(u32);')
    assert 'EDEN_DMA_CLEAR_RANGE bytes=%llu' in clear
    # The CPU's indirect draws: the limit is 2^28 vertices (patch 11's check, patch 13's limit).
    macro = (tree / 'src/video_core/macro.cpp').read_text()
    assert 'return u64(count) * u64(instance_count) > (u64(1) << 28);' in macro

    test = Path(folder) / 'fill.cpp'
    test.write_text(r'''
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>
#include <algorithm>
using u8 = std::uint8_t; using u32 = std::uint32_t; using u64 = std::uint64_t;
namespace {
''' + helper + r'''
// What the destination holds after a fill of `elements` elements over `before`.
std::vector<u8> Apply(const ConstantFill& fill, u32 elements, std::vector<u8> before) {
    const u64 bytes = u64{elements} * fill.element_size;
    for (u64 i = 0; i < bytes; ++i)
        if (fill.written[i % fill.element_size]) before[i] = fill.pattern[i % fill.element_size];
    return before;
}
// The earlier emulation: line_length_in * component_size bytes of the constant, an element of one.
std::vector<u8> ApplyBefore(u32 component_size, u32 elements, u32 value, std::vector<u8> before) {
    std::vector<u32> words(elements, value);
    std::memcpy(before.data(), words.data(), u64{elements} * component_size);
    return before;
}
u32 Word32(const std::vector<u8>& bytes) { u32 w; std::memcpy(&w, bytes.data(), 4); return w; }
}
int main() {
    constexpr u32 A = 4, B = 5, NO = 6;
    // Final Fantasy Resonance's counter: an old float 1.0 cleared as four 1-byte components, then
    // one instance added to it by the culling shader's atomic.
    const std::vector<u8> old_float{0x00, 0x00, 0x80, 0x3f};
    auto bytes = MakeConstantFill(1, 4, {A, A, A, A}, 0, 0);
    assert(bytes && bytes->element_size == 4 && !bytes->keeps_bytes && bytes->Word() == 0u);
    assert(Word32(ApplyBefore(1, 1, 0, old_float)) + 1 == 1065353217u); // 0x3F800001, as logged
    assert(Word32(Apply(*bytes, 1, old_float)) + 1 == 1u);
    // A 32-bit fill, as before.
    auto word = MakeConstantFill(4, 1, {A, 0, 0, 0}, 0x12345678, 0);
    assert(word && word->element_size == 4 && word->Word() == 0x12345678u);
    // Smaller components take the low bytes of the constant.
    auto half = MakeConstantFill(2, 1, {A, 0, 0, 0}, 0x1234abcd, 0);
    assert(half && half->element_size == 2 && half->Word() == 0xabcdabcdu);
    auto byte = MakeConstantFill(1, 1, {A, 0, 0, 0}, 0x7f, 0);
    assert(byte && byte->Word() == 0x7f7f7f7fu);
    const std::vector<u8> ones(16, 0xee);
    const auto filled = Apply(*byte, 3, ones);
    assert(filled[0] == 0x7f && filled[2] == 0x7f && filled[3] == 0xee);
    // Two constants: the element alternates, not a single 32-bit word.
    auto pair = MakeConstantFill(4, 2, {A, B, 0, 0}, 1, 2);
    assert(pair && pair->element_size == 8 && !pair->Word());
    const auto pairs = Apply(*pair, 2, std::vector<u8>(16, 0));
    assert(Word32({pairs.begin() + 8, pairs.begin() + 12}) == 1 && Word32({pairs.begin() + 12, pairs.end()}) == 2);
    auto quad = MakeConstantFill(4, 4, {A, A, A, A}, 0, 0);
    assert(quad && quad->element_size == 16 && quad->Word() == 0u);
    // Before: four 4-byte components cleared one quarter of the range.
    const std::vector<u8> dirty(16, 0xff);
    assert(ApplyBefore(4, 1, 0, dirty)[4] == 0xff && Apply(*quad, 1, dirty)[15] == 0);
    // 3-byte elements repeat as a word only when their bytes are all equal.
    auto three = MakeConstantFill(3, 1, {A, 0, 0, 0}, 0x00112233, 0);
    assert(three && three->element_size == 3 && !three->Word());
    auto three_equal = MakeConstantFill(1, 3, {A, A, A, 0}, 0x55, 0);
    assert(three_equal && three_equal->Word() == 0x55555555u);
    // A NO_WRITE component keeps the destination's bytes.
    auto kept = MakeConstantFill(4, 2, {A, NO, 0, 0}, 9, 0);
    assert(kept && kept->keeps_bytes && !kept->Word());
    const auto partial = Apply(*kept, 1, dirty);
    assert(Word32(partial) == 9 && partial[4] == 0xff);
    auto none = MakeConstantFill(4, 1, {NO, 0, 0, 0}, 9, 0);
    assert(none && none->element_size == 0);
    // A component from the source is a remapped copy: not a fill.
    assert(!MakeConstantFill(4, 2, {A, 0, 0, 0}, 0, 0));
    assert(!MakeConstantFill(4, 1, {2, 0, 0, 0}, 0, 0));
    // The whole pages of a range: none inside one page, the ends aligned inward otherwise.
    using Pages = std::pair<u64, u64>;
    assert((WholePages(0x1004, 0x1008, 0x1000) == Pages{0x1008, 0x1008}));
    assert((WholePages(0x1000, 0x2000, 0x1000) == Pages{0x1000, 0x2000}));
    assert((WholePages(0x1004, 0x4010, 0x1000) == Pages{0x2000, 0x4000}));
    assert((WholePages(0x1ff0, 0x2010, 0x1000) == Pages{0x2000, 0x2000}));
    assert((WholePages(0x2000, 0x2010, 0x1000) == Pages{0x2000, 0x2000}));
    // FillBytes writes what a byte at a time would, for every element shape, offset and size.
    std::vector<ConstantFill> shapes;
    for (u32 size = 1; size <= 4; ++size)
        for (u32 count = 1; count <= 4; ++count) {
            shapes.push_back(*MakeConstantFill(size, count, {A, A, A, A}, 0, 0));
            shapes.push_back(*MakeConstantFill(size, count, {A, A, A, A}, 0x5a5a5a5a, 0));
            shapes.push_back(*MakeConstantFill(size, count, {A, B, A, B}, 0x01020304, 0xa0b0c0d0));
            shapes.push_back(*MakeConstantFill(size, count, {B, A, A, B}, 0x11223344, 0x55667788));
        }
    for (const ConstantFill& shape : shapes)
        for (u64 offset = 0; offset < 40; ++offset)
            for (u64 size : {0ull, 1ull, 3ull, 7ull, 8ull, 9ull, 31ull, 48ull, 4096ull, 65535ull}) {
                std::vector<u8> fast(size + 1, 0xcc), slow(size + 1, 0xcc);
                FillBytes(shape, offset, fast.data(), size);
                for (u64 i = 0; i < size; ++i) slow[i] = shape.pattern[(offset + i) % shape.element_size];
                assert(fast == slow);
            }
    // And it is fast: 16 MiB against the byte-at-a-time loop it replaced, best of five.
    const auto time = [](auto&& fill_once) {
        double best = 1e9;
        for (int run = 0; run < 5; ++run) {
            const auto start = std::chrono::steady_clock::now();
            fill_once();
            best = std::min(best, std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start).count());
        }
        return best;
    };
    std::vector<u8> big(16u << 20);
    unsigned sink = 0;
    for (const auto& [name, shape] : {std::pair{"counter (1-byte components x4)", *bytes},
                                      std::pair{"12-byte element", *MakeConstantFill(4, 3, {A, B, A, 0}, 1, 2)}}) {
        const double fast = time([&] { FillBytes(shape, 4, big.data(), big.size()); sink += big[12345]; });
        const double slow = time([&] {
            for (u64 i = 0; i < big.size(); ++i) big[i] = shape.pattern[(4 + i) % shape.element_size];
            sink += big[12345];
        });
        std::printf("16 MiB %s: %.2f ms, a byte at a time %.2f ms\n", name, fast, slow);
        assert(fast * 2 < slow);
    }
    std::printf("fill elements PASS%s\n", sink == 0xffffffffu ? " " : "");
}
''')
    binary = Path(folder) / 'fill'
    subprocess.run(['c++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    str(test), '-o', str(binary)], check=True)
    out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=120)
    lines = out.stdout.strip().splitlines()
    assert lines[-1].strip() == 'fill elements PASS', out.stdout
    print('\n'.join(lines[:-1]))

    compiled = []
    if build is not None:
        commands = json.loads((build / 'compile_commands.json').read_text())
        # The patched files themselves, the buffer cache's instantiations (buffer_cache.h) and the
        # null renderer (its header's BufferWritten).
        wanted = {name for name in touched if name.endswith('.cpp')} | {
            'src/video_core/renderer_vulkan/vk_buffer_cache.cpp',
            'src/video_core/renderer_opengl/gl_buffer_cache_base.cpp',
            'src/video_core/renderer_null/null_rasterizer.cpp'}
        overlay = tree / 'src'
        for entry in commands:
            file = Path(entry['file'])
            rel = next((w for w in wanted if file.as_posix().endswith('/' + w)), None)
            if rel is None:
                continue
            args = shlex.split(entry['command']) if 'command' in entry else list(entry['arguments'])
            out_args, skip = [args[0], '-I', str(overlay)], False
            for arg in args[1:]:
                if skip:
                    skip = False
                    continue
                if arg in ('-o', '-MF', '-MT', '-MQ'):
                    skip = True
                    continue
                if arg in ('-MD', '-MMD', '-c') or arg == entry['file']:
                    continue
                out_args.append(arg)
            source = tree / rel if (tree / rel).exists() else file
            out_args += ['-fsyntax-only', str(source)]
            subprocess.run(out_args, check=True, cwd=entry['directory'])
            compiled.append(rel)
        for required in ('src/video_core/engines/maxwell_dma.cpp',):
            assert required in compiled, f'{required} is not in this build: nothing compiled it'
print('Copy engine constant fill: every element byte, the GPU\'s copy, multi-line fills, the '
      'cleared counter case, all Eden patches in order' +
      (f', patched {", ".join(sorted(set(compiled)))} compile' if compiled else '') + ' PASS')
