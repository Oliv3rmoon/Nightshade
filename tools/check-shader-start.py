#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The shader cache's load while a game plays (headless/shader_start.h, tools/prepare-vulkan-port.py).

1. headless/shader_start_check.cpp drives the load's shared state as Eden's Vulkan pipeline cache
   does: a reader, builders on a pool, a GPU thread that takes or builds pipelines and saves what
   it builds, run to the end and stopped in the middle, many times, under the thread sanitizer and
   under the address and undefined-behaviour sanitizers.
2. With an argument, the Vulkan port's pipeline cache as prepare-vulkan-port.py writes it: either a
   folder that has it (the native build's), or Eden's source, which is prepared here first. Checked
   there: the cache stops its load before anything else when it goes and before it loads another
   game's; the GPU thread takes a pipeline from the load before building one; its saves go through
   the load; the builds are timed and counted only as builds of the cache; the driver is asked how
   it made each one; the report is written at the end.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
compiler = next((c for c in ('clang++-18', 'clang++', 'c++') if shutil.which(c)), None)
if compiler is None:
    sys.exit('check-shader-start: no C++ compiler')

with tempfile.TemporaryDirectory(prefix='eden-shader-start-') as work:
    work = pathlib.Path(work)
    for sanitizer, rounds in (('thread', '400'), ('address,undefined', '600')):
        binary = work / ('check-' + sanitizer.split(',')[0])
        subprocess.run([compiler, '-std=c++20', '-O1', '-g', '-pthread', f'-fsanitize={sanitizer}',
                        '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'headless'),
                        str(root / 'headless/shader_start_check.cpp'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary), rounds], capture_output=True, text=True, timeout=900)
        sys.stdout.write(f'[{sanitizer}] ' + result.stdout)
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-4000:])
            sys.exit(f'shader start check failed under {sanitizer}: exit {result.returncode}')

    if len(sys.argv) > 1:
        given = pathlib.Path(sys.argv[1])
        if (given / 'vk_pipeline_cache_cost.cpp').exists():
            port = given
        else:
            port = work / 'port'
            port.mkdir()
            subprocess.run([sys.executable, '-B', str(root / 'tools/prepare-vulkan-port.py'), str(given), str(port)],
                           check=True, stdout=subprocess.DEVNULL)
        cache = (port / 'vk_pipeline_cache_cost.cpp').read_text()
        graphics = (port / 'vk_graphics_pipeline_cost.cpp').read_text()
        compute = (port / 'vk_compute_pipeline_cost.cpp').read_text()
        wrapper = (port / 'vulkan_wrapper.cpp').read_text()

        def section(text, begin, end):
            start = text.index(begin)
            return text[start:text.index(end, start)]

        destructor = section(cache, 'PipelineCache::~PipelineCache() {', '\n}\n')
        assert destructor.index('EdenStopLoad(this);') < destructor.index('serialization_thread.WaitForRequests();')
        load = section(cache, 'void PipelineCache::LoadDiskResources(', 'void PipelineCache::QueueVulkanPipelineCacheFlush()')
        # A previous load ends before the cache files change; a load before the game starts ends whole.
        assert load.index('EdenStopLoad(this);') < load.index('pipeline_cache_filename = base_dir / "vulkan.bin";')
        assert load.rstrip().endswith('EdenStopLoad(this);\n}')
        assert 'load->reader = std::jthread(std::move(read_and_build));' in load
        assert 'stop_with_caller{stop_loading, [loader] { loader->Stop(); }}' in load
        # The builders touch none of the GPU thread's caches; they offer to the load.
        builds = section(load, 'auto load_compute{', 'auto read_and_build')
        assert 'graphics_cache' not in builds and 'compute_cache' not in builds and 'callback' not in builds
        assert builds.count('const EdenBuildDone done{*loader};') == 2
        assert builds.count('const ::Eden::ShaderStart::Job timing;') == 2
        assert 'loader->OfferGraphics(key, hash, std::move(pipeline));' in builds
        assert 'loader->OfferCompute(key, hash, std::move(pipeline));' in builds
        # The key filters of the file stay as the port makes them (dynamic state, static vertex input).
        assert 'EdenAttributelessStaticKey(key.state)' in builds
        # Each built on the builder that read it, whole (not handed to another worker).
        assert 'CreateComputePipeline(pools, key, env_, statistics, false)' in builds
        assert 'CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), statistics, false)' in builds
        # The GPU thread: from the load first.
        slow = section(cache, 'GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {', '\n}\n')
        assert slow.index('loader->TakeGraphics(graphics_key, graphics_key.Hash());') < slow.index('pipeline = CreateGraphicsPipeline();')
        current = section(cache, 'ComputePipeline* PipelineCache::CurrentComputePipeline() {', '\n}\n')
        assert current.index('loader->TakeCompute(key, key.Hash());') < current.index('pipeline = CreateComputePipeline(key, shader);')
        assert 'serialization_thread.QueueWork([this, key' not in cache
        assert cache.count('EdenSave(this, serialization_thread,') == 2
        # The stages, and the driver's account of each pipeline.
        for stage in ('cfg', 'ir', 'spirv', 'shader_module'):
            assert f'::Eden::ShaderStart::Clock' in cache and f'{{::Eden::ShaderStart::{stage}}}' in cache, stage
        for text in (graphics, compute):
            assert text.index('::Eden::ShaderStart::Clock eden_layout{::Eden::ShaderStart::layout};') < \
                text.index('eden_layout.Stop();') < text.index('auto func{[this, shader_notify')
        assert wrapper.count('EdenCreatePipeline(ci, [&](const Vk') == 2
        assert 'VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT' in wrapper
        assert '::Eden::ShaderStart::Report(stderr, summary, eden_heap);' in load
        print('Shader cache load in the Vulkan port: stopped first when the cache goes, the GPU thread takes from '
              'the load before building, saves through the load, every stage and the driver accounted PASS')
