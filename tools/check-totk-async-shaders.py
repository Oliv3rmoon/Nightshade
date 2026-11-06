#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tears of the Kingdom's asynchronous shaders, the rules for a draw whose pipeline is not built, and
what stderr.log says about both.

1. headless/title_defaults.h: a listed game (Tears of the Kingdom) has asynchronous shaders on
   unless its own Performance switch says otherwise; any other game has what Settings and its own
   switch say. The session applies it after reading the settings and before Safe Launch, and says so.
2. The launcher shows the default on the game's Performance row, and its video profiles keep it
   (tools/check-video-profiles.py checks the tiers).
3. headless/async_shaders.h counts the decisions and reports them every 5 s when they changed;
   headless/pipeline_identity.h hashes each stage's SPIR-V for the EDEN_PIPELINE line.
4. tools/analyze-pipeline-reuse.py on a made-up stderr.log.
5. With Eden's source as the argument: prepare-vulkan-port.py's PipelineCache::BuiltPipeline is
   compiled against stand-ins for the registers it reads and run through every case: built, async
   off, a small target (Eden PR #4639), depth (upstream Eden), a small draw, anything else; and the
   SPIR-V hashes are noted per stage and taken by the GraphicsPipeline that prints them.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
headless = root / 'headless'
compiler = next((c for c in ('clang++-18', 'clang++', 'c++') if shutil.which(c)), None)
if compiler is None:
    sys.exit('check-totk-async-shaders: no C++ compiler')
TOTK = '0x0100F2C0115B6000'


def compile_and_run(folder, name, source, includes=()):
    path = Path(folder) / (name + '.cpp')
    path.write_text(source)
    binary = Path(folder) / name
    flags = [compiler, '-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-I' + str(headless)]
    flags += ['-I' + str(include) for include in includes]
    subprocess.run(flags + [str(path), '-o', str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True, check=True)


# 1. The session: the default after the settings are read, before Safe Launch and the switch.
defaults = (headless / 'title_defaults.h').read_text()
assert f'    {TOTK}, // The Legend of Zelda: Tears of the Kingdom' in defaults
main = (headless / 'main.cpp').read_text()
read = main.index('auto speed = Eden::LoadPerformance(eden_game_title_id(guest));')
default = main.index('const auto async_shaders = Eden::TitleDefaults::AsyncShadersFor(', read)
safe = main.index('if (Eden::SafeLaunch::On()) Eden::SafeLaunch::Apply(speed);', read)
switch = main.index('Settings::values.use_asynchronous_shaders = speed.async_shaders;', read)
reset = main.index('Eden::AsyncShaders::Reset(speed.async_shaders);', read)
assert read < default < safe < switch < reset
assert 'Eden::LoadGameSettings(eden_game_title_id(guest)).performance[1]);' in main[default:safe]
assert 'speed.async_shaders = async_shaders.on;' in main[default:safe]
assert 'static_assert(std::string_view{Eden::kPerformanceKeys[1]} == "async_shaders");' in main[read:default]
said = main.index('else if (async_shaders.by_title)', safe)
assert said < switch and 'Asynchronous shaders on: this game\'s own default' in main[said:switch]
assert main.count('Settings::values.use_asynchronous_shaders =') == 1
# A timing run in every build: cost-run.txt outside the development-only block.
cost = main.index('Eden::Performance::vulkan_cost_enabled = std::filesystem::exists(Eden::AppFile("cost-run.txt"));')
assert cost < main.index('#ifdef EDEN_DEV_VULKAN\n        const bool performance_run')
assert main.count('vulkan_cost_enabled = std::filesystem::exists') == 1
# 2. The launcher: the Performance row shows what the game runs with.
options = (headless / 'prosperoeden/pe/ui/game_options.cpp').read_text()
assert '#include "../../../title_defaults.h"' in options
assert 'on_off(title_default || *performance_switch(prefs, i))' in options
# 3. The report: every 5 s with the frame report.
graphics = (headless / 'graphics.cpp').read_text()
frame = graphics.index('Eden::Performance::ReportVulkan();')
assert graphics.index('Eden::AsyncShaders::Report(stderr);', frame) - frame < 300

with tempfile.TemporaryDirectory(prefix='eden-totk-async-') as folder:
    result = compile_and_run(folder, 'defaults', r'''
#include "title_defaults.h"
#include "async_shaders.h"
#include "pipeline_identity.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace Eden;
int main() {
    constexpr std::uint64_t totk = 0x0100F2C0115B6000ULL, other = 0x0100000000010000ULL;
    // Tears of the Kingdom: on without a switch of its own, whatever Settings says.
    for (const bool settings : {false, true}) {
        const auto chosen = TitleDefaults::AsyncShadersFor(totk, settings, -1);
        assert(chosen.on && chosen.by_title);
    }
    // Its own switch wins, on or off.
    assert(!TitleDefaults::AsyncShadersFor(totk, true, 0).on && !TitleDefaults::AsyncShadersFor(totk, true, 0).by_title);
    assert(TitleDefaults::AsyncShadersFor(totk, false, 1).on && !TitleDefaults::AsyncShadersFor(totk, false, 1).by_title);
    // Another game, or none: Settings' value with the game's own over it, as before.
    assert(!TitleDefaults::AsyncShadersFor(other, false, -1).on && TitleDefaults::AsyncShadersFor(other, true, -1).on);
    assert(!TitleDefaults::AsyncShadersFor(other, true, 0).on && !TitleDefaults::AsyncShadersFor(other, true, -1).by_title);
    assert(!TitleDefaults::AsyncShadersListed(0) && TitleDefaults::AsyncShadersListed(totk));

    // The counts: a line when they changed, none when they did not; a new session starts at 0.
    AsyncShaders::Reset(true);
    assert(!AsyncShaders::Report(stdout));
    AsyncShaders::Note(AsyncShaders::skipped);
    AsyncShaders::Note(AsyncShaders::skipped);
    AsyncShaders::Note(AsyncShaders::waited_small_target);
    assert(AsyncShaders::Report(stdout) && !AsyncShaders::Report(stdout));
    AsyncShaders::Note(AsyncShaders::skipped_depth);
    assert(AsyncShaders::Report(stdout));
    AsyncShaders::Reset(false);
    AsyncShaders::Note(AsyncShaders::waited_sync);
    assert(AsyncShaders::Report(stdout));

    // SPIR-V hashes: the same code the same hash, other code another, never 0; noted per stage on
    // this thread, taken once.
    const std::vector<std::uint32_t> a{0x07230203, 1, 2, 3}, b{0x07230203, 1, 2, 4};
    using PipelineIdentity::SpirvHash;
    assert(SpirvHash(a) == SpirvHash(std::vector<std::uint32_t>(a)) && SpirvHash(a) != SpirvHash(b));
    assert(SpirvHash(std::vector<std::uint32_t>{}) != 0);
    PipelineIdentity::NoteSpirv(4, b);
    PipelineIdentity::BeginSpirv();  // a new pipeline: nothing from the last one
    PipelineIdentity::NoteSpirv(0, a);
    PipelineIdentity::NoteSpirv(4, b);
    PipelineIdentity::NoteSpirv(9, b);  // no such stage: ignored
    const auto taken = PipelineIdentity::TakeSpirv();
    assert(taken[0] == SpirvHash(a) && taken[4] == SpirvHash(b) && taken[1] == 0 && taken[2] == 0 && taken[3] == 0);
    assert(PipelineIdentity::TakeSpirv() == PipelineIdentity::SpirvHashes{});
}
''')
    assert result.stdout.splitlines() == [
        'EDEN_ASYNC_SHADERS on=1 skipped=2 skipped_depth=0 waited_small_target=1 waited_small_draw=0 waited_sync=0',
        'EDEN_ASYNC_SHADERS on=1 skipped=2 skipped_depth=1 waited_small_target=1 waited_small_draw=0 waited_sync=0',
        'EDEN_ASYNC_SHADERS on=0 skipped=0 skipped_depth=0 waited_small_target=0 waited_small_draw=0 waited_sync=1',
    ], result.stdout

    # 4. The analysis of a stderr.log: the cache's pipelines, then three built while playing.
    log = Path(folder) / 'stderr.log'
    log.write_text('\n'.join([
        'EDEN_PIPELINE vk=0x1 graphics key=1 va=0 vb=a tc=0 te=0 gs=0 fs=b spv=vs:1,fs:2',
        'EDEN_PIPELINE vk=0x2 graphics key=2 va=0 vb=c tc=0 te=0 gs=0 fs=d spv=vs:3,fs:4',
        'EDEN_PIPELINE vk=0x3 compute cs=e',
        'EDEN_SHADER_START mode=background pipelines=3',
        'EDEN_PROGRESS cores=idle',
        'EDEN_PIPELINE vk=0x4 graphics key=3 va=0 vb=a tc=0 te=0 gs=0 fs=b spv=vs:1,fs:2',
        'EDEN_PIPELINE vk=0x5 graphics key=4 va=0 vb=a tc=0 te=0 gs=0 fs=b spv=vs:5,fs:2',
        'EDEN_PIPELINE vk=0x6 graphics key=5 va=0 vb=a tc=0 te=0 gs=0 fs=d spv=vs:1,fs:6',
        'EDEN_PROGRESS cores=idle',
        'EDEN_PIPELINE vk=0x7 graphics key=6 va=0 vb=f tc=0 te=0 gs=0 fs=b spv=vs:7,fs:2',
        'EDEN_PIPELINE vk=0x8 graphics key=7 va=0 vb=g tc=0 te=0 gs=0 fs=h spv=vs:8,fs:9',
        'EDEN_ASYNC_SHADERS on=1 skipped=4 skipped_depth=1 waited_small_target=0 waited_small_draw=2 waited_sync=0',
    ]) + '\n')
    analysis = subprocess.run([sys.executable, '-I', str(root / 'tools/analyze-pipeline-reuse.py'), str(log)],
                              capture_output=True, text=True, check=True).stdout
    for expected in ('graphics pipelines from the cache: 2', 'graphics pipelines built while playing: 5',
                     'compute pipelines: 1', 'most in one 30 s window: 3', 'spirv seen: 1 (20%)',
                     'pair seen: 1 (20%)', 'stages seen: 1 (20%)', 'one new: 1 (20%)', 'both new: 1 (20%)',
                     'last EDEN_ASYNC_SHADERS: EDEN_ASYNC_SHADERS on=1 skipped=4'):
        assert expected in analysis, (expected, analysis)

    # 5. The port's BuiltPipeline and SPIR-V notes, from Eden's source.
    if len(sys.argv) > 1:
        port = Path(folder) / 'port'
        port.mkdir()
        subprocess.run([sys.executable, '-B', str(root / 'tools/prepare-vulkan-port.py'), sys.argv[1], str(port)],
                       check=True, stdout=subprocess.DEVNULL)
        cache = (port / 'vk_pipeline_cache_cost.cpp').read_text()
        graphics_pipeline = (port / 'vk_graphics_pipeline_cost.cpp').read_text()
        assert '#include "async_shaders.h"\n#include "pipeline_identity.h"\n' in cache
        begin = cache.index('GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {')
        function = cache[begin:cache.index('\n}\n', begin) + 3]
        # The SPIR-V of each stage noted as it is written, after the last pipeline's are cleared.
        create = cache.index('std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(\n    ShaderPools& pools')
        begin_spirv = cache.index('::Eden::PipelineIdentity::BeginSpirv();', create)
        note = cache.index('::Eden::PipelineIdentity::NoteSpirv(stage_index, code);', create)
        made = cache.index('return std::make_unique<GraphicsPipeline>(', create)
        assert create < begin_spirv < cache.index('for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index)', create)
        assert cache.index('device.SaveShader(code);', create) < note < made
        assert cache.count('::Eden::PipelineIdentity::NoteSpirv(') == 1
        # Taken where the pipeline is constructed, on that thread; printed once it exists.
        taken = graphics_pipeline.index('eden_spirv = ::Eden::PipelineIdentity::TakeSpirv()] {')
        printed = graphics_pipeline.index('::Eden::PipelineIdentity::Graphics(*pipeline, key.Hash(), key.unique_hashes, eden_spirv);')
        assert taken < printed
        result = compile_and_run(folder, 'built', r'''
#include "async_shaders.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
struct GraphicsPipeline {
    bool built = false;
    bool IsBuilt() const { return built; }
};
struct Maxwell {
    struct RenderTarget { u32 width = 0, height = 0; };
    struct { std::array<RenderTarget, 8> rt{}; u32 zeta_enable = 0; } regs;
    struct { struct { struct { u32 count = 0; } index_buffer, vertex_buffer; } draw_state; } draw_manager;
};
struct PipelineCache {
    bool use_asynchronous_shaders = true;
    Maxwell* maxwell3d = nullptr;
    GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;
};
''' + function + r'''
int main() {
    namespace Async = Eden::AsyncShaders;
    Maxwell m;
    PipelineCache cache;
    cache.maxwell3d = &m;
    GraphicsPipeline built{true}, building{false};
    const auto scene = [&] {
        m.regs.rt[0] = {1920, 1080};
        m.regs.zeta_enable = 0;
        m.draw_manager.draw_state.index_buffer.count = 3000;
        m.draw_manager.draw_state.vertex_buffer.count = 3000;
    };
    Async::Reset(true);
    scene();
    assert(cache.BuiltPipeline(&built) == &built);           // built: drawn, nothing counted
    assert(cache.BuiltPipeline(&building) == nullptr);       // a scene's draw: skipped
    m.regs.rt[0] = {256, 256};                               // a small target: waited (PR #4639)
    assert(cache.BuiltPipeline(&building) == &building);
    m.regs.zeta_enable = 1;                                  // even with depth
    assert(cache.BuiltPipeline(&building) == &building);
    m.regs.rt[0] = {0, 0};                                   // no colour target: not small
    assert(cache.BuiltPipeline(&building) == nullptr);       // depth: skipped (upstream Eden)
    scene();
    m.regs.zeta_enable = 1;
    m.draw_manager.draw_state.index_buffer.count = 6;        // small, with depth: skipped
    assert(cache.BuiltPipeline(&building) == nullptr);
    m.regs.zeta_enable = 0;                                  // small, no depth: waited
    assert(cache.BuiltPipeline(&building) == &building);
    m.regs.rt[0] = {640, 360};                               // 640x360 itself is not small
    m.draw_manager.draw_state.index_buffer.count = 3000;
    assert(cache.BuiltPipeline(&building) == nullptr);
    cache.use_asynchronous_shaders = false;                  // off: always waited
    assert(cache.BuiltPipeline(&building) == &building);
    assert(cache.BuiltPipeline(&built) == &built);
    assert(Async::Report(stdout));
}
''')
        assert result.stdout.strip() == ('EDEN_ASYNC_SHADERS on=1 skipped=2 skipped_depth=2 waited_small_target=2 '
                                         'waited_small_draw=1 waited_sync=1'), result.stdout
        print('the port\'s BuiltPipeline: built drawn, a scene skipped, a small target waited (Eden PR #4639), '
              'depth skipped (upstream Eden), a small draw waited, async off waited; SPIR-V noted per stage')
print('Tears of the Kingdom has asynchronous shaders on unless its own switch says otherwise; the decisions '
      'are counted in EDEN_ASYNC_SHADERS and each stage\'s SPIR-V hash is on EDEN_PIPELINE PASS')
