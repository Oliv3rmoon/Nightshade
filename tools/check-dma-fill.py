#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The copy engine's constant fill (Eden patch 12, tools/eden-patches/12-dma-constant-fill.diff).

A game clears memory with the copy engine: a remapped copy whose components are constants. Eden
wrote line_length_in * component_size bytes of it, an element of one component, to guest memory
only unless the component was 4 bytes, and copied from offset_in for a multi-line fill. A counter
cleared as four 1-byte components kept three of its four bytes, the GPU's copy of it kept all of
them, and the culling shader that then added to it gave the draw an instance count such as
0x3F800001 (an old float 1.0 plus one): Final Fantasy Resonance stopped the GPU on draws asking
for 27 million to 2.7 billion instances (Oliv3rmoon/Nightshade#1).

This applies every Eden patch, in order, to the pinned archive's files; builds the fill's element
helper on its own and checks what it writes (the case above among them); and compiles the patched
files that this build compiles with their own compile commands (tools/build-headless-host.sh
passes the build folder).

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
fill_patch = root / 'tools/eden-patches/12-dma-constant-fill.diff'
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
    # The fill runs before the multi-line copy, and reaches every cache through the inline path.
    launch = dma.split('void MaxwellDMA::Launch() {', 1)[1].split('\n}\n', 1)[0]
    assert launch.index('LaunchConstantFill()') < launch.index('if (launch.multi_line_enable) {')
    body = dma.split('bool MaxwellDMA::LaunchConstantFill() {', 1)[1].split('\n}\n', 1)[0]
    assert 'accelerate.BufferClear(dst, line_bytes / 4, *word)' in body
    assert 'u64{regs.line_length_in} * fill->element_size' in body
    # Otherwise: partial pages (and fills that keep bytes) through the inline-to-memory path,
    # whole pages written and then invalidated in every cache.
    assert body.count('rasterizer->AccelerateInlineToMemory(') == 1
    assert 'const auto [whole_begin, whole_end] = WholePages(dst, end, PIECE);' in body
    assert body.index('fill_inline(dst, whole_begin);') < body.index(
        'memory_manager.InvalidateRegion(whole_begin, whole_end - whole_begin);') < body.index(
        'fill_inline(whole_end, end);')
    assert 'EDEN_DMA_FILL elements=%u' in dma
    # DMAClear decides as upstream does (a clear it refuses is the fill's to finish, without one
    # buffer made of the whole range); it only says when its quarter-sized check refused one.
    cache = (tree / 'src/video_core/buffer_cache/buffer_cache.h').read_text()
    clear = cache.split('bool BufferCache<P>::DMAClear(', 1)[1].split('\n}\n', 1)[0]
    assert 'const bool dest_dirty = IsRegionRegistered(*cpu_dst_address, amount);' in clear
    assert clear.index('IsRegionRegistered(*cpu_dst_address, amount * sizeof(u32))') < clear.index(
        'return false;\n    }\n\n    const size_t size = amount * sizeof(u32);')
    assert 'EDEN_DMA_CLEAR_RANGE bytes=%llu' in clear
    # The CPU's indirect draws: the limit is 2^28 vertices (patch 11's check, patch 12's limit).
    macro = (tree / 'src/video_core/macro.cpp').read_text()
    assert 'return u64(count) * u64(instance_count) > (u64(1) << 28);' in macro

    test = Path(folder) / 'fill.cpp'
    test.write_text(r'''
#include <array>
#include <cassert>
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
    std::puts("fill elements PASS");
}
''')
    binary = Path(folder) / 'fill'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    str(test), '-o', str(binary)], check=True)
    out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
    assert out.stdout.strip() == 'fill elements PASS', out.stdout

    compiled = []
    if build is not None:
        commands = json.loads((build / 'compile_commands.json').read_text())
        # The patched files themselves, and the buffer cache's instantiations (buffer_cache.h).
        wanted = {name for name in touched if name.endswith('.cpp')} | {
            'src/video_core/renderer_vulkan/vk_buffer_cache.cpp',
            'src/video_core/renderer_opengl/gl_buffer_cache_base.cpp'}
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
