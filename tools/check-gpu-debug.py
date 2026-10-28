#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A release build applies a game's "gpu_debug" switches (settings_store.h, docs/SETTINGS.md).

headless/preferences_check.cpp checks how they are read. This checks that the session reads them
for the game it starts, outside the development-only dev-settings block, sets every switch before
the renderer is built, and writes the EDEN_GPU_DEBUG line; that their defaults are the renderer's;
that the shader compiler gets the coherent storage mode (Eden patch 10, tools/prepare-vulkan-port.py);
and that the Library never writes them.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
begin = main.index('#ifdef EDEN_DEV_PROFILE\n        // One-run A/B switches')
end = main.index('#else', begin)
release, dev = main[:begin] + main[end:], main[begin:end]

read = 'Eden::ResolveGpuDebug(Eden::LoadGpuDebug(eden_game_title_id(guest)), dev_gpu_debug);'
assert read in release, read
assert '[[maybe_unused]] Eden::GpuDebugSettings dev_gpu_debug;' in release
applied = [
    'Eden::DevVulkan::coherent_storage = gpu_debug.coherent_storage;',
    'Eden::DevVulkan::sync_submissions = gpu_debug.submit_sync != 0;',
    'Eden::DevVulkan::hcr_mode = gpu_debug.hcr;',
    'Eden::DevVulkan::compute_barriers = gpu_debug.compute_barriers != 0;',
    'Eden::DevVulkan::disable_conditional_rendering = gpu_debug.conditional_rendering == 0;',
    'std::fprintf(stderr, "%s renderer=%s\\n", Eden::GpuDebugLine(gpu_debug).c_str(),',
]
# Every session sets them again (they outlive one), before the emulator and its renderer are built.
system = release.index('Core::System system;')
for statement in applied:
    assert statement in release, statement
    assert release.index(read) < release.index(statement) < system, statement
# dev-settings names the same five and goes first.
for name in ('coherent_storage', 'submit_sync', 'hcr', 'compute_barriers', 'conditional_rendering'):
    assert f'dev_gpu_debug.{name} = ' in dev, name

# The defaults are what the renderer has without them (headless/dev_vulkan.h).
store = (root / 'headless/settings_store.h').read_text()
vulkan = (root / 'headless/dev_vulkan.h').read_text()
assert 'inline constexpr GpuDebugSettings kGpuDebugDefaults{0, 0, 1, 1, 1};' in store
for declaration in ('inline int coherent_storage = 0;', 'inline bool sync_submissions = false;',
                    'inline int hcr_mode = 1;', 'inline bool compute_barriers = true;',
                    'inline bool disable_conditional_rendering = false;'):
    assert declaration in vulkan, declaration
fields = re.search(r'struct GpuDebugSettings \{(.*?)\};', store, re.S).group(1)
assert re.findall(r'int (\w+) = -1;', fields) == ['coherent_storage', 'submit_sync', 'hcr', 'compute_barriers',
                                                   'conditional_rendering']

# The mode's names are Shader::CoherentStorage's values, in order (Eden patch 10).
assert 'inline constexpr const char* kCoherentStorageKeys[] = {"atomics", "compute", "all"};' in store
patch = (root / 'tools/eden-patches/10-coherent-storage.diff').read_text()
enum = re.search(r'\+enum class CoherentStorage : u32 \{\n(.*?)\n\+\};', patch, re.S).group(1)
assert re.findall(r'\+    (\w+),', enum) == ['Atomics', 'Compute', 'All'], enum
assert '+    CoherentStorage coherent_storage{CoherentStorage::Atomics};' in patch
port = (root / 'tools/prepare-vulkan-port.py').read_text()
assert ("'    profile.coherent_storage = static_cast<Shader::CoherentStorage>(\\n'\n"
        "     '        std::clamp(::Eden::DevVulkan::coherent_storage, 0, 2));\\n'") in port
# The cache operation of LDG/STG reaches the backend (Eden patch 09).
ldg = (root / 'tools/eden-patches/09-ldg-cache-ops.diff').read_text()
for line in ('+    return {.coherent = cache == LoadCache::CG || cache == LoadCache::CV};',
             '+    return {.coherent = cache == StoreCache::CG || cache == StoreCache::WT};',
             '+        &*block.PrependNewInst(it, new_opcode, {storage_index, offset}, inst.Flags<u32>())};',
             '+        return ctx.OpLoad(result_type, pointer, spv::MemoryAccessMask::Volatile);'):
    assert line in ldg, line

# The Library neither shows nor writes them: SaveGameSettings leaves "gpu_debug" as it is.
save = store.split('inline bool SaveGameSettings(', 1)[1].split('\n}\n', 1)[0]
assert 'gpu_debug' not in save
services = (root / 'headless/prosperoeden/eden_services.cpp').read_text()
assert 'gpu_debug' not in services and 'GpuDebug' not in services

# Documented, each switch, with the game's title ID as the file has it (capitals).
docs = (root / 'docs/SETTINGS.md').read_text()
section = docs.split('## For testing a game that stops the GPU', 1)[1].split('\n## ', 1)[0]
for name in ('coherent_storage', 'submit_sync', 'hcr', 'compute_barriers', 'conditional_rendering',
             '"0100606026F12000": { "gpu_debug": {', 'EDEN_GPU_DEBUG coherent='):
    assert name in section, name
print('GPU debug switches: a game\'s "gpu_debug" reaches the renderer and the shader compiler before the '
      'game loads, under dev-settings, with the renderer\'s defaults, logged, and the Library keeps it PASS')
