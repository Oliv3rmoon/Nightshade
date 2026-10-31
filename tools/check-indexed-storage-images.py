#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of Eden patch 12: an indexed storage image array stays within what the pipelines bind.

Fighting Force Collection (Oliv3rmoon/Nightshade#4): a compute shader indexes an array of storage
images. Pinned Eden sizes such an array from the constant buffer space after its first handle,
up to 1024 elements, and caps only sampled textures. With OpenGL the dispatch that first used it
(when a game is picked on the collection's menu) pushed more views than the pipeline's fixed
static_vector holds (64 textures + 16 images) and the session ended with
"boost::container::bad_alloc thrown"; with Vulkan every element's handle was read, past the game's
own array, as an empty texture descriptor ("texture format=0").

This applies tools/eden-patches in order to the files they touch (from the pinned archive, its
hash checked against UPSTREAM.json), checks that the storage image cap reaches both descriptor
kinds and that OpenGL's limit is its graphics pipelines' 8 images, then compiles the patched
functions themselves: the pinned count for an array at the start of a constant buffer is 1024,
the patch binds 8, shared between arrays and never more than the host allows. Where Boost's
headers are found it also shows the pinned count overflowing OpenGL's static_vector with the
message the console logged.
"""
from pathlib import Path
import hashlib, json, os, re, shutil, subprocess, sys, tarfile, tempfile

root = Path(__file__).resolve().parents[1]
archive = Path(sys.argv[1]) if len(sys.argv) > 1 else root / '.deps/eden-67bada77.tar.gz'
pinned = json.loads((root / 'UPSTREAM.json').read_text())['archives']['eden-67bada77.tar.gz']
digest = hashlib.sha256()
with archive.open('rb') as data:
    for chunk in iter(lambda: data.read(1 << 20), b''):
        digest.update(chunk)
assert digest.hexdigest() == pinned, f'{archive} is not the pinned Eden archive'

patches = sorted((root / 'tools/eden-patches').glob('*.diff'))
assert patches[-1].name == '12-indexed-storage-images.diff', [p.name for p in patches]
touched = sorted({line[6:] for p in patches for line in p.read_text().splitlines()
                  if line.startswith('+++ b/')})
# The image limits of OpenGL's pipelines, as pinned (no patch changes them).
limits = {'src/video_core/renderer_opengl/gl_graphics_pipeline.cpp': 'constexpr u32 MAX_IMAGES = 8;',
          'src/video_core/renderer_opengl/gl_compute_pipeline.cpp': 'constexpr u32 MAX_IMAGES = 16;'}
assert not set(limits) & set(touched)

with tempfile.TemporaryDirectory() as tmp:
    tree = Path(tmp) / 'eden'
    tree.mkdir()
    with tarfile.open(archive) as tar:
        for member in tar:
            parts = member.name.split('/', 1)
            if len(parts) != 2 or (parts[1] not in touched and parts[1] not in limits) or not member.isfile():
                continue
            assert member.size < 16 << 20, member.name
            target = tree / parts[1]
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source, target.open('wb') as out:
                shutil.copyfileobj(source, out)
    missing = [name for name in [*touched, *limits] if not (tree / name).is_file()]
    assert not missing, missing
    # As tools/prepare-build.sh: every patch, in name order, onto the pinned files.
    for patch in patches:
        subprocess.run(['patch', '-d', str(tree), '-p1', '-N', '-s', '-i', str(patch)], check=True)

    texture_pass = (tree / 'src/shader_recompiler/ir_opt/texture_pass.cpp').read_text()
    gl_cache = (tree / 'src/video_core/renderer_opengl/gl_shader_cache.cpp').read_text()

    # The cap is computed once per shader and applied where both storage descriptor kinds are
    # added, before the index clamp uses the count.
    assert ('    const u32 storage_dynamic_cap = DynamicStorageImageCap(host_info, '
            'DynamicStorageImageArrayCount(to_replace));') in texture_pass
    storage_case = texture_pass.split('        case IR::Opcode::ImageWrite: {', 1)[1].split('        default:', 1)[0]
    capped = storage_case.index('                count = storage_dynamic_cap;')
    assert capped < storage_case.index('index = descriptors.Add(ImageBufferDescriptor{'), 'image buffers'
    assert capped < storage_case.index('index = descriptors.Add(ImageDescriptor{'), 'images'
    assert 'EDEN_STORAGE_IMAGE_ARRAY cbuf=' in storage_case
    clamp = texture_pass.index('inst->SetArg(0, ir.UMin(ir.ShiftRightLogical(cbuf.dynamic_offset, shift), '
                               'ir.Imm32(count - 1)));')
    assert texture_pass.index('        case IR::Opcode::ImageWrite: {') < clamp
    # OpenGL's graphics pipelines hold 8 images (compute 16).
    assert '        .max_descriptor_set_storage_images = 8,' in gl_cache
    for name, value in limits.items():
        assert value in (tree / name).read_text(), (name, value)

    # The patched functions, compiled as they are.
    def function(signature):
        start = texture_pass.index(signature)
        end = texture_pass.index('\n}\n', start) + 3
        return texture_pass[start:end]
    constants = re.findall(r'^constexpr u32 (?:DESCRIPTOR_SIZE|DESCRIPTOR_MAX_COUNT|MAX_DYNAMIC_STORAGE_IMAGES) = [^;]+;',
                           texture_pass, re.M)
    assert len(constants) == 3, constants
    code = '\n'.join([
        '#include <algorithm>', '#include <bit>', '#include <cassert>', '#include <cstdint>',
        'using u32 = std::uint32_t;',
        'struct HostTranslateInfo { u32 max_descriptor_set_storage_images; };',
        *constants,
        function('u32 DynamicDescriptorCount(u32 base_offset, u32 size_shift, u32 max_descriptors) {'),
        function('u32 DynamicStorageImageCap(const HostTranslateInfo& host_info, u32 dynamic_arrays) {'),
        r'''
int main() {
    // An array whose first handle is at the start of the constant buffer, 8 bytes a handle, on a
    // host that reports Eden's default limit (OpenGL, and RADV's are larger still): 1024 elements.
    const u32 pinned = DynamicDescriptorCount(0x10, 3, DESCRIPTOR_MAX_COUNT);
    assert(pinned == 1024);
    // More than OpenGL's compute pipeline binds (64 textures + 16 images), which the console hit.
    assert(pinned > 64 + 16);
    const HostTranslateInfo vulkan{8388606}, opengl{8}, small{4};
    // Patched: 8 elements on either renderer, within OpenGL's 8 graphics and 16 compute images.
    assert(DynamicStorageImageCap(vulkan, 1) == 8);
    assert(DynamicStorageImageCap(opengl, 1) == 8);
    assert(std::min(pinned, DynamicStorageImageCap(opengl, 1)) <= 8);
    // Shared between arrays, never below one element, never above the host's own limit.
    assert(DynamicStorageImageCap(vulkan, 2) == 4);
    assert(DynamicStorageImageCap(vulkan, 3) == 2);
    assert(DynamicStorageImageCap(vulkan, 16) == 1);
    assert(DynamicStorageImageCap(small, 1) == 4);
    assert(DynamicStorageImageCap(HostTranslateInfo{0}, 1) == 1);
    assert(DynamicStorageImageCap(vulkan, 0) == 8);
    return 0;
}
'''])
    exe = Path(tmp) / 'cap'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable',
                    '-fsanitize=address,undefined', '-x', 'c++', '-', '-o', str(exe)],
                   input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)

    # The console's message, from the pinned count in OpenGL's compute views (where Boost is).
    boost_dirs = [Path(p) for p in ('/usr/include', '/usr/local/include', '/opt/homebrew/include')]
    cache = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache')
    boost_dirs += [h.parents[2] for h in sorted(cache.glob(
        'ps5-eden-headless.*/source/.cache/cpm/boost*/*/boost/container/static_vector.hpp'))]
    boost = next((d for d in boost_dirs if (d / 'boost/container/static_vector.hpp').is_file()), None)
    built = False
    if boost:
        overflow = r'''
#include <boost/container/static_vector.hpp>
#include <cstdio>
#include <cstring>
int main() {
    boost::container::static_vector<int, 64 + 16> views;  // gl_compute_pipeline.cpp
    try {
        for (int index = 0; index < 1024; ++index) views.push_back(index);
    } catch (const std::exception& error) {
        std::printf("%zu views, then: %s\n", views.size(), error.what());
        return std::strcmp(error.what(), "boost::container::bad_alloc thrown") == 0 ? 0 : 1;
    }
    return 1;
}
'''
        exe = Path(tmp) / 'overflow'
        built = subprocess.run(['c++', '-std=c++20', '-I' + str(boost), '-x', 'c++', '-', '-o', str(exe)],
                               input=overflow, text=True, capture_output=True).returncode == 0
    if built:
        shown = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip()
        assert shown == '80 views, then: boost::container::bad_alloc thrown', shown
        print(f'Indexed storage images: the pinned 1024 elements overflow OpenGL\'s views ({shown})')
    else:
        print(f'Indexed storage images: no usable Boost headers here ({boost}); the static_vector overflow was not shown')
print('Indexed storage images: PASS patch 12 applies with the set, binds at most 8 elements on either renderer '
      '(shared between arrays, within the host limit), and OpenGL\'s limit is its 8 graphics images')
