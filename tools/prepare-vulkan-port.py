"""Adapt the pinned Eden Vulkan platform boundary and required renderer fixes."""
from pathlib import Path
import struct
import subprocess
import sys
source, output = map(Path, sys.argv[1:])
port = Path(__file__).resolve().parents[1] / 'headless'
def adapt(relative, destination, replacements):
    text = (source / relative).read_text()
    for old, new in replacements:
        if text.count(old) != 1:
            raise RuntimeError(f"Pinned Vulkan anchor changed: {relative}: {old[:60]}")
        text = text.replace(old, new)
    path = output / destination
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)

adapt('src/core/frontend/emu_window.h', 'include/core/frontend/emu_window.h',
      [('    Xcb,', '    Xcb,\n    PS5,')])
adapt('src/video_core/renderer_vulkan/vk_rasterizer.cpp', 'vulkan_rasterizer.cpp', [
    # Compute synchronization (dev_vulkan.h compute_barriers): indirect
    # dispatches returned before the pre-dispatch barrier, and no barrier made a dispatch's
    # writes visible to later indirect arguments, vertex/index fetch, shaders or copies.
    ('void RasterizerVulkan::DispatchCompute() {',
     """namespace {
constexpr VkMemoryBarrier COMPUTE_WRITE_BARRIER{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .pNext = nullptr,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                     VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
};
constexpr VkMemoryBarrier COMPUTE_INPUT_BARRIER{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .pNext = nullptr,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
};
constexpr VkPipelineStageFlags COMPUTE_CONSUMER_STAGES =
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_TRANSFER_BIT;
} // namespace

void RasterizerVulkan::DispatchCompute() {"""),
    ('        scheduler.RequestOutsideRenderPassOperationContext();\n        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),',
     """        scheduler.RequestOutsideRenderPassOperationContext();
        if (::Eden::DevVulkan::compute_barriers) {
            scheduler.Record([](vk::CommandBuffer cmdbuf) {
                cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                                       0, COMPUTE_INPUT_BARRIER);
            });
        }
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),"""),
    ('            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);\n',
     """            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
            if (::Eden::DevVulkan::compute_barriers)
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, COMPUTE_CONSUMER_STAGES, 0,
                                       COMPUTE_WRITE_BARRIER);
"""),
    ('        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);\n',
     """        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
        if (::Eden::DevVulkan::compute_barriers)
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, COMPUTE_CONSUMER_STAGES, 0,
                                   COMPUTE_WRITE_BARRIER);
"""),
    # CounterEnable below starts a query immediately. A deferred clear has no
    # active pass yet, so later realization would end that query in a different
    # render-pass scope (VUID-vkCmdEndQuery-None-07007). Keep the immediate path
    # when the guest enables occlusion counting.
    ('const bool can_defer_clear = ENABLE_DEFERRED_CLEAR &&',
     'const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && !regs.zpass_pixel_count_enable &&'),
    ('#include <algorithm>', '#include <algorithm>\n#include "performance.h"'),
    # Guest threads take the cache locks here (tracked-page writes, flush-area reads).
    ('bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {',
     'bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {\n    ::Eden::Performance::Timer guest_write_timer(::Eden::Performance::guest_cpu_write);'),
    ('VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {',
     'VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {\n    ::Eden::Performance::Timer guest_read_timer(::Eden::Performance::guest_cpu_read);\n'
     # A read of memory no image was ever registered in: the texture cache would find none and
     # change nothing, so its lock (held by the GPU thread through every draw) is not taken
     # (headless/image_pages.h).
     '    if (::Eden::ImagePages::NeverHeld(addr, size)) {\n'
     '        return VideoCore::RasterizerDownloadArea{\n'
     '            .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),\n'
     '            .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),\n'
     '            .preemtive = true,\n'
     '        };\n'
     '    }'),
    # ... and wait for those locks spinning briefly before sleeping (performance.h GuestCacheLock).
    ('        std::scoped_lock lock{texture_cache.mutex};\n        auto area = texture_cache.GetFlushArea(addr, size);',
     '        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);\n'
     '        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};\n        auto area = texture_cache.GetFlushArea(addr, size);'),
    ('        std::scoped_lock lock{buffer_cache.mutex};\n        if (buffer_cache.OnCPUWrite(addr, size)) {',
     '        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);\n'
     '        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};\n        if (buffer_cache.OnCPUWrite(addr, size)) {'),
    ('        std::scoped_lock lock{texture_cache.mutex};\n        texture_cache.WriteMemory(addr, size);\n    }\n    pipeline_cache.InvalidateRegion(addr, size);',
     '        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);\n'
     '        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};\n        texture_cache.WriteMemory(addr, size);\n    }\n    pipeline_cache.InvalidateRegion(addr, size);'),
    # The upstream code has an __ANDROID__ ifdef for DRAWS_TO_DISPATCH (512 vs 4096) and
    # CHECK_MASK (3 vs 7). Neither branch applies: replace the whole block. 512 draws per
    # command buffer submission: 4,096 exceeds the PS5 GPU watchdog timeout in shader-heavy
    # scenes (VK_ERROR_DEVICE_LOST in one game's cities). CHECK_MASK is the configurable
    # dispatch interval (dev-settings dispatch_draws=N); default 64 draws between hand-offs.
    ('#ifdef __ANDROID__\n    static constexpr u32 DRAWS_TO_DISPATCH = 512;\n    static constexpr u32 CHECK_MASK = 3;\n#else\n    static constexpr u32 DRAWS_TO_DISPATCH = 4096;\n    static constexpr u32 CHECK_MASK = 7;\n#endif // __ANDROID__\n\n    static_assert(DRAWS_TO_DISPATCH % (CHECK_MASK + 1) == 0);\n',
     '    static constexpr u32 DRAWS_TO_DISPATCH = 512;\n    const u32 CHECK_MASK = ::Eden::Performance::dispatch_mask.load(std::memory_order_relaxed);\n    recorded_tick = scheduler.CurrentTick();\n\n'),
    # A guest fence with something to wait for submits twice in a row: its own Flush
    # (InnerFence::Queue, vk_fence_manager.cpp), then the fence manager's FlushCommands, which
    # asks only whether a draw was counted since the counter was last cleared - and a fence's
    # Flush does not clear it. The second submission holds nothing: a quarter of a millisecond
    # of the worker thread in the driver, and a large open-world game makes seven of them a
    # frame (24 submissions a frame, 7 of them empty). FlushCommands now submits only when
    # nothing has been submitted since the last draw was recorded (FlushWork notes the
    # scheduler's tick at every draw, clear and dispatch).
    ('void RasterizerVulkan::FlushCommands() {\n    if (draw_counter == 0) {\n        return;\n    }\n    draw_counter = 0;\n    scheduler.Flush();\n}',
     'namespace {\n// The scheduler\'s tick when the last draw, clear or dispatch was recorded (GPU thread).\nu64 recorded_tick = 0;\n} // namespace\n\nvoid RasterizerVulkan::FlushCommands() {\n    if (draw_counter == 0) {\n        return;\n    }\n    draw_counter = 0;\n    if (scheduler.CurrentTick() != recorded_tick) {\n        return; // a submission has taken every draw already\n    }\n    scheduler.Flush();\n}'),
    # Where the ~4 us a draw spends goes: six phases under one running clock, each Mark taking the
    # gap since the previous one so they do not overlap. The Timer still wraps the whole function,
    # so a draw that returns early is charged for its unmarked tail and the sum of the six falls
    # short by that amount plus two clock reads per phase. control/scheduler.cpp aside, this is the
    # only place the dispatch's time can be spent - so its six numbers should add up to it.
    ('''    FlushWork();
    gpu_memory->FlushCaching();

    GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    if (!pipeline->Configure(is_indexed))
        return;

    UpdateDynamicStates();

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    draw_func();''',
     '''    const ::Eden::Performance::Timer rasterizer_draw_timer(::Eden::Performance::rasterizer_draw);
    auto phase = ::Eden::Performance::MarkStart();
    FlushWork();
    gpu_memory->FlushCaching();
    ::Eden::Performance::Mark(::Eden::Performance::draw_flush, phase);

    GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    ::Eden::Performance::Mark(::Eden::Performance::draw_pipeline, phase);
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    // Waiting for both cache mutexes. SetEngine between them and here is two pointer assignments,
    // and the hold itself carries on through what follows: the phases below measure it.
    ::Eden::Performance::Mark(::Eden::Performance::draw_lock_wait, phase);
    if (!pipeline->Configure(is_indexed))
        return;
    ::Eden::Performance::Mark(::Eden::Performance::draw_config, phase);

    UpdateDynamicStates();

    query_cache.NotifySegment(true);
    HandleTransformFeedback();
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    ::Eden::Performance::Mark(::Eden::Performance::draw_state, phase);
    draw_func();
    ::Eden::Performance::Mark(::Eden::Performance::draw_record, phase);'''),
    # Development: where OnCPUWrite spends its time (performance.h CpuWriteDetail).
    ("""    DEBUG_ASSERT(addr != 0 || size != 0);
    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
""",
     """    DEBUG_ASSERT(addr != 0 || size != 0);
    // A write to the page this core's last queued write covers joins it (performance.h DeferWrite).
    if (::Eden::Performance::cpu_write_defer.load(std::memory_order_relaxed) &&
        ::Eden::Performance::ExtendDeferredWrite(addr, size, ::Eden::Performance::cpu_write_core)) {
        return false;
    }
    // Under the buffer cache lock: mark the buffer pages after this write CPU-modified as their own
    // first writes would (performance.h cpu_write_ahead), up to a page no buffer covers or one the
    // GPU modified.
    const auto write_ahead = [this](DAddr written, u64 written_size) {
        const unsigned pages = ::Eden::Performance::cpu_write_ahead.load(std::memory_order_relaxed);
        unsigned marked = 0;
        DAddr page = Common::AlignDown(written + written_size - 1, Core::DEVICE_PAGESIZE) + Core::DEVICE_PAGESIZE;
        for (; marked < pages; ++marked, page += Core::DEVICE_PAGESIZE)
            if (!buffer_cache.IsRegionRegistered(page, Core::DEVICE_PAGESIZE) ||
                buffer_cache.OnCPUWrite(page, Core::DEVICE_PAGESIZE))
                break;
        return marked;
    };
    if (::Eden::Performance::cpu_write_detail.load(std::memory_order_relaxed)) [[unlikely]] {
        using Detail = ::Eden::Performance::CpuWriteDetail;
        Detail& stats = ::Eden::Performance::cpu_write_stats[::Eden::Performance::cpu_write_core & 3];
        long long mark = ::Eden::Performance::NowNs();
        const auto lap = [&](std::atomic<unsigned long long> Detail::*total) {
            const long long now = ::Eden::Performance::NowNs();
            (stats.*total).fetch_add(static_cast<unsigned long long>(now - mark), std::memory_order_relaxed);
            mark = now;
        };
        {
            ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
            const bool registered = buffer_cache.IsRegionRegistered(addr, size);
            const bool gpu_modified = buffer_cache.OnCPUWrite(addr, size);
            lap(&Detail::buffer_ns);
            (!registered ? stats.unregistered : gpu_modified ? stats.gpu_modified : stats.cpu_modified)
                .fetch_add(1, std::memory_order_relaxed);
            if (gpu_modified) {
                return true;
            }
            if (registered)
                stats.ahead_pages.fetch_add(write_ahead(addr, size), std::memory_order_relaxed);
            lap(&Detail::buffer_ns);
        }
        {
            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
            texture_cache.WriteMemory(addr, size);
            lap(&Detail::texture_ns);
        }
        pipeline_cache.InvalidateRegion(addr, size);
        lap(&Detail::shader_ns);
        return false;
    }
    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
"""),
    # A forced release blocks the GPU thread until the fence thread has retired
    # every older fence. Time only that producer-side drain.
    ('    fence_manager.WaitPendingFences(force);',
     '''    if (force) {
        ::Eden::Performance::Timer drain_timer(::Eden::Performance::gpu_fence_drain, 0, true);
        fence_manager.WaitPendingFences(force);
    } else {
        fence_manager.WaitPendingFences(force);
    }'''),
    # UpdateDynamicStates asked the cache for the current pipeline a second time
    # per draw, recomputing the same fixed-state key. Reuse PrepareDraw's result;
    # other callers keep the original lookup.
    ('template <typename Func>\nvoid RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {',
     '''namespace {
// GPU thread only: the pipeline PrepareDraw refreshed for the draw in progress.
GraphicsPipeline* prepared_pipeline = nullptr;
} // namespace

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {'''),
    ('    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};\n    // update engine as channel may be different.',
     '''    ::Eden::Performance::YieldToGuests();  // a guest thread asleep on a cache lock goes first
    ::Eden::AstcPolicy::Draw();
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    prepared_pipeline = pipeline;
    SCOPE_EXIT {
        prepared_pipeline = nullptr;
    };
    // update engine as channel may be different.'''),
    ('        if (auto* gp = pipeline_cache.CurrentGraphicsPipeline(); gp && gp->HasDynamicVertexInput()) {',
     '        if (auto* gp = prepared_pipeline ? prepared_pipeline : pipeline_cache.CurrentGraphicsPipeline();\n            gp && gp->HasDynamicVertexInput()) {'),
    # Development GPU fault probe (dev_vulkan.h, pipeline_trace=on): once the runner
    # creates /app0/sync-draws.txt, report every draw/dispatch to klog and wait for the
    # GPU, so the last report before a fatal GPU fault names the operation.
    ('#include "performance.h"', '#include "performance.h"\n#include "dev_vulkan.h"\n#include "diagnostics.h"\n#include "image_pages.h"\n#include "astc_policy.h"\n#include <filesystem>'),
    ('GraphicsPipeline* prepared_pipeline = nullptr;',
     'GraphicsPipeline* prepared_pipeline = nullptr;\n'
     '// The pipeline of the last prepared draw, for the development fault probe.\n'
     'GraphicsPipeline* probe_pipeline = nullptr;\n'
     'bool SyncProbeActive() {\n'
     '    static unsigned poll = 0;\n'
     '    static bool active = false;\n'
     '    if (!active && ::Eden::DevVulkan::trace_pipelines && (++poll & 255) == 0)\n'
     '        active = std::filesystem::exists("/app0/sync-draws.txt");\n'
     '    return active;\n'
     '}\n'
     'void SyncProbe(Scheduler& scheduler, const char* kind, const void* pipeline) {\n'
     '    if (!SyncProbeActive()) return;\n'
     '    static unsigned long long sequence = 0;\n'
     '    char text[96];\n'
     '    std::snprintf(text, sizeof(text), "%s #%llu p=%p", kind, ++sequence, pipeline);\n'
     '    ::Eden::Report("sync", text);\n'
     '    scheduler.Finish();\n'
     '}'),
    ('    prepared_pipeline = pipeline;\n', '    prepared_pipeline = pipeline;\n    probe_pipeline = pipeline;\n'),
    # Development: how long draw preparation holds the cache locks guest writes wait for.
    ('    probe_pipeline = pipeline;\n',
     '    probe_pipeline = pipeline;\n'
     '    const long long hold_start = ::Eden::Performance::cpu_write_detail.load(std::memory_order_relaxed)\n'
     '        ? ::Eden::Performance::NowNs() : 0;\n'
     '    SCOPE_EXIT {\n'
     '        if (hold_start != 0) ::Eden::Performance::AddSince(::Eden::Performance::draw_cache_hold, hold_start);\n'
     '    };\n'),
    ('        }\n    });\n}\n\nvoid RasterizerVulkan::DrawIndirect() {',
     '        }\n    });\n    SyncProbe(scheduler, "draw", probe_pipeline);\n}\n\nvoid RasterizerVulkan::DrawIndirect() {'),
    ('void RasterizerVulkan::DrawTexture() {', 'void RasterizerVulkan::DrawTexture() {\n    SCOPE_EXIT { SyncProbe(scheduler, "draw_texture", nullptr); };'),
    ('void RasterizerVulkan::Clear(u32 layer_count) {', 'void RasterizerVulkan::Clear(u32 layer_count) {\n    SCOPE_EXIT { SyncProbe(scheduler, "clear", nullptr); };'),
    ('    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};\n    if (!pipeline) {\n        return;\n    }',
     '    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};\n    if (!pipeline) {\n        return;\n    }\n'
     '    SCOPE_EXIT { SyncProbe(scheduler, "dispatch", pipeline); };'),
    # Guest writes while the GPU thread holds a cache lock are queued instead of waiting
    # (performance.h cpu_write_defer); the GPU thread runs them before its next draw.
    ('''    {
        ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
''',
     '''    {
        if (!::Eden::Performance::GuestCacheLockOrDefer(buffer_cache.mutex)) {
            ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
            return false;
        }
        std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
        write_ahead(addr, size);
    }
    {
        if (!::Eden::Performance::GuestCacheLockOrDefer(texture_cache.mutex)) {
            ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
            return false;
        }
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        texture_cache.WriteMemory(addr, size);
    }
'''),
    ('''            ::Eden::Performance::GuestCacheLock(buffer_cache.mutex);
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
''',
     '''            if (!::Eden::Performance::GuestCacheLockOrDefer(buffer_cache.mutex)) {
                lap(&Detail::buffer_wait_ns);
                ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
                return false;
            }
            std::lock_guard lock{buffer_cache.mutex, std::adopt_lock};
            lap(&Detail::buffer_wait_ns);
'''),
    ('''            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
''',
     '''            if (!::Eden::Performance::GuestCacheLockOrDefer(texture_cache.mutex)) {
                lap(&Detail::texture_wait_ns);
                ::Eden::Performance::DeferWrite(addr, size, ::Eden::Performance::cpu_write_core);
                return false;
            }
            std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
            lap(&Detail::texture_wait_ns);
'''),
    # Guest reads may skip the download check while the texture cache lock is held (cpu_read_skip).
    ('''        ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        auto area = texture_cache.GetFlushArea(addr, size);''',
     '''        if (::Eden::Performance::cpu_read_skip.load(std::memory_order_relaxed)) {
            if (!texture_cache.mutex.try_lock()) {
                ::Eden::Performance::cpu_read_skipped.fetch_add(1, std::memory_order_relaxed);
                // Empty: HandleRasterizerDownload does not remember it, so the next read checks again.
                return VideoCore::RasterizerDownloadArea{.start_address = addr, .end_address = addr, .preemtive = true};
            }
        } else {
            ::Eden::Performance::GuestCacheLock(texture_cache.mutex);
        }
        std::lock_guard lock{texture_cache.mutex, std::adopt_lock};
        auto area = texture_cache.GetFlushArea(addr, size);'''),
    ('        if (hold_start != 0) ::Eden::Performance::AddSince(::Eden::Performance::draw_cache_hold, hold_start);\n',
     '        if (hold_start != 0) ::Eden::Performance::AddHold(hold_start);\n'),
    # Dynamic vertex input: a stream with instancing on and frequency 0 advances per vertex, as the
    # pipeline's static vertex input (binding_divisors) and OpenGL's divisor 0 take it. Here it was
    # an instance-rate binding with divisor 0, every vertex reading element 0: geometry collapsed
    # (objects vanishing for a frame, a whole scene drawn as one flat colour).
    ('        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};\n',
     '        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding) &&\n'
     '                                input_binding.frequency != 0};\n'),
])
# Host conditional rendering (VK_EXT_conditional_rendering, dev_vulkan.h hcr_mode): count each
# decision (EDEN_DEV_GUEST cond= slots 13-19); hcr=exact sends the conditions Eden would draw
# unconditionally, or predicate from a pending host query, to the exact CPU evaluation;
# hcr=cpu evaluates every condition on the CPU and predicates from constant 0/1 buffers.
adapt('src/video_core/renderer_vulkan/vk_query_cache.cpp', 'vulkan_query_cache.cpp', [
    ('#include "video_core/vulkan_common/vulkan_wrapper.h"\n',
     '#include "video_core/vulkan_common/vulkan_wrapper.h"\n#include "dev_vulkan.h"\n#include "performance.h"\n'),
    ('    vk::Buffer hcr_resolve_buffer;\n',
     '    vk::Buffer hcr_resolve_buffer;\n    vk::Buffer hcr_constant_buffer; // hcr=cpu: predicates 0 and 1\n'),
    ('        hcr_resolve_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);\n    }\n',
     '''        hcr_resolve_buffer = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.IsExtConditionalRendering() && ::Eden::DevVulkan::hcr_mode == 2) {
            VkBufferCreateInfo constant_ci = buffer_ci;
            constant_ci.size = 2 * sizeof(u32);
            constant_ci.usage = VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
            hcr_constant_buffer = memory_allocator.CreateBuffer(constant_ci, MemoryUsage::Upload);
            const u32 predicates[2]{0, 1};
            std::memcpy(hcr_constant_buffer.Mapped().data(), predicates, sizeof(predicates));
            hcr_constant_buffer.Flush();
        }
    }
'''),
    ('''bool QueryCacheRuntime::HostConditionalRenderingCompareValue(VideoCommon::LookupData object_1,
                                                             [[maybe_unused]] bool qc_dirty) {
    if (!impl->device.IsExtConditionalRendering()) {
        return false;
    }
    HostConditionalRenderingCompareBCImpl(object_1.address, true, true);
    return true;
}''',
     '''namespace {
// hcr=cpu: Maxwell3D::ProcessQueryCondition's exact evaluation (the read flushes pending GPU
// results), then the driver predicates the draws from a constant buffer. kind: 0 Conditional,
// 1 IfEqual, 2 IfNotEqual.
bool HostConditionalRenderingOnCpu(QueryCacheRuntime& runtime, QueryCacheRuntimeImpl& impl, DAddr address,
                                   unsigned kind) {
    if (address == 0 || !impl.hcr_constant_buffer) {
        ::Eden::Performance::CountCondition(13);
        runtime.EndHostConditionalRendering();
        return false;
    }
    u32 compare[6]{};
    impl.device_memory.ReadBlock(address, compare, sizeof(compare));
    const bool equal = compare[0] == compare[4] && compare[1] == compare[5];
    const bool execute = kind == 0 ? compare[0] != 0 && compare[1] != 0 : (kind == 1 ? equal : !equal);
    ::Eden::Performance::CountCondition(execute ? 17 : 18);
    const bool was_running = impl.is_hcr_running;
    if (was_running) {
        runtime.PauseHostConditionalRendering();
    }
    impl.hcr_setup.buffer = *impl.hcr_constant_buffer;
    impl.hcr_setup.offset = execute ? sizeof(u32) : 0;
    impl.hcr_setup.flags = 0;
    impl.hcr_is_set = true;
    impl.is_hcr_running = false;
    if (was_running) {
        runtime.ResumeHostConditionalRendering();
    }
    return true;
}
} // namespace

bool QueryCacheRuntime::HostConditionalRenderingCompareValue(VideoCommon::LookupData object_1,
                                                             [[maybe_unused]] bool qc_dirty) {
    if (!impl->device.IsExtConditionalRendering()) {
        return false;
    }
    if (::Eden::DevVulkan::hcr_mode == 2) {
        return HostConditionalRenderingOnCpu(*this, *impl, object_1.address, 0);
    }
    if (::Eden::DevVulkan::hcr_mode == 1 && qc_dirty) {
        ::Eden::Performance::CountCondition(19);
        EndHostConditionalRendering();
        return false;
    }
    ::Eden::Performance::CountCondition(16);
    HostConditionalRenderingCompareBCImpl(object_1.address, true, true);
    return true;
}'''),
    ('''    if (!impl->device.IsExtConditionalRendering()) {
        return false;
    }

    const auto check_in_bc = [&](DAddr address) {''',
     '''    if (!impl->device.IsExtConditionalRendering()) {
        return false;
    }
    if (::Eden::DevVulkan::hcr_mode == 2) {
        return HostConditionalRenderingOnCpu(*this, *impl, object_1.address, equal_check ? 1 : 2);
    }

    const auto check_in_bc = [&](DAddr address) {'''),
    ('''    if (!is_in_ac[0] && !is_in_ac[1]) {
        EndHostConditionalRendering();
        return false;
    }

    if (!qc_dirty && !is_in_bc[0] && !is_in_bc[1]) {
        EndHostConditionalRendering();
        return false;
    }
''',
     '''    if (!is_in_ac[0] && !is_in_ac[1]) {
        ::Eden::Performance::CountCondition(13);
        EndHostConditionalRendering();
        return false;
    }

    if (!qc_dirty && !is_in_bc[0] && !is_in_bc[1]) {
        ::Eden::Performance::CountCondition(13);
        EndHostConditionalRendering();
        return false;
    }
    // hcr=exact: a pending host query is not in any buffer yet; the CPU evaluation waits for it.
    if (::Eden::DevVulkan::hcr_mode == 1 && qc_dirty) {
        ::Eden::Performance::CountCondition(19);
        EndHostConditionalRendering();
        return false;
    }
'''),
    ('''            HostConditionalRenderingCompareValueImpl(*objects[j], equal_check);
            return true;''',
     '''            ::Eden::Performance::CountCondition(15);
            HostConditionalRenderingCompareValueImpl(*objects[j], equal_check);
            return true;'''),
    ('''    if (!is_gpu_high) {
        EndHostConditionalRendering();
        return true;
    }

    if (!is_in_bc[0] && !is_in_bc[1]) {
        EndHostConditionalRendering();
        return true;
    }
    HostConditionalRenderingCompareBCImpl(object_1.address, equal_check);''',
     '''    // Eden draws these unconditionally; hcr=exact evaluates them on the CPU instead.
    if (!is_gpu_high || (!is_in_bc[0] && !is_in_bc[1])) {
        ::Eden::Performance::CountCondition(14);
        EndHostConditionalRendering();
        return ::Eden::DevVulkan::hcr_mode != 1;
    }
    ::Eden::Performance::CountCondition(16);
    HostConditionalRenderingCompareBCImpl(object_1.address, equal_check);'''),
])
adapt('src/video_core/renderer_vulkan/vk_swapchain.cpp', 'vulkan_swapchain.cpp', [
    # An acquisition error has no usable image index. Forward it to the existing
    # renderer recovery path instead of indexing or presenting an invalid image.
    ('LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));',
     'LOG_ERROR(Render_Vulkan, "vkAcquireNextImageKHR returned {}", string_VkResult(result));\n        vk::Check(result);'),
    ('LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));',
     'LOG_CRITICAL(Render_Vulkan, "Failed to present with error {}", string_VkResult(result));\n        vk::Check(result);'),
])
# Keep the overlay in the existing display composition pass (also used for
# frontend screenshots), excluding guest applet captures. Cache its pipeline.
# Preserve upstream ABI, including consumers whose quoted includes resolve to
# the original header. Display/capture purpose belongs to the draw call.
adapt('src/video_core/present.h', 'include/video_core/present.h', [])
adapt('src/video_core/renderer_vulkan/vk_present_manager.h',
      'include/video_core/renderer_vulkan/vk_present_manager.h', [
    ('#include <mutex>', '#include <mutex>\n#include <exception>'),
    ('    std::jthread present_thread;',
     '    std::exception_ptr present_failure;\n    std::jthread present_thread;'),
])
adapt('src/video_core/renderer_vulkan/vk_present_manager.cpp', 'vulkan_present_manager.cpp', [
    ('#include "video_core/renderer_vulkan/vk_present_manager.h"',
     '#include "video_core/renderer_vulkan/vk_present_manager.h"\n#include "gpu_failure.h"\n#include "performance.h"'),
    # GetRenderFrame runs on the GPU thread: time its free-frame and
    # present-completion waits (FIFO back-pressure on the producer).
    ('    // Wait for free presentation frames\n    std::unique_lock lock{free_mutex};',
     '    // Wait for free presentation frames\n    ::Eden::Performance::Timer present_wait_timer(::Eden::Performance::gpu_present_wait, 0, true);\n    std::unique_lock lock{free_mutex};'),
    ('PresentThread(token);', '''try {
            PresentThread(token);
        } catch (...) {
            const auto error = std::current_exception();
            {
                std::scoped_lock lock{queue_mutex, free_mutex};
                present_failure = error;
                present_queue.clear();
            }
            free_cv.notify_all();
            frame_cv.notify_all();
            Eden::RecordGpuFailure(error);
        }'''),
    ('free_cv.wait(lock, [this] { return !free_queue.empty(); });',
     '''free_cv.wait(lock, [this] { return present_failure || !free_queue.empty(); });
    if (present_failure) std::rethrow_exception(present_failure);'''),
    ('            present_queue.push_back(frame);',
     '            if (present_failure) return;\n            present_queue.push_back(frame);'),
    ('frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });',
     'frame_cv.wait(queue_lock, [this] { return present_failure || present_queue.empty(); });'),
])
# Descriptor buffers: Eden's null buffer (nullDescriptor devices) has device address 0,
# but WriteDescriptorBuffer passed it with the binding's range. RADV then builds a real
# descriptor at GPU address 0; desktop kernels only log the resulting page fault, the PS5
# kills the title (TCP read at VA 0).
# The spec requires a null pAddressInfo (or VK_WHOLE_SIZE) for a null descriptor.
# Development A/B switches for device features (headless/dev_vulkan.h, dev-settings).
adapt('src/video_core/vulkan_common/vulkan_device.cpp', 'vulkan_device.cpp', [
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "performance.h"'),
    # The allocator counts what it holds itself instead of asking the driver. The driver's figures
    # are wrong on the console: it counts each allocation of graphics memory twice (as VRAM and as
    # visible VRAM), so the allocator saw the budget of the 8 GiB heap used up at 4 GiB. From there
    # it could make no more of its 256 MiB blocks and gave every image memory of its own: 926
    # allocations where 40 blocks had done, a third of what it held unused (1.3 of 3.9 GiB in a large
    # open-world game at 2x), and the shared pool cut into hundreds of pieces. Without the driver's
    # budget its own count stands against four fifths of the heap.
    ('    if (extensions.memory_budget) {\n        flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;\n    }\n',
     '    // The driver\'s budget is not asked for: its count is double (tools/prepare-vulkan-port.py).\n'),
    # "Memory in use" for the texture and buffer caches: on the console, the budget less what is
    # left of the pool the CPU shares with the GPU (performance.h, graphics_memory_free).
    ('u64 Device::GetDeviceMemoryUsage() const {\n',
     'namespace {\n'
     '// The largest block of the shared pool that was free when the device was made: about the most\n'
     '// graphics can be given in this session.\n'
     'u64 graphics_capacity = 0;\n'
     '}\n\n'
     'u64 Device::GetDeviceMemoryUsage() const {\n'
     '    if (const auto free_memory = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed);\n'
     '        free_memory && ::Eden::Performance::graphics_usage_from_pool.load(std::memory_order_relaxed)) {\n'
     '        // The caches evict what is unused 1.6 GiB under the budget and evict hard 0.8 GiB under\n'
     '        // it. Where the pool could never give graphics 6.4 GiB, those marks become a quarter\n'
     '        // and an eighth of what it could give.\n'
     '        const double scale = graphics_capacity != 0 && graphics_capacity < 6400_MiB\n'
     '                                 ? static_cast<double>(6400_MiB) / static_cast<double>(graphics_capacity)\n'
     '                                 : 1.0;\n'
     '        const u64 free_bytes = static_cast<u64>(static_cast<double>(free_memory()) * scale);\n'
     '        return device_access_memory > free_bytes ? device_access_memory - free_bytes : 0;\n'
     '    }\n'),
    ('    extensions.descriptor_buffer = features.descriptor_buffer.descriptorBuffer;',
     '    extensions.descriptor_buffer = features.descriptor_buffer.descriptorBuffer &&\n'
     '        !::Eden::DevVulkan::disable_descriptor_buffer;'),
    ('    features.robustness2.robustBufferAccess2 = VK_FALSE;\n'
     '    features.robustness2.robustImageAccess2 = VK_FALSE;\n'
     '    extensions.robustness_2 = features.robustness2.nullDescriptor;',
     '    if (!::Eden::DevVulkan::robustness2) {\n'
     '        features.robustness2.robustBufferAccess2 = VK_FALSE;\n'
     '        features.robustness2.robustImageAccess2 = VK_FALSE;\n'
     '    }\n'
     '    if (::Eden::DevVulkan::disable_null_descriptor) {\n'
     '        features.robustness2.nullDescriptor = VK_FALSE;\n'
     '    }\n'
     '    extensions.robustness_2 = features.robustness2.nullDescriptor;'),
    ('            (queue_family_properties[*graphics].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0;',
     '            (queue_family_properties[*graphics].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0 &&\n'
     '            !::Eden::DevVulkan::disable_sparse;'),
    ('            features.custom_border_color.customBorderColorWithoutFormat;\n    }\n'
     '    RemoveExtensionFeatureIfUnsuitable(extensions.custom_border_color',
     '            features.custom_border_color.customBorderColorWithoutFormat &&\n'
     '            !::Eden::DevVulkan::disable_custom_border;\n    }\n'
     '    RemoveExtensionFeatureIfUnsuitable(extensions.custom_border_color'),
    # dev-settings conditional_rendering=off: guest render-enable conditions are resolved
    # without VK_EXT_conditional_rendering (the path Eden takes on drivers without it).
    ('    // VK_EXT_border_color_swizzle\n',
     '    if (::Eden::DevVulkan::disable_conditional_rendering) {\n'
     '        extensions.conditional_rendering = false;\n    }\n'
     '    // VK_EXT_border_color_swizzle\n'),
    # The heaps and the budget the texture/buffer caches size themselves from.
    ('            device_access_memory = std::min<u64>(device_access_memory, normal_memory + scaler_memory);\n'
     '        }\n    }\n}\n',
     '            device_access_memory = std::min<u64>(device_access_memory, normal_memory + scaler_memory);\n'
     '        }\n    }\n'
     '    if (const auto free_memory = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed)) {\n'
     '        graphics_capacity = free_memory();\n'
     '    }\n'
     '    std::printf("EDEN_VULKAN_MEMORY integrated=%d heaps=%zu local=%llu initial_usage=%llu device_access=%llu "\n'
     '                "capacity=%llu\\n",\n'
     '                int(is_integrated), valid_heap_memory.size(), static_cast<unsigned long long>(local_memory),\n'
     '                static_cast<unsigned long long>(device_initial_usage),\n'
     '                static_cast<unsigned long long>(device_access_memory),\n'
     '                static_cast<unsigned long long>(graphics_capacity));\n}\n'),
])
# Development GPU fault probe (dev_vulkan.h, submit_sync=on): wait for every
# submission, so the CPU never records against work the GPU has not finished.
GPU_TIME_HELPERS = r'''
namespace {
// GPU execution time, for the FPS overlay (performance.h, gpu_time_wanted) and for the
// development report (dev_vulkan.h gpu_time): timestamps at the start and end
// of every scheduler command buffer, summed once the GPU has run them and printed every
// 5 s as EDEN_GPU_TIME. Worker thread only.
struct GpuTimeProbe {
    static constexpr u32 SLOTS = 512;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write{};
    double period_ns{};
    u64 next{}, first_pending{};
    u32 current{};
    u64 busy_ns{}, max_ns{}, submissions{};
    std::chrono::steady_clock::time_point report{};
    bool failed{};
    bool stamped{}; // the command buffer being recorded has its first timestamp
} gpu_time;

bool GpuTimeWanted() {
    return ::Eden::DevVulkan::gpu_time || ::Eden::Performance::gpu_time_wanted.load(std::memory_order_relaxed);
}
void GpuTimeGaveUp() {
    gpu_time.failed = true;
    ::Eden::Performance::gpu_time_failed.store(true, std::memory_order_relaxed);
}

void GpuTimeBegin(const Device& device, vk::CommandBuffer cmdbuf) {
    gpu_time.stamped = false;
    if (!GpuTimeWanted() || gpu_time.failed) return;
    if (!gpu_time.write) {
        const auto& dld = device.GetDispatchLoader();
        gpu_time.write = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            dld.vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdWriteTimestamp"));
        gpu_time.period_ns = device.GetPhysical().GetProperties().limits.timestampPeriod;
        if (!gpu_time.write || gpu_time.period_ns <= 0) { GpuTimeGaveUp(); return; }
        gpu_time.pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = GpuTimeProbe::SLOTS * 2,
            .pipelineStatistics = 0,
        });
        gpu_time.report = std::chrono::steady_clock::now();
    }
    if (gpu_time.next - gpu_time.first_pending >= GpuTimeProbe::SLOTS) {
        GpuTimeGaveUp(); // results stopped arriving; never overwrite a pending slot
        std::printf("EDEN_GPU_TIME failed=ring\n");
        return;
    }
    gpu_time.current = static_cast<u32>(gpu_time.next++ % GpuTimeProbe::SLOTS);
    cmdbuf.ResetQueryPool(*gpu_time.pool, gpu_time.current * 2, 2);
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2);
    gpu_time.stamped = true;
}

void GpuTimeEnd(vk::CommandBuffer cmdbuf) {
    if (!gpu_time.stamped) return;
    gpu_time.write(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *gpu_time.pool, gpu_time.current * 2 + 1);
}

void GpuTimeCollect(const Device& device) {
    if (gpu_time.failed || !gpu_time.write) return;
    while (gpu_time.first_pending + 1 < gpu_time.next) {
        const u32 slot = static_cast<u32>(gpu_time.first_pending % GpuTimeProbe::SLOTS);
        std::array<u64, 2> stamps{};
        if (device.GetLogical().GetQueryResults(*gpu_time.pool, slot * 2, 2, sizeof(stamps), stamps.data(),
                                               sizeof(u64), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) break;
        const u64 ns = stamps[1] > stamps[0]
                           ? static_cast<u64>(double(stamps[1] - stamps[0]) * gpu_time.period_ns) : 0;
        gpu_time.busy_ns += ns;
        ::Eden::Performance::gpu_time_busy_ns.fetch_add(ns, std::memory_order_relaxed);
        ::Eden::Performance::gpu_time_results.fetch_add(1, std::memory_order_relaxed);
        gpu_time.max_ns = (std::max)(gpu_time.max_ns, ns);
        ++gpu_time.submissions;
        ++gpu_time.first_pending;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(now - gpu_time.report).count();
    if (wall >= 5'000'000'000 && ::Eden::DevVulkan::gpu_time) {
        std::printf("EDEN_GPU_TIME wall_ms=%.1f busy_ms=%.1f submissions=%llu max_ms=%.2f\n", wall / 1e6,
                    gpu_time.busy_ns / 1e6, static_cast<unsigned long long>(gpu_time.submissions),
                    gpu_time.max_ns / 1e6);
        gpu_time.busy_ns = gpu_time.max_ns = gpu_time.submissions = 0;
        gpu_time.report = now;
    }
}
} // namespace
'''
adapt('src/video_core/renderer_vulkan/vk_scheduler.cpp', 'vulkan_scheduler.cpp', [
    ('#include <memory>', '#include <memory>\n#include <array>\n#include <atomic>\n#include <chrono>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "performance.h"'),
    ('namespace Vulkan {\n', 'namespace Vulkan {\n' + GPU_TIME_HELPERS),
    ('        .pInheritanceInfo = nullptr,\n    });\n    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());',
     '        .pInheritanceInfo = nullptr,\n    });\n    GpuTimeBegin(device, current_cmdbuf);\n'
     '    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());'),
    ('        upload_cmdbuf.End();\n        cmdbuf.End();\n',
     '        upload_cmdbuf.End();\n        GpuTimeEnd(cmdbuf);\n        cmdbuf.End();\n'),
    ('            vk::Check(result);\n            break;\n        }\n    });\n    chunk->MarkSubmit();',
     '            vk::Check(result);\n            break;\n        }\n        GpuTimeCollect(device);\n    });\n    chunk->MarkSubmit();'),
    ('    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);\n'
     '    AllocateNewContext();\n    return signal_value;',
     '    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);\n'
     '    if (::Eden::DevVulkan::sync_submissions) {\n        Wait(signal_value);\n    }\n'
     '    AllocateNewContext();\n    return signal_value;'),
])
adapt('src/video_core/renderer_vulkan/pipeline_helper.h',
      'include/video_core/renderer_vulkan/pipeline_helper.h', [
    ('            VkDescriptorGetInfoEXT get_info{',
     '            const VkDescriptorAddressInfoEXT* const buffer_info{\n'
     '                entry.address.address != 0 ? &address_info : nullptr};\n'
     '            VkDescriptorGetInfoEXT get_info{'),
    ('get_info.data.pUniformBuffer = &address_info;', 'get_info.data.pUniformBuffer = buffer_info;'),
    ('get_info.data.pStorageBuffer = &address_info;', 'get_info.data.pStorageBuffer = buffer_info;'),
    ('get_info.data.pUniformTexelBuffer = &address_info;', 'get_info.data.pUniformTexelBuffer = buffer_info;'),
    ('get_info.data.pStorageTexelBuffer = &address_info;', 'get_info.data.pStorageTexelBuffer = buffer_info;'),
])
adapt('src/video_core/renderer_vulkan/vk_blit_screen.h',
      'include/video_core/renderer_vulkan/vk_blit_screen.h', [
    ('VkFormat current_swapchain_view_format);',
     'VkFormat current_swapchain_view_format, bool draw_hud = false);'),
])
adapt('src/video_core/renderer_vulkan/renderer_vulkan.h',
      'include/video_core/renderer_vulkan/renderer_vulkan.h', [
    ('    void Composite(', '    void PresentLoading();\n    void Composite('),
    ('#ifdef HAS_LSFG\n#include "video_core/renderer_vulkan/present/frame_gen.h"\n#endif',
     '#ifdef HAS_LSFG\n#include "video_core/renderer_vulkan/present/frame_gen.h"\n#endif\n'
     '#ifdef HAS_MIHAWKGEN\n#include "mihawkgen.h"\n#include "mihawkgen_bridge.h"\n#endif'),
    ('#ifdef HAS_LSFG\n    FrameGen frame_gen;\n#endif',
     '#ifdef HAS_LSFG\n    FrameGen frame_gen;\n#endif\n'
     '#ifdef HAS_MIHAWKGEN\n    Eden::MihawkGen mihawkgen_;\n    bool mihawkgen_init_{};\n#endif'),
])
adapt('src/video_core/renderer_vulkan/renderer_vulkan.cpp', 'vulkan_renderer.cpp', [
    ('#include "video_core/renderer_vulkan/renderer_vulkan.h"',
     '#include "video_core/renderer_vulkan/renderer_vulkan.h"\n#include "display_refresh.h"\n'
     '#include "diagnostics.h"\n#include "hud.h"'),
    # The overlay's resolution piece (hud.h): the guest's own picture, taken as it is handed to the
    # display. The OpenGL renderer does not say, so the piece stays off there (vulkan_loading.inc
    # does the same for the loading screen).
    ('void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {',
     'void RendererVulkan::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {\n'
     + (port / 'vulkan_guest_resolution.inc').read_text()),
    # The HUD counted the guest rate only: OnFrameDisplayed() runs once per Composite(), while the
    # frames frame generation adds are presented through PresentManager::Present() (hud.h). Count
    # every frame handed to the present manager.
    ('        present_manager.Present(generated);\n    }',
     '        present_manager.Present(generated);\n'
     '        Eden::CountPresentedFrame();\n'
     '    }'),
    # A frame the display has no refresh for is not presented: a game patched for more frames
    # than the output shows keeps its pace instead of waiting for a refresh per frame
    # (headless/display_refresh.h). The frame still ends as any other.
    ('    RenderScreenshot(framebuffers);\n    Frame* frame = present_manager.GetRenderFrame();',
     '    RenderScreenshot(framebuffers);\n'
     '    if (::Eden::Display::SkipFrame()) {\n'
     '        gpu.RendererFrameEndNotify();\n'
     '        rasterizer.TickFrame();\n'
     '        return;\n'
     '    }\n'
     '    Frame* frame = present_manager.GetRenderFrame();'),
    ('    Report();', '    PresentLoading();\n    Report();'),
    ('swapchain.GetImageViewFormat());', 'swapchain.GetImageViewFormat(), true);'),
    ('layout, 1, format);', 'layout, 1, format, true);'),
    ('void RendererVulkan::Report() const {',
     (port / 'vulkan_loading.inc').read_text() + '\nvoid RendererVulkan::Report() const {'),
    # Present() only records the hand-off into a fresh chunk. Dispatch it now so
    # the present thread gets this frame immediately instead of after the
    # producer's next 8-draw dispatch (upstream does this only with LSFG).
    ('    present_manager.Present(frame);\n#ifdef HAS_LSFG\n    scheduler.DispatchWork();\n#endif',
     '    present_manager.Present(frame);\n    Eden::CountPresentedFrame();\n'
     '    scheduler.DispatchWork();'),
    # MihawkGen neural frame generation: skip the LSFG block when MihawkGen is active,
    # and run MihawkGen's own present path instead (headless/vulkan_mihawkgen.inc).
    ('#ifdef HAS_LSFG\n    void(frame_gen.WantedGenerations',
     '#ifdef HAS_LSFG\n'
     '#ifndef HAS_MIHAWKGEN\n'
     '#define EDEN_LSFG_GATE true\n'
     '#else\n'
     '#define EDEN_LSFG_GATE (!Eden::MihawkGenBridge::active.load(std::memory_order_relaxed))\n'
     '#endif\n'
     '    if (EDEN_LSFG_GATE) {\n'
     '    void(frame_gen.WantedGenerations'),
    ('        Eden::CountPresentedFrame();\n    }\n#endif',
     '        Eden::CountPresentedFrame();\n    }\n    } // LSFG gate\n#endif\n'
     + (port / 'vulkan_mihawkgen.inc').read_text()),
    ('} // namespace Vulkan', '''} // namespace Vulkan
namespace Eden {
void PresentVulkanLoading(VideoCore::RendererBase& renderer) {
    static_cast<Vulkan::RendererVulkan&>(renderer).PresentLoading();
}
}'''),
])
adapt('src/video_core/renderer_vulkan/present/window_adapt_pass.h',
      'include/video_core/renderer_vulkan/present/window_adapt_pass.h', [
    # The overlay and the loading screen are drawn in this pass when it is given the allocator
    # their picture's memory comes from (vulkan_hud_prepare.inc).
    ('class RasterizerVulkan;\n\nclass WindowAdaptPass final {',
     'class RasterizerVulkan;\nclass MemoryAllocator;\n\nclass WindowAdaptPass final {'),
    ('const Layout::FramebufferLayout& layout, Frame* dst);',
     'const Layout::FramebufferLayout& layout, Frame* dst, MemoryAllocator* hud_memory = nullptr);'),
    ('    vk::Pipeline coverage_pipeline;',
     '    vk::Pipeline coverage_pipeline;\n    vk::PipelineLayout hud_layout;\n    vk::Pipeline hud_pipeline;\n'
     '    vk::Image hud_art;\n    vk::ImageView hud_art_view;\n    vk::Sampler hud_sampler;\n'
     '    vk::DescriptorSetLayout hud_set_layout;\n    vk::DescriptorPool hud_pool;\n'
     '    vk::DescriptorSets hud_sets;'),
])
adapt('src/video_core/renderer_vulkan/present/window_adapt_pass.cpp', 'vulkan_window_adapt_pass.cpp', [
    ('#include "core/frontend/framebuffer_layout.h"',
     '#include "core/frontend/framebuffer_layout.h"\n#include "hud.h"\n#include "loading_art.h"\n'
     '#include "vulkan_hud_shaders.h"'),
    ('const Layout::FramebufferLayout& layout, Frame* dst) {',
     'const Layout::FramebufferLayout& layout, Frame* dst, MemoryAllocator* hud_memory) {'),
    ('    scheduler.Record([=](vk::CommandBuffer cmdbuf) {',
     (port / 'vulkan_hud_prepare.inc').read_text() + '\n    scheduler.Record([=](vk::CommandBuffer cmdbuf) {'),
    ('        cmdbuf.EndRenderPass();',
     (port / 'vulkan_hud_draw.inc').read_text() + '\n        cmdbuf.EndRenderPass();'),
])
adapt('src/video_core/renderer_vulkan/vk_blit_screen.cpp', 'vulkan_blit_screen.cpp', [
    ('VkFormat current_swapchain_view_format) {',
     'VkFormat current_swapchain_view_format, bool draw_hud) {'),
    ('image_index, layers, framebuffers, layout, frame);',
     'image_index, layers, framebuffers, layout, frame, draw_hud ? &memory_allocator : nullptr);'),
])
shader_header = '#pragma once\n#include <cstdint>\n'
for stage in ('vert', 'frag'):
    binary = output / f'vulkan_hud.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                    '-o', str(binary), str(port / f'vulkan_hud.{stage}')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    shader_header += f'inline constexpr uint32_t EDEN_HUD_{stage.upper()}_SPV[] = {{\n'
    shader_header += ','.join(hex(word) for word in words) + '\n};\n'
path = output / 'vulkan_hud_shaders.h'
if not path.exists() or path.read_text() != shader_header:
    path.write_text(shader_header)
# The allocator's budget is the driver's, and on the console it does not say what is available: the
# driver counts each allocation of graphics memory twice (as VRAM and as visible VRAM), so the budget
# of the 8 GiB heap is used up at 4 GiB, and it knows nothing of the pool the CPU shares. Eden asks
# for images and buffers within the budget and takes a refusal as final (its Commit paths do not:
# they ask again outside the budget). A large open-world game at 2x ended with "ran out of graphics
# memory" while the pool had 0.75 GiB in one block and 1.5 GiB free. An image or a buffer the budget
# refuses is asked for again outside it, as Commit does; the driver refuses when the pool has no
# room, and that is still the session's end.
allocator = (source / 'src/video_core/vulkan_common/vulkan_memory_allocator.cpp').read_text()
for call in ('vmaCreateImage(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info)',
             'vmaCreateBuffer(allocator, &ci, &alloc_ci, &handle, &allocation, &alloc_info)',
             'vmaCreateBufferWithAlignment(allocator, &ci, &alloc_ci, min_alignment, &handle,\n'
             '                                           &allocation, &alloc_info)'):
    old = '    vk::Check(' + call + ');\n'
    if allocator.count(old) != 1:
        raise RuntimeError(f'Pinned Vulkan allocator changed: {call[:28]} is there {allocator.count(old)} times')
    allocator = allocator.replace(old, (
        '    VkResult created = CALL;\n'
        '    if (created != VK_SUCCESS) {\n'
        '        VmaAllocationCreateInfo outside_budget = alloc_ci;\n'
        '        outside_budget.flags &= ~static_cast<VmaAllocationCreateFlags>(VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT);\n'
        '        created = AGAIN;\n'
        '        const auto times = ::Eden::Performance::outside_budget.fetch_add(1, std::memory_order_relaxed) + 1;\n'
        '        if ((times & (times - 1)) == 0)  // the 1st, 2nd, 4th, 8th ... time\n'
        '            std::fprintf(stderr, "[ProsperoEden] graphics memory: the allocator\'s budget refused %llu requests; "\n'
        '                         "asked again outside it (result %d): DETAIL\\n", static_cast<unsigned long long>(times), int(created),\n'
        '                         VALUES);\n'
        # Refused outside the budget too: the session ends here, and its log says what there was.
        '        if (created != VK_SUCCESS) {\n'
        '            VmaBudget held[VK_MAX_MEMORY_HEAPS]{};\n'
        '            vmaGetHeapBudgets(allocator, held);\n'
        '            unsigned long long blocks = 0, used = 0;\n'
        '            for (const VmaBudget& heap : held) {\n'
        '                blocks += heap.statistics.blockBytes;\n'
        '                used += heap.statistics.allocationBytes;\n'
        '            }\n'
        '            const auto free_block = ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed);\n'
        '            std::fprintf(stderr, "[ProsperoEden] graphics memory: refused outside the budget too (result %d): the pool\'s "\n'
        '                         "largest free block is %llu MiB; the allocator holds %llu MiB in blocks, %llu MiB of it in use\\n",\n'
        '                         int(created), free_block ? free_block() >> 20 : 0ull, blocks >> 20, used >> 20);\n'
        '        }\n'
        '    }\n'
        '    vk::Check(created);\n'
        ).replace('CALL', call).replace('AGAIN', call.replace('&alloc_ci', '&outside_budget'))
        .replace('DETAIL', 'image %ux%ux%u, %u layers, %u levels, %u samples, format %d' if 'Image' in call
                 else 'buffer of %llu bytes, usage 0x%x')
        .replace('VALUES', 'ci.extent.width, ci.extent.height, ci.extent.depth, ci.arrayLayers, ci.mipLevels, '
                 'unsigned(ci.samples), int(ci.format)' if 'Image' in call
                 else 'static_cast<unsigned long long>(ci.size), unsigned(ci.usage)'))
# What the allocator holds in its blocks, for the FPS overlay (performance.h, graphics_memory_held):
# asked while there is an allocator, a sum of counters it keeps.
held_ctor = ('MemoryAllocator::MemoryAllocator(const Device &device_)\n', 'MemoryAllocator::~MemoryAllocator() = default;\n',
             '                    device_.GetPhysical().GetProperties().limits.bufferImageGranularity} {\n')
for anchor in held_ctor:
    if allocator.count(anchor) != 1:
        raise RuntimeError(f'Pinned Vulkan allocator changed: {anchor[:40]} is there {allocator.count(anchor)} times')
allocator = allocator.replace(held_ctor[0], (
    'namespace {\n'
    'std::atomic<VmaAllocator> overlay_allocator{nullptr};\n'
    'unsigned long long GraphicsMemoryHeld() {\n'
    '    const VmaAllocator allocator = overlay_allocator.load(std::memory_order_acquire);\n'
    '    if (allocator == nullptr) return 0;\n'
    '    VmaBudget held[VK_MAX_MEMORY_HEAPS]{};\n'
    '    vmaGetHeapBudgets(allocator, held);\n'
    '    unsigned long long blocks = 0;\n'
    '    for (const VmaBudget& heap : held) blocks += heap.statistics.blockBytes;\n'
    '    return blocks;\n'
    '}\n'
    '} // namespace\n\n') + held_ctor[0])
allocator = allocator.replace(held_ctor[2], held_ctor[2] + (
    '    overlay_allocator.store(allocator, std::memory_order_release);\n'
    '    ::Eden::Performance::graphics_memory_held.store(&GraphicsMemoryHeld, std::memory_order_release);\n'))
allocator = allocator.replace(held_ctor[1], (
    'MemoryAllocator::~MemoryAllocator() {\n'
    '    ::Eden::Performance::graphics_memory_held.store(nullptr, std::memory_order_release);\n'
    '    overlay_allocator.store(nullptr, std::memory_order_release);\n'
    '}\n'))
include = '#include "video_core/vulkan_common/vulkan_memory_allocator.h"\n'
if allocator.count(include) != 1:
    raise RuntimeError('Pinned Vulkan allocator changed: its own include')
allocator = allocator.replace(include, include + '#include <atomic>\n#include <cstdio>\n#include "performance.h"\n')
path = output / 'vulkan_memory_allocator.cpp'
if not path.exists() or path.read_text() != allocator:
    path.write_text(allocator)
# Staging buffers (headless/staging_buffers.h): above 16 MiB a buffer the CPU writes or reads is its
# transfer rounded up to 16 MiB instead of the next power of two, a kept buffer is handed out again
# only when it holds the transfer, and a refused buffer is asked for once more after the kept ones
# the GPU is done with are let go. Metroid Prime 4 at 2x ended asking for 256 MiB for a transfer
# a little over 128 MiB, with 64 MiB in the pool's largest free block (#195).
# tools/check-staging-buffers.py applies these replacements to the pinned source and runs the result.
adapt('src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp', 'vulkan_staging_buffer_pool.cpp', [
    ('#include "video_core/vulkan_common/vulkan_wrapper.h"\n',
     '#include "video_core/vulkan_common/vulkan_wrapper.h"\n#include <atomic>\n#include <cstdio>\n'
     '#include "staging_buffers.h"\n'),
    ('    const auto is_free = [this](const StagingBuffer& entry) {\n'
     '        return !entry.deferred && scheduler.IsFree(entry.tick);\n'
     '    };',
     '    const auto is_free = [this, size](const StagingBuffer& entry) {\n'
     '        return !entry.deferred && scheduler.IsFree(entry.tick) &&\n'
     '               ::Eden::StagingBuffers::Holds(entry.mapped_span.size(), entry.log2_level) >= size;\n'
     '    };'),
    ('    auto const log2_size = Common::Log2Ceil<u32>(u32(size));\n',
     '    auto const log2_size = Common::Log2Ceil<u32>(u32(size));\n'
     '    const bool exact = ::Eden::StagingBuffers::Exact(size, usage != MemoryUsage::DeviceLocal);\n'
     '    const VkDeviceSize made = ::Eden::StagingBuffers::Size(size, log2_size, exact);\n'
     '    if (exact) {\n'
     '        // Kept buffers of this power of two too small for the transfer go before it is made.\n'
     '        ::Eden::StagingBuffers::ReleaseFrom(GetCache(usage)[log2_size], [this, size](const StagingBuffer& entry) {\n'
     '            return !entry.deferred && scheduler.IsFree(entry.tick) &&\n'
     '                   ::Eden::StagingBuffers::Holds(entry.mapped_span.size(), entry.log2_level) < size;\n'
     '        });\n'
     '    }\n'),
    ('        .size = 1ULL << log2_size,\n', '        .size = made,\n'),
    ('    vk::Buffer buffer = memory_allocator.CreateBuffer(buffer_ci, usage);\n',
     '    vk::Buffer buffer;\n'
     '    try {\n'
     '        buffer = memory_allocator.CreateBuffer(buffer_ci, usage);\n'
     '    } catch (const vk::Exception& refused) {\n'
     '        if (refused.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY) {\n'
     '            throw;\n'
     '        }\n'
     '        // Refused: the kept buffers the GPU is done with go, and the buffer is asked for once\n'
     '        // more. Refused again, the error goes on as before.\n'
     '        const auto idle = [this](const StagingBuffer& entry) {\n'
     '            return !entry.deferred && scheduler.IsFree(entry.tick);\n'
     '        };\n'
     '        ::Eden::StagingBuffers::Released released;\n'
     '        for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {\n'
     '            for (StagingBuffers& level : *cache) {\n'
     '                released += ::Eden::StagingBuffers::ReleaseFrom(level, idle);\n'
     '            }\n'
     '        }\n'
     '        std::fprintf(stderr, "[ProsperoEden] graphics memory: a staging buffer of %llu bytes was refused; "\n'
     '                     "released %zu idle staging buffers (%llu MiB) and asked again\\n",\n'
     '                     static_cast<unsigned long long>(made), released.buffers,\n'
     '                     static_cast<unsigned long long>(released.bytes >> 20));\n'
     '        buffer = memory_allocator.CreateBuffer(buffer_ci, usage);\n'
     '        std::fprintf(stderr, "[ProsperoEden] graphics memory: the staging buffer was made when asked again\\n");\n'
     '    }\n'
     '    // A large buffer smaller than the power of two: the 1st, 2nd, 4th, 8th ... in the log.\n'
     '    if (exact && made >= (VkDeviceSize{64} << 20) && made < (VkDeviceSize{1} << log2_size)) {\n'
     '        static std::atomic<unsigned long long> large{0};\n'
     '        const auto times = large.fetch_add(1, std::memory_order_relaxed) + 1;\n'
     '        if ((times & (times - 1)) == 0) {\n'
     '            std::fprintf(stderr, "[ProsperoEden] graphics memory: staging buffer of %llu MiB made for a transfer "\n'
     '                         "of %llu MiB, not the %llu MiB of the next power of two (%llu so far)\\n",\n'
     '                         static_cast<unsigned long long>(made >> 20), static_cast<unsigned long long>(size >> 20),\n'
     '                         static_cast<unsigned long long>((VkDeviceSize{1} << log2_size) >> 20), times);\n'
     '        }\n'
     '    }\n'),
])
adapt('src/video_core/vulkan_common/vulkan.h', 'include/video_core/vulkan_common/vulkan.h',
      [('#else\n#define VK_USE_PLATFORM_XLIB_KHR', '#elif defined(PS5_NATIVE)\n// Native display-plane presentation, no desktop window-system headers.\n#else\n#define VK_USE_PLATFORM_XLIB_KHR')])
adapt('src/video_core/vulkan_common/vulkan_instance.cpp', 'vulkan_instance.cpp', [
    ('namespace Vulkan {', 'extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);\n\nnamespace Vulkan {'),
    ('    case Core::Frontend::WindowSystemType::Headless:', '    case Core::Frontend::WindowSystemType::PS5:\n        extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);\n        break;\n    case Core::Frontend::WindowSystemType::Headless:'),
    ('#else\n    case Core::Frontend::WindowSystemType::X11:', '#elif !defined(PS5_NATIVE)\n    case Core::Frontend::WindowSystemType::X11:'),
    ('''    if (!library.IsOpen()) {
        LOG_ERROR(Render_Vulkan, "Vulkan library not available");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    if (!library.GetSymbol("vkGetInstanceProcAddr", &dld.vkGetInstanceProcAddr)) {
        LOG_ERROR(Render_Vulkan, "vkGetInstanceProcAddr not present in Vulkan");
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }''', '    (void)library;\n    dld.vkGetInstanceProcAddr = &::vkGetInstanceProcAddr;'),
])
adapt('src/video_core/vulkan_common/vulkan_library.cpp', 'vulkan_library.cpp', [
    ('#if defined(__ANDROID__) && defined(ARCHITECTURE_arm64)', '#if defined(PS5_NATIVE)\n    return std::make_shared<Common::DynamicLibrary>();\n#elif defined(__ANDROID__) && defined(ARCHITECTURE_arm64)'),
])
adapt('src/video_core/vulkan_common/vulkan_wrapper.h', 'include/video_core/vulkan_common/vulkan_wrapper.h', [
    ('#include <array>', '#include <array>\n#include <cstdio>\n#include "performance.h"'),
    ('return dld->vkQueueSubmit(queue,', 'auto timer = Eden::Performance::VulkanTimer(2);\n        return dld->vkQueueSubmit(queue,'),
    ('return dld->vkQueueSubmit2(queue,', 'auto timer = Eden::Performance::VulkanTimer(2);\n        return dld->vkQueueSubmit2(queue,'),
    ('return dld->vkWaitForFences(owner,', 'auto timer = Eden::Performance::VulkanTimer(3);\n        return dld->vkWaitForFences(owner,'),
    ('const VkResult result = dld->vkWaitSemaphores(owner,', 'auto timer = Eden::Performance::VulkanTimer(4);\n        const VkResult result = dld->vkWaitSemaphores(owner,'),
    ('return dld->vkDeviceWaitIdle(handle);', 'auto timer = Eden::Performance::VulkanTimer(5);\n        return dld->vkDeviceWaitIdle(handle);'),
    ('''inline void Check(VkResult result) {
    if (result != VK_SUCCESS) {
        throw Exception(result);''', '''inline void Check(VkResult result, const char* file = __builtin_FILE(),
                  int line = __builtin_LINE(), const char* operation = __builtin_FUNCTION()) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[ProsperoEden] Vulkan failure: %s:%d (%s) VkResult=%d\\n",
                     file, line, operation, static_cast<int>(result));
        throw Exception(result);'''),
])
# While a cached pipeline is built (headless/shader_start.h), the driver is asked how it made it
# (VkPipelineCreationFeedback: found in the pipeline cache Eden gives it, vulkan_pipelines.bin, or
# not, and its own time for it), and the call is timed. Other pipelines are created as before.
eden_driver_feedback = '''namespace {
template <typename CreateInfo, typename Create>
VkResult EdenCreatePipeline(const CreateInfo& ci, Create&& create) {
    if (!::Eden::ShaderStart::counting) {
        return create(ci);
    }
    VkPipelineCreationFeedback feedback{};
    const VkPipelineCreationFeedbackCreateInfo feedback_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
        .pNext = ci.pNext,
        .pPipelineCreationFeedback = &feedback,
        .pipelineStageCreationFeedbackCount = 0,
        .pPipelineStageCreationFeedbacks = nullptr,
    };
    const bool asked = ::Eden::ShaderStart::feedback.load(std::memory_order_relaxed);
    CreateInfo with_feedback = ci;
    if (asked) {
        with_feedback.pNext = &feedback_ci;
    }
    const long long start = ::Eden::ShaderStart::Now();
    const VkResult result = create(with_feedback);
    const bool valid = asked && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0;
    ::Eden::ShaderStart::NoteDriver(
        ::Eden::ShaderStart::Now() - start, valid,
        (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0,
        feedback.duration);
    return result;
}
} // Anonymous namespace

Pipeline Device::CreateGraphicsPipeline('''
adapt('src/video_core/vulkan_common/vulkan_wrapper.cpp', 'vulkan_wrapper.cpp', [
    ('#include "video_core/vulkan_common/vulkan_wrapper.h"', '#include "video_core/vulkan_common/vulkan_wrapper.h"\n#include "performance.h"\n#include "shader_start.h"'),
    ('Pipeline Device::CreateGraphicsPipeline(', eden_driver_feedback),
    ('auto const result = dld->vkCreateGraphicsPipelines(handle, cache, 1, &ci, nullptr, &object);',
     'auto timer = Eden::Performance::VulkanTimer(0);\n'
     '    auto const result = EdenCreatePipeline(ci, [&](const VkGraphicsPipelineCreateInfo& info) {\n'
     '        return dld->vkCreateGraphicsPipelines(handle, cache, 1, &info, nullptr, &object);\n'
     '    });'),
    ('Check(dld->vkCreateComputePipelines(handle, cache, 1, &ci, nullptr, &object));',
     'auto timer = Eden::Performance::VulkanTimer(1);\n'
     '    Check(EdenCreatePipeline(ci, [&](const VkComputePipelineCreateInfo& info) {\n'
     '        return dld->vkCreateComputePipelines(handle, cache, 1, &info, nullptr, &object);\n'
     '    }));'),
])
download_source = (source / 'src/video_core/texture_cache/texture_cache.h').read_text()
download_begin = download_source.index('void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size)')
download_loop = download_source.index('    for (const ImageId image_id : images) {', download_begin)
download_prefix = download_source[download_begin:download_loop]
# Reuse DeleteImage's exact accounting to stop lookahead when its existing
# pressure policy changes. Fail on source drift rather than inventing a budget.
delete_begin = download_source.index('    if (image.HasScaled()) {', download_source.index('void TextureCache<P>::DeleteImage('))
delete_end = download_source.index('    const GPUVAddr gpu_addr', delete_begin)
reclaimed_bytes = download_source[delete_begin:delete_end].replace('total_used_memory -=', 'bytes +=')
gc_begin = download_source.index('void TextureCache<P>::RunGarbageCollector() {')
gc_end = download_source.index('\ntemplate <class P>', gc_begin)
gc_original = download_source[gc_begin:gc_end]
gc = gc_original
gc_setup = '''    const auto ReclaimedBytes = [&](const ImageBase& image) {
        u64 bytes = 0;
''' + reclaimed_bytes + '''        return bytes;
    };
''' + (port / 'vulkan_gc_downloads.inc').read_text()
for old, new in [
    ('    const auto Cleanup = [this, &num_iterations, &high_priority_mode, &aggressive_mode]',
     gc_setup + '\n    const auto Cleanup = [&, this]'),
    ('        if (must_download) {', '        if (must_download && !TakePrefetched(image_id, image)) {'),
    # Time the per-image drain Cleanup still takes for an image the prefetch could not batch.
    # One GPU pipeline wait per eviction, against one wait for a whole batch in the batched path:
    # this is the stall the prefetch exists to remove, so keep it visible.
    ('            image.DownloadMemory(map, copies);\n            runtime.Finish();\n'
     '            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);',
     '            image.DownloadMemory(map, copies);\n'
     '            {\n'
     '                const ::Eden::Performance::Timer drain_timer(\n'
     '                    ::Eden::Performance::gc_unbatched_drain, 1);\n'
     '                runtime.Finish();\n'
     '            }\n'
     '            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);'),
    ('(!high_priority_mode && must_download)) {', '(!DirtyEvictions() && must_download)) {'),
    ('    Configure(false);', '    Configure(false);\n    PrefetchDownloads();'),
    ('        Configure(true);', '        Configure(true);\n        PrefetchDownloads();'),
    # The eviction budget counts visits, not evictions: skipped images (decoding,
    # costly, or dirty images kept below the critical mark) burn it and stop the
    # scan, so kept images shield everything behind them. Count evictions; bound
    # the scan separately. gc_scan=upstream restores visit counting.
    ('    size_t num_iterations = 0;',
     '    size_t num_iterations = 0;\n    size_t num_scanned = 0;\n    constexpr size_t max_scanned = 8192;'),
    ('        if (num_iterations == 0) {\n            return true;\n        }\n        --num_iterations;',
     '        if (num_iterations == 0 || num_scanned >= max_scanned) {\n            return true;\n        }\n        ++num_scanned;\n        if (::Eden::Performance::gc_count_visits.load(std::memory_order_relaxed)) {\n            --num_iterations;\n        }'),
    ('        if ((!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) || (!DirtyEvictions() && must_download)) {\n            return false;\n        }',
     '        if ((!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) || (!DirtyEvictions() && must_download)) {\n            return false;\n        }\n        if (!::Eden::Performance::gc_count_visits.load(std::memory_order_relaxed)) {\n            --num_iterations;\n        }'),
    # The aggressive pass gets its own scan allowance: Configure resets the
    # eviction budget per pass, and the scan counter must reset with it, or a
    # first pass spent on skips would stillborn the second one. Runs after the
    # Configure(true) pair above, whose first line this matches.
    ('        Configure(true);',
     '        Configure(true);\n        num_scanned = 0;'),
    # What it frees because memory is short, for a line in the log (headless/texture_evictions.h).
    ('        UnregisterImage(image_id);\n        DeleteImage(image_id, image.scale_tick > frame_tick + 5);',
     '        if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {\n'
     '            ::Eden::TextureEvictions::Freed(aggressive_mode, True(image.flags & ImageFlagBits::Converted),\n'
     '                                            must_download);\n'
     '        }\n'
     '        UnregisterImage(image_id);\n        DeleteImage(image_id, image.scale_tick > frame_tick + 5);'),
]:
    if gc.count(old) != 1:
        raise RuntimeError(f'Pinned GC anchor changed: {old}')
    gc = gc.replace(old, new)
texture_costs = []
for signature, index in (
    ('RunGarbageCollector()', 13), ('DownloadMemory(DAddr cpu_addr, size_t size)', 14),
    ('PopAsyncFlushes()', 15),
):
    anchor = f'void TextureCache<P>::{signature} {{'
    scope = f'''
    std::optional<Eden::Performance::Timer> cost_timer;
    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {{
        if (Eden::Performance::vulkan_cost_enabled.load(std::memory_order_relaxed))
            cost_timer.emplace(Eden::Performance::vulkan_api[{index}]);
    }}
'''
    if index == 13:
        scope += '''    if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {
        if ((cost_timer || Eden::Performance::texture_budget_log.load(std::memory_order_relaxed)) &&
            frame_tick % 300 == 0)
            std::printf("EDEN_VULKAN_TEXTURE_BUDGET frame=%llu usage=%llu expected=%llu critical=%llu "
                        "keep_dirty=%d memory_short=%d\\n",
                        static_cast<unsigned long long>(frame_tick),
                        static_cast<unsigned long long>(total_used_memory),
                        static_cast<unsigned long long>(expected_memory),
                        static_cast<unsigned long long>(critical_memory),
                        int(Eden::Performance::gc_keep_dirty.load(std::memory_order_relaxed)),
                        int(Eden::Performance::graphics_memory_short.load(std::memory_order_relaxed)));
    }
'''
    texture_costs.append((anchor, anchor + scope))
# Large textures re-upload only the block rows guest writes changed (headless/texture_partial.h).
VULKAN_CACHE = 'if constexpr (std::is_same_v<Runtime, Vulkan::TextureCacheRuntime>) {'
texture_partial = [
    # The page lists belong to this cache's images: a title started after another in the same
    # process (return to menu) must not see the previous cache's ImageId indices.
    ('    : runtime{runtime_}, device_memory{device_memory_} {\n',
     '    : runtime{runtime_}, device_memory{device_memory_} {\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::images.clear();\n'
     '    }\n'),
    ('    ForEachImageInRegion(cpu_addr, size, [this](ImageId image_id, Image& image) {\n'
     '        if (True(image.flags & ImageFlagBits::CpuModified)) {\n'
     '            return;\n'
     '        }\n'
     '        image.flags |= ImageFlagBits::CpuModified;',
     '    ForEachImageInRegion(cpu_addr, size, [this, cpu_addr, size](ImageId image_id, Image& image) {\n'
     '        ' + VULKAN_CACHE + '\n'
     '            if (Eden::TexturePartial::Write(device_memory, image, image_id.index, cpu_addr, size)) {\n'
     '                return;\n'
     '            }\n'
     '        }\n'
     '        if (True(image.flags & ImageFlagBits::CpuModified)) {\n'
     '            return;\n'
     '        }\n'
     '        image.flags |= ImageFlagBits::CpuModified;'),
    ('    ASSERT(True(image.flags & ImageFlagBits::Tracked));\n'
     '    image.flags &= ~ImageFlagBits::Tracked;',
     '    ASSERT(True(image.flags & ImageFlagBits::Tracked));\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::End(device_memory, image_id.index);\n'
     '    }\n'
     '    image.flags &= ~ImageFlagBits::Tracked;'),
    ('    if (invalidate) {\n'
     '        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);',
     '    if (invalidate) {\n'
     '        ' + VULKAN_CACHE + '\n'
     '            Eden::TexturePartial::End(device_memory, image_id.index);\n'
     '        }\n'
     '        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);'),
    ('    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");',
     '    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");\n'
     '    ' + VULKAN_CACHE + '\n'
     '        Eden::TexturePartial::images.erase(image_id.index);\n'
     '    }'),
    ('    if (False(image.flags & ImageFlagBits::CpuModified)) {\n'
     '        // Only upload modified images\n'
     '        return;\n'
     '    }\n',
     '    if (False(image.flags & ImageFlagBits::CpuModified)) {\n'
     '        // Only upload modified images\n'
     '        return;\n'
     '    }\n'
     '    ' + VULKAN_CACHE + '\n' + (port / 'texture_partial_refresh.inc').read_text() + '    }\n'),
]
adapt('src/video_core/texture_cache/texture_cache.h',
      'vulkan-cache/video_core/texture_cache/texture_cache.h', [
    ('namespace VideoCommon {', '#include "performance.h"\n#include "texture_partial.h"\n#include "image_pages.h"\n#include "astc_policy.h"\n#include "texture_evictions.h"\n#include "common/scope_exit.h"\nnamespace Vulkan { class TextureCacheRuntime; }\n\nnamespace VideoCommon {'),
    # An ASTC texture kept as BC3 is converted in the background in a busy frame only, and in
    # place on a light screen (headless/astc_policy.h). The frame is counted where the texture
    # cache ends its own.
    ('    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {\n        QueueAsyncDecode(image, image_id);\n        return;\n    }',
     '    if (True(image.flags & ImageFlagBits::AsynchronousDecode) && ::Eden::AstcPolicy::InBackground()) {\n        QueueAsyncDecode(image, image_id);\n        return;\n    }'),
    ('    TickAsyncDecode();\n    TickAsyncUnswizzle();', '    ::Eden::AstcPolicy::Frame();\n    TickAsyncDecode();\n    TickAsyncUnswizzle();'),
    # Every 10 s at most, what the collector freed because memory was short (texture_evictions.h).
    ('    sentenced_images.Tick();\n',
     '    ' + VULKAN_CACHE + '\n'
     '        ::Eden::TextureEvictions::Frame(async_decodes.size(),\n'
     '            ::Eden::Performance::graphics_memory_free.load(std::memory_order_relaxed),\n'
     '            ::Eden::Performance::graphics_memory_held.load(std::memory_order_acquire));\n'
     '    }\n'
     '    sentenced_images.Tick();\n'),
    # Every place an image is registered at is marked before the texture cache can find the
    # image there (headless/image_pages.h): the two inserts below are the only ones.
    ('        auto map_id =\n            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);',
     '        ::Eden::ImagePages::Mark(image.cpu_addr, image.guest_size_bytes);\n'
     '        auto map_id =\n            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);'),
    ('            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);',
     '            ::Eden::ImagePages::Mark(cpu_addr, size);\n'
     '            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);'),
    (download_prefix, download_prefix + (port / 'vulkan_download_batch.inc').read_text() + '\n'),
    (gc_original, gc),
    # RADV reports all Vulkan allocations, not just cached textures. A game's
    # measured ~1.9 GiB working set triggered dirty eviction at the old 1.6 GiB
    # threshold despite a 4 GiB budget. Retain 40% headroom; keep the original
    # critical threshold and minimum 1 GiB spacing. OpenGL remains unchanged.
    ('const s64 min_vacancy_expected = (6 * mem_threshold) / 10;',
     'const s64 min_vacancy_expected =\n            ((std::is_same_v<Runtime, Vulkan::TextureCacheRuntime> ? 4 : 6) * mem_threshold) / 10;'),
] + texture_partial + texture_costs)

# ASTC recompressed to BC1 or BC3 a layer at a time, as headless/CMakeLists.txt makes Eden's own
# util.cpp do: this copy is the one the Vulkan build links, and it decoded a whole mip level of
# every layer into one buffer, 484 MiB for a texture array of 121 layers. A Tears of the Kingdom
# session (v1.010.005) ended asking for exactly that much ("Direct allocation failed ...
# bytes=509607936", the block and its lead) with the pool short.
recompress_old = '''            decode_scratch.resize_destructive(level_size);

            Tegra::Texture::ASTC::Decompress(
                input_offset, copy.image_extent.width, copy.image_extent.height,
                copy.image_subresource.num_layers * copy.image_extent.depth, tile_size.width,
                tile_size.height, decode_scratch);

            compress(decode_scratch, copy.image_extent.width, copy.image_extent.height,
                     copy.image_subresource.num_layers * copy.image_extent.depth,
                     output.subspan(output_offset));
'''
recompress_new = '''            // A layer at a time (headless/CMakeLists.txt): ASTC blocks are 16 bytes, BC blocks
            // cover 4x4 pixels.
            const u32 layers = copy.image_subresource.num_layers * copy.image_extent.depth;
            const u32 layer_input = Common::DivideUp(copy.image_extent.width, tile_size.width) *
                                    Common::DivideUp(copy.image_extent.height, tile_size.height) * 16;
            const u32 layer_output = Common::AlignUp(copy.image_extent.width, 4) *
                                     Common::AlignUp(copy.image_extent.height, 4) / bpp_div;
            decode_scratch.resize_destructive(level_size / (std::max)(layers, 1u));
            for (u32 layer = 0; layer < layers; ++layer) {
                Tegra::Texture::ASTC::Decompress(
                    input_offset.subspan(layer * layer_input), copy.image_extent.width,
                    copy.image_extent.height, 1, tile_size.width, tile_size.height, decode_scratch);
                compress(decode_scratch, copy.image_extent.width, copy.image_extent.height, 1,
                         output.subspan(output_offset + layer * layer_output));
            }
'''
adapt('src/video_core/texture_cache/util.cpp', 'texture_util.cpp', [
    ('#include <vector>', '#include <cstring>\n#include <vector>\n#include "texture_partial.h"'),
    (recompress_old, recompress_new),
    ('} // namespace VideoCommon', (port / 'texture_rows.inc').read_text() + '\n} // namespace VideoCommon'),
])

# The four calls inside bind_geometry and the three inside bind_update. They sit in buffer_cache.h
# rather than in Configure, and both spend their time in SynchronizeBuffer's page walk over the
# memory tracker - once for the index buffer, once per enabled vertex buffer, once per per-stage
# segment, every draw, dirty or not. Which of the seven that is cannot be decided by reading it:
# the texture-buffer loop looked like the cost of bind and measured 2.8%.
adapt('src/video_core/buffer_cache/buffer_cache.h',
      'vulkan-cache/video_core/buffer_cache/buffer_cache.h', [
    ('#include "video_core/buffer_cache/buffer_cache_base.h"',
     '#include "video_core/buffer_cache/buffer_cache_base.h"\n#include "performance.h"'),
    ('void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {',
     'void BufferCache<P>::BindHostGeometryBuffers(bool is_indexed) {\n'
     '    long long since_geo = ::Eden::Performance::MarkStart();'),
    ('    BindHostVertexBuffers();\n'
     '    BindHostTransformFeedbackBuffers();\n'
     '    if (current_draw_indirect) {\n'
     '        BindHostDrawIndirectBuffers();\n'
     '    }\n'
     '}',
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_index, since_geo);\n'
     '    BindHostVertexBuffers();\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_vertex, since_geo);\n'
     '    BindHostTransformFeedbackBuffers();\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_xf, since_geo);\n'
     '    if (current_draw_indirect) {\n'
     '        BindHostDrawIndirectBuffers();\n'
     '    }\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::geo_indirect, since_geo);\n'
     '}'),
    ('void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {',
     'void BufferCache<P>::DoUpdateGraphicsBuffers(bool is_indexed) {\n'
     '    long long since_upd = ::Eden::Performance::MarkStart();'),
    ('        UpdateTransformFeedbackBuffers();\n'
     '        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {\n'
     '            UpdateUniformBuffers(stage);\n'
     '            UpdateStorageBuffers(stage);\n'
     '            UpdateTextureBuffers(stage);\n'
     '        }\n'
     '        if (current_draw_indirect) {\n'
     '            UpdateDrawIndirect();\n'
     '        }',
     '        UpdateTransformFeedbackBuffers();\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_state, since_upd);\n'
     '        for (size_t stage = 0; stage < NUM_STAGES; ++stage) {\n'
     '            UpdateUniformBuffers(stage);\n'
     '            UpdateStorageBuffers(stage);\n'
     '            UpdateTextureBuffers(stage);\n'
     '        }\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_stage, since_upd);\n'
     '        if (current_draw_indirect) {\n'
     '            UpdateDrawIndirect();\n'
     '        }\n'
     '        ::Eden::Performance::Mark(::Eden::Performance::upd_indirect, since_upd);'),
])

# These costs sit outside vkCreateGraphicsPipelines: guest shader translation,
# worker readiness, and background serialization can block different threads.
# The six phases inside Configure. draw_config is all of Configure but SetEngine's two pointer
# assignments, and ConfigureImpl is ~210 lines; without these the 2.78 us a draw spends
# configuring arrives as one number with nothing to aim at. One running clock, marked at the
# boundaries the source already has.
configure_phases = [
    ('bool GraphicsPipeline::ConfigureImpl(bool is_indexed) {',
     'bool GraphicsPipeline::ConfigureImpl(bool is_indexed) {\n'
     '    long long since = ::Eden::Performance::MarkStart();'),
    ('    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);',
     '    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);\n'
     '    // Sampler and image tables synced, uniform-buffer state set.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_sync, since);'),
    ('    texture_cache.FillImageViews(std::span(views.data(), views.size()), false, Spec::has_images);',
     '    texture_cache.FillImageViews(std::span(views.data(), views.size()), false, Spec::has_images);\n'
     '    // Every texture handle read out of guest memory, every sampler and view filled.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_read, since);\n'
     '    long long since_bind = ::Eden::Performance::MarkStart();'),
    ('    if (regs.transform_feedback_enabled != 0) {',
     '    // The texture-buffer loop over every enabled stage.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_tbo, since_bind);\n'
     '    if (regs.transform_feedback_enabled != 0) {'),
    ('    buffer_cache.UpdateGraphicsBuffers(is_indexed);',
     '    buffer_cache.UpdateGraphicsBuffers(is_indexed);\n'
     '    // Index, vertex, transform-feedback and per-stage buffer updates done.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_update, since_bind);'),
    ('    buffer_cache.BindHostGeometryBuffers(is_indexed);',
     '    buffer_cache.BindHostGeometryBuffers(is_indexed);\n'
     '    // Storage, texture and geometry buffers bound.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::bind_geometry, since_bind);\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_bind, since);'),
    ('    texture_cache.UpdateRenderTargets(false);',
     '    // Descriptor queue acquired, image descriptors pushed, uploads barriered.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_desc, since);\n'
     '    texture_cache.UpdateRenderTargets(false);'),
    ('    if (IsBuilt() && !pipeline) {',
     '    // Render targets current, feedback loops checked.\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_rt, since);\n'
     '    if (IsBuilt() && !pipeline) {'),
    ('    return ConfigureDraw(rescaling, render_area);',
     '    // Compare the descriptor payload against last draw\'s and record the bindings.\n'
     '    const bool configured = ConfigureDraw(rescaling, render_area);\n'
     '    ::Eden::Performance::Mark(::Eden::Performance::cfg_draw, since);\n'
     '    return configured;'),
]
for name in ('vk_graphics_pipeline', 'vk_compute_pipeline'):
    # Once the pipeline exists, Eden's hashes beside its VkPipeline on stderr
    # (headless/pipeline_identity.h): the driver's stall dump names the handle whose shader the
    # GPU stopped in.
    identity = ('::Eden::PipelineIdentity::Graphics(*pipeline, key.Hash(), key.unique_hashes);'
                if name == 'vk_graphics_pipeline' else
                '::Eden::PipelineIdentity::Compute(*pipeline, shader_hash);')
    anchors = [
        ('#include <algorithm>', '#include <algorithm>\n#include "performance.h"\n#include "pipeline_identity.h"\n'
                                 '#include "shader_start.h"\n#include "crash_report.h"'),
        ('            std::unique_lock lock{build_mutex};',
         '            auto timer = Eden::Performance::VulkanTimer(16);\n            std::unique_lock lock{build_mutex};'),
        ('            pipeline_statistics->Collect(device, *pipeline);\n        }\n',
         '            pipeline_statistics->Collect(device, *pipeline);\n        }\n        ' + identity + '\n'),
    ]
    # The shader cache's report (headless/shader_start.h): the descriptor set and pipeline layouts
    # and the update template, made before the pipeline itself.
    if name == 'vk_graphics_pipeline':
        anchors += [
            ('      descriptor_buffer_ring{descriptor_buffer_ring_}, spv_modules{std::move(stages)} {\n',
             '      descriptor_buffer_ring{descriptor_buffer_ring_}, spv_modules{std::move(stages)} {\n'
             '    ::Eden::ShaderStart::Clock eden_layout{::Eden::ShaderStart::layout};\n'),
            ('    auto func{[this, shader_notify, &render_pass_cache, pipeline_statistics] {',
             '    eden_layout.Stop();\n    auto func{[this, shader_notify, &render_pass_cache, pipeline_statistics] {'),
        ]
    else:
        anchors += [
            ('      shader_hash{shader_hash_}, spv_module(std::move(spv_module_)) {\n',
             '      shader_hash{shader_hash_}, spv_module(std::move(spv_module_)) {\n'
             '    ::Eden::ShaderStart::Clock eden_layout{::Eden::ShaderStart::layout};\n'),
            ('    auto func{[this, shader_notify, pipeline_statistics] {',
             '    eden_layout.Stop();\n    auto func{[this, shader_notify, pipeline_statistics] {'),
        ]
    if name == 'vk_graphics_pipeline':
        anchors += configure_phases
    # While the driver compiles the pipeline, a crash report names it after the thread
    # (headless/crash_report.h, Doing): a fault inside the driver leaves no other trace of the shader
    # (Oliv3rmoon/Nightshade#4), and EDEN_PIPELINE is printed only once the pipeline exists.
    if name == 'vk_graphics_pipeline':
        anchors.append(('    pipeline = device.GetLogical().CreateGraphicsPipeline({\n',
                        '    const ::Eden::Crash::Doing eden_doing{"the driver compiling graphics pipeline key", key.Hash()};\n'
                        '    pipeline = device.GetLogical().CreateGraphicsPipeline({\n'))
    else:
        anchors.append(('            pipeline = device.GetLogical().CreateComputePipeline(compute_ci, *pipeline_cache);\n',
                        '            const ::Eden::Crash::Doing eden_doing{"the driver compiling compute pipeline cs", shader_hash};\n'
                        '            pipeline = device.GetLogical().CreateComputePipeline(compute_ci, *pipeline_cache);\n'))
    adapt(f'src/video_core/renderer_vulkan/{name}.cpp', name + '_cost.cpp', anchors)
shader_costs = [
    ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include <set>\n#include <unordered_set>\n'
     '#include "performance.h"\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
    # For testing a game that stops the GPU (dev_vulkan.h coherent_storage, from a game's
    # "gpu_debug" or dev-settings): more of its storage buffers device-coherent (Eden patch 10). The
    # profile is made with the renderer, after the session has set the switch. Eden builds every
    # shader's SPIR-V again from the guest code at each start, so no cache holds another mode's.
    ('        .max_user_clip_distances = device.GetMaxUserClipDistances()\n    };\n',
     '        .max_user_clip_distances = device.GetMaxUserClipDistances()\n    };\n'
     '    profile.coherent_storage = static_cast<Shader::CoherentStorage>(\n'
     '        std::clamp(::Eden::DevVulkan::coherent_storage, 0, 2));\n'),
    # Development crash probes: report each pipeline's first use (stage hashes) to klog,
    # which survives a GPU fault that kills the title.
    ('    current_pipeline = pipeline.get();\n    return BuiltPipeline(current_pipeline);\n}',
     '    current_pipeline = pipeline.get();\n'
     '    if (::Eden::DevVulkan::trace_pipelines) {\n'
     '        static std::unordered_set<const void*> traced;\n'
     '        if (traced.insert(current_pipeline).second) {\n'
     '            const auto& h = graphics_key.unique_hashes;\n'
     '            char text[256];\n'
     '            std::snprintf(text, sizeof(text), "G%zu p=%p new=%d va=%llx vb=%llx tc=%llx te=%llx gs=%llx fs=%llx",\n'
     '                          traced.size(), (const void*)current_pipeline, int(is_new), (unsigned long long)h[0], (unsigned long long)h[1],\n'
     '                          (unsigned long long)h[2], (unsigned long long)h[3], (unsigned long long)h[4],\n'
     '                          (unsigned long long)h[5]);\n'
     '            ::Eden::Report("pipeline", text);\n'
     '            LOG_INFO(Render_Vulkan, "EDEN_PIPELINE_FIRST_USE {}", text);\n'
     '        }\n'
     '    }\n'
     '    return BuiltPipeline(current_pipeline);\n}'),
    ('    if (!is_new) {\n        return pipeline.get();\n    }\n    pipeline = CreateComputePipeline(key, shader);\n    return pipeline.get();\n}',
     '    if (is_new) {\n'
     '        pipeline = CreateComputePipeline(key, shader);\n'
     '    }\n'
     '    if (::Eden::DevVulkan::trace_pipelines && pipeline) {\n'
     '        static std::unordered_set<const void*> traced;\n'
     '        if (traced.insert(pipeline.get()).second) {\n'
     '            char text[160];\n'
     '            std::snprintf(text, sizeof(text), "C%zu p=%p new=%d cs=%llx shared=%u wg=%u,%u,%u", traced.size(),\n'
     '                          (const void*)pipeline.get(), int(is_new), (unsigned long long)key.unique_hash, unsigned(key.shared_memory_size),\n'
     '                          unsigned(key.workgroup_size[0]), unsigned(key.workgroup_size[1]),\n'
     '                          unsigned(key.workgroup_size[2]));\n'
     '            ::Eden::Report("pipeline", text);\n'
     '            LOG_INFO(Render_Vulkan, "EDEN_PIPELINE_FIRST_USE {}", text);\n'
     '        }\n'
     '    }\n'
     '    return pipeline.get();\n}'),
    ('std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {',
     'std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {\n    auto timer = Eden::Performance::VulkanTimer(17);'),
    ('    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {',
     '    const ComputePipelineCacheKey& key, const ShaderInfo* shader) {\n    auto timer = Eden::Performance::VulkanTimer(17);'),
    ('                                                 u32 cache_version) try {',
     '                                                 u32 cache_version) try {\n    auto timer = Eden::Performance::VulkanTimer(18);'),
    ('}\n\nPipelineCache::~PipelineCache()',
     '    if (Eden::Performance::vulkan_cost_enabled.load(std::memory_order_relaxed))\n'
     '        std::printf("EDEN_VULKAN_DYNAMIC blend=%u enables=%u vertex=%u\\n",\n'
     '                    unsigned(dynamic_features.has_extended_dynamic_state_3_blend),\n'
     '                    unsigned(dynamic_features.has_extended_dynamic_state_3_enables),\n'
     '                    unsigned(dynamic_features.has_dynamic_vertex_input));\n'
     '}\n\nPipelineCache::~PipelineCache()'),
    # Static vertex input (no attribute) for pipelines whose vertex shader reads no vertex attribute
    # (dev_vulkan.h static_vertex_input): with VK_EXT_vertex_input_dynamic_state the console's
    # driver drew them wrong (a full-screen triangle from the vertex index covering a game's
    # cutscenes with a flat colour). Building a pipeline records whether its vertex shader reads
    # attributes; the static key has no attribute, so the disk cache keeps loading it.
    ('#include "video_core/engines/maxwell_3d.h"',
     '#include "video_core/engines/maxwell_3d.h"\n#include "video_core/dirty_flags.h"\n'
     '#include <atomic>\n#include <mutex>\n#include <set>\n#include <unordered_set>\n'
     '#include "video_core/renderer_vulkan/vk_state_tracker.h"'),
    ('GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {',
     """namespace {
std::mutex eden_attributeless_mutex;
using EdenVertexIdentity = std::array<u64, 2>;
std::set<EdenVertexIdentity> eden_attributeless_vertex;
std::atomic<u32> eden_attributeless_generation{0};
bool eden_static_vertex_input_bound = false;

void EdenRecordAttributeless(EdenVertexIdentity vertex_hash, const Shader::Info& info) {
    for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        if (info.loads.Generic(index)) {
            return;
        }
    }
    std::scoped_lock lock{eden_attributeless_mutex};
    if (eden_attributeless_vertex.insert(vertex_hash).second) {
        eden_attributeless_generation.fetch_add(1, std::memory_order_release);
    }
}

// GPU thread: whether the vertex shader reads no attribute, as far as built pipelines tell.
bool EdenAttributeless(EdenVertexIdentity vertex_hash) {
    static EdenVertexIdentity last_hash{};
    static u32 last_generation = ~0u;
    static bool last_result = false;
    const u32 generation = eden_attributeless_generation.load(std::memory_order_acquire);
    if (vertex_hash != last_hash || generation != last_generation) {
        std::scoped_lock lock{eden_attributeless_mutex};
        last_hash = vertex_hash;
        last_generation = generation;
        last_result = eden_attributeless_vertex.contains(vertex_hash);
    }
    return last_result;
}

// A static key without attributes (what the loader takes back from the disk cache).
bool EdenAttributelessStaticKey(const FixedPipelineState& state) {
    return state.dynamic_vertex_input == 0 &&
           std::ranges::none_of(state.attributes,
                                [](const auto& attribute) { return attribute.raw != 0; });
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {"""),
    ('    graphics_key.state.Refresh(*maxwell3d, dynamic_features);\n',
     """    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    const bool static_vertex_input = graphics_key.state.dynamic_vertex_input != 0 &&
                                     ::Eden::DevVulkan::static_vertex_input &&
                                     EdenAttributeless({graphics_key.unique_hashes[0], graphics_key.unique_hashes[1]});
    if (static_vertex_input) {
        // Instance-rate streams as the registers set them (with every stream per vertex the
        // driver drew these wrong too); no attribute, so the key does not depend on what the
        // registers hold for attributes the shader does not read.
        const auto& regs = maxwell3d->regs;
        graphics_key.state.dynamic_vertex_input.Assign(0);
        graphics_key.state.enabled_divisors = 0;
        graphics_key.state.attributes = {};
        graphics_key.state.binding_divisors = {};
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            const bool is_enabled = regs.vertex_stream_instances.IsInstancingEnabled(index);
            graphics_key.state.binding_divisors[index] =
                is_enabled ? regs.vertex_streams[index].frequency : 0;
            graphics_key.state.enabled_divisors |= (is_enabled ? u64{1} : 0) << index;
        }
        // The union's attribute_types now holds divisors: the next dynamic draw recomputes them
        // and sets its vertex input again (a static pipeline replaced it).
        maxwell3d->dirty.flags[Dirty::VertexInput] = true;
    }
    if (static_vertex_input && !eden_static_vertex_input_bound) {
        // Rebind the vertex buffers with their strides, dynamic state of the static pipeline.
        auto& flags = maxwell3d->dirty.flags;
        flags[VideoCommon::Dirty::VertexBuffers] = true;
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
        }
    }
    eden_static_vertex_input_bound = static_vertex_input;
"""),
    ('            (key.state.dynamic_vertex_input != 0) !=\n                dynamic_features.has_dynamic_vertex_input) {',
     '            ((key.state.dynamic_vertex_input != 0) !=\n'
     '                 dynamic_features.has_dynamic_vertex_input &&\n'
     '             !(dynamic_features.has_dynamic_vertex_input &&\n'
     '               ::Eden::DevVulkan::static_vertex_input &&\n'
     '               EdenAttributelessStaticKey(key.state)))) {'),
    ('    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};\n',
     '    if (uses_vertex_b) {\n'
     '        EdenRecordAttributeless({key.unique_hashes[0], key.unique_hashes[1]}, programs[1].info);\n'
     '    }\n'
     '    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};\n'),
]
# Time both graphics and compute preparation, without modifying the compiler or
# reading guest state on another thread. These intervals overlap shader_prepare.
shader_source = (source / 'src/video_core/renderer_vulkan/vk_pipeline_cache.cpp').read_text()
for expression, index, count in (
    ('main_pools.ReleaseContents()', 19, 2),
    ('TranslateProgram(pools.inst, pools.block, env, cfg, host_info)', 21, 3),
    ('EmitSPIRV(profile, runtime_info, program, binding)', 22, 1),
    ('EmitSPIRV(profile, program)', 22, 1),
    ('BuildShader(device, code)', 23, 2),
):
    if shader_source.count(expression) != count:
        raise RuntimeError(f'Shader cost anchor changed: {expression}')
    # The same stages for the shader cache's own report (headless/shader_start.h), which counts
    # only builds of cached pipelines and is always on.
    stage = {21: 'ir', 22: 'spirv', 23: 'shader_module'}.get(index)
    start = f'::Eden::ShaderStart::Clock start_cost{{::Eden::ShaderStart::{stage}}}; ' if stage else ''
    shader_source = shader_source.replace(expression,
        f'([&] {{ auto cost = Eden::Performance::VulkanTimer({index}); {start}'
        f'return {expression}; }}())')
for statement in (
    'Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);',
    'Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};',
):
    expected = 2 if 'cfg_offset' in statement else 1  # graphics exception dump too
    if shader_source.count(statement) != expected:
        raise RuntimeError(f'Shader CFG anchor changed: {statement}')
    shader_source = shader_source.replace(statement,
        'auto cfg_cost = Eden::Performance::VulkanTimer(20);\n        '
        '::Eden::ShaderStart::Clock cfg_start_cost{::Eden::ShaderStart::cfg};\n        ' + statement +
        '\n        cfg_cost.reset();\n        cfg_start_cost.Stop();')
for old, new in shader_costs:
    if shader_source.count(old) != 1:
        raise RuntimeError(f'Shader pipeline anchor changed: {old[:60]}')
    shader_source = shader_source.replace(old, new)
# The driver's pipeline cache file (vulkan_pipelines.bin). Eden rewrote it in place: opening it
# emptied it, and a large game's cache takes seconds to write, so a session that ended during the
# write (the ten-second limit on leaving a game, a crash, rest mode) left it empty or cut off, and
# every pipeline was compiled again at the next start (#170). It is now written beside the old
# file and renamed over it once whole, and leaving a game rewrites it only when pipelines were
# added since it was last written.
# The anchors include the timer the cost patches above add to the function.
pipeline_cache_writes = [
    ('PipelineCache::~PipelineCache() {\n'
     '    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {\n'
     '        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,\n'
     '                                     CACHE_VERSION);\n'
     '    }\n'
     '}\n',
     'PipelineCache::~PipelineCache() {\n'
     '    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {\n'
     '        // A flush still being written would race this one for the same file.\n'
     '        serialization_thread.WaitForRequests();\n'
     '        // Written after the game loaded and at each flush: with no pipeline added since,\n'
     '        // the file already holds everything, and writing it again only delays leaving.\n'
     '        const bool written = last_flush.time_since_epoch().count() != 0;\n'
     '        if (!written || pipelines_since_flush != 0) {\n'
     '            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,\n'
     '                                         CACHE_VERSION);\n'
     '        }\n'
     '    }\n'
     '}\n'),
    ('''void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) try {
    auto timer = Eden::Performance::VulkanTimer(18);
    std::ofstream file(filename, std::ios::binary);
    file.exceptions(std::ifstream::failbit);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
        return;
    }
    file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
        .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

    size_t cache_size = 0;
    std::vector<char> cache_data;
    if (pipeline_cache) {
        pipeline_cache.Read(&cache_size, nullptr);
        cache_data.resize(cache_size);
        pipeline_cache.Read(&cache_size, cache_data.data());
    }
    file.write(cache_data.data(), cache_size);

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));

} catch (const std::ios_base::failure& e) {
    LOG_ERROR(Common_Filesystem, "{}", e.what());
    if (!Common::FS::RemoveFile(filename)) {
        LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(filename));
    }
}
''',
'''void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) {
    auto timer = Eden::Performance::VulkanTimer(18);  // as the port times the write it replaces
    // Written whole beside the cache, then renamed over it: until the rename the previous cache
    // stays as it was, and a write that fails or is cut short leaves it alone.
    auto partial = filename;
    partial += ".part";
    std::error_code error;
    try {
        size_t cache_size = 0;
        std::vector<char> cache_data;
        if (pipeline_cache) {
            pipeline_cache.Read(&cache_size, nullptr);
            cache_data.resize(cache_size);
            pipeline_cache.Read(&cache_size, cache_data.data());
        }
        {
            std::ofstream file(partial, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                          Common::FS::PathToUTF8String(partial));
                return;
            }
            file.exceptions(std::ofstream::failbit | std::ofstream::badbit);
            file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
                .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version))
                .write(cache_data.data(), static_cast<std::streamsize>(cache_size));
            file.flush();
        }
        std::filesystem::rename(partial, filename, error);
        if (error) {
            LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                      Common::FS::PathToUTF8String(filename), error.message());
            std::filesystem::remove(partial, error);
            return;
        }
        LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
                 Common::FS::PathToUTF8String(filename));
    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        std::filesystem::remove(partial, error);
    } catch (const std::bad_alloc&) {
        // A copy of a large cache did not fit in memory: keep the one on disk.
        LOG_ERROR(Render_Vulkan, "No memory to copy the Vulkan driver pipeline cache; kept the previous file");
        std::filesystem::remove(partial, error);
    }
}
'''),
]
for old, new in pipeline_cache_writes:
    if shader_source.count(old) != 1:
        raise RuntimeError(f'Pipeline cache write anchor changed: {old[:60]}')
    shader_source = shader_source.replace(old, new)
# How many threads build pipelines (performance.h PipelineWorkers): at least a few on the console,
# where Eden's own count (the processor count less one) gave a large game's 13,938 cached pipelines
# about 25 a second at start, nine minutes of loading.
workers_old = '#else\n    return max_core_threads;\n#endif\n}\n'
if shader_source.count(workers_old) != 1:
    raise RuntimeError('Pipeline worker count anchor changed')
shader_source = shader_source.replace(workers_old,
    '#else\n    return ::Eden::Performance::PipelineWorkers(max_core_threads);\n#endif\n}\n')
# Pipeline builders placed on every CPU while the shader cache was built at start go back to the
# spare CPUs the next time they build, once the game runs (performance.h SettlePipelineBuilder).
for signature in (
    '    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,\n'
    '    bool build_in_parallel) try {\n',
    '    PipelineStatistics* statistics, bool build_in_parallel) try {\n',
):
    if shader_source.count(signature) != 1:
        raise RuntimeError(f'Pipeline builder settle anchor changed: {signature[:50]}')
    shader_source = shader_source.replace(signature, signature + '    ::Eden::Performance::SettlePipelineBuilder();\n')
# The shader cache's load (headless/shader_start.h). Read at a start and built before the game
# began, a large game's cache took 200 s at every start (Tears of the Kingdom, 16,000 pipelines).
# Now, unless the settings say otherwise ("background_shader_cache"), the game starts as soon as its
# graphics are ready and the cache is read and built behind it: the GPU thread takes each pipeline
# from the load once it is built, and builds one it needs before then itself, as it builds any new
# one (the load then leaves it out). The file is read as the builders need entries, a few per
# builder ahead, not all at once (the whole file was queued: the heap grew by 1.9 GB during Tears
# of the Kingdom's start, and it keeps what it grows by). A pipeline the GPU thread makes
# while the file is still being read is saved once the reading has ended, and only if the file
# does not have it. Leaving the game stops the load: the reader returns at its next entry, builds
# not begun are skipped, and the cache waits for the builds in progress before it goes. Every
# load, finished or stopped, leaves EDEN_SHADER_START lines in stderr.log: where its time went.
loader_registry = '''namespace {
// The load of one PipelineCache's shader cache: what its reader, its builders and its GPU thread
// share (headless/shader_start.h), found by the cache it loads for.
using EdenLoader = ::Eden::ShaderStart::Loader<GraphicsPipelineCacheKey, GraphicsPipeline,
                                               ComputePipelineCacheKey, ComputePipeline,
                                               Common::UniqueFunction<void>>;
struct EdenLoad {
    std::shared_ptr<EdenLoader> loader;
    std::unique_ptr<PipelineStatistics> statistics;
    long long read_wait_ns = 0;  // the reader's waits for room among the builds (its thread only)
    std::jthread reader;         // while the game plays; joined before the load is destroyed
};
std::mutex eden_loads_mutex;
std::unordered_map<const void*, std::unique_ptr<EdenLoad>> eden_loads;
std::atomic<unsigned> eden_loads_alive{0};

std::shared_ptr<EdenLoader> EdenLoaderOf(const void* cache) {
    if (eden_loads_alive.load(std::memory_order_acquire) == 0) {
        return {};
    }
    std::scoped_lock lock{eden_loads_mutex};
    const auto found = eden_loads.find(cache);
    return found == eden_loads.end() ? nullptr : found->second->loader;
}

// Stops a cache's load and waits for it to end: the reader returns at its next entry, builds not
// begun end at once, and each builder finishes the one it is on; what was built and not taken is
// destroyed. Before the cache goes, and before it loads another game's cache.
void EdenStopLoad(const void* cache) {
    std::unique_ptr<EdenLoad> load;
    {
        std::scoped_lock lock{eden_loads_mutex};
        const auto found = eden_loads.find(cache);
        if (found == eden_loads.end()) {
            return;
        }
        load = std::move(found->second);
        eden_loads.erase(found);
        eden_loads_alive.fetch_sub(1, std::memory_order_release);
    }
    load->loader->Stop();
    if (load->reader.joinable()) {
        load->reader.join();
    }
    load->loader->WaitIdle();
    (void)load->loader->TakeAll();
}

// A pipeline the GPU thread made, saved to the cache file unless the file has it already.
void EdenSave(const void* cache, Common::ThreadWorker& thread, u64 hash, bool compute,
              Common::UniqueFunction<void> save) {
    if (const auto loader = EdenLoaderOf(cache)) {
        if (auto now = loader->Save(hash, compute, std::move(save))) {
            thread.QueueWork(std::move(*now));
        }
        return;
    }
    thread.QueueWork(std::move(save));
}

// One admitted entry's build has ended, however it ended.
struct EdenBuildDone {
    EdenLoader& loader;
    ~EdenBuildDone() {
        loader.Done();
    }
};
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    // The shader cache's load first: its builders and reader use what follows.
    EdenStopLoad(this);
'''

def loader_body(filter_block):
    return '''    // Where the start's time goes (headless/shader_start.h): the files' sizes and the driver
    // cache's load here, every build's stages, and EDEN_SHADER_START lines in stderr.log at the end.
    const long long eden_start = ::Eden::ShaderStart::Now();
    const auto eden_heap = ::Eden::ShaderStart::HeapMark::Take();
    ::Eden::ShaderStart::Reset();
    ::Eden::ShaderStart::Summary eden_summary;
    eden_summary.background = ::Eden::ShaderStart::background.load(std::memory_order_relaxed);
    std::error_code eden_size_error;
    eden_summary.file_bytes = std::filesystem::file_size(pipeline_cache_filename, eden_size_error);
    if (eden_size_error) {
        eden_summary.file_bytes = 0;
    }
    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";
        eden_summary.driver_cache_bytes =
            std::filesystem::file_size(vulkan_pipeline_cache_filename, eden_size_error);
        if (eden_size_error) {
            eden_summary.driver_cache_bytes = 0;
        }
        const long long eden_driver_start = ::Eden::ShaderStart::Now();
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
        eden_summary.driver_cache_load_ns = ::Eden::ShaderStart::Now() - eden_driver_start;
    }

    // Built on the workers as the file is read, at most a few entries per worker ahead.
    eden_summary.workers =
        device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers();
    auto eden_new_load = std::make_unique<EdenLoad>();
    eden_new_load->loader = std::make_shared<EdenLoader>(
        eden_summary.workers * (eden_summary.background ? 2 : 4), eden_summary.file_bytes);
    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        eden_new_load->statistics = std::make_unique<PipelineStatistics>(device);
    }
    const std::shared_ptr<EdenLoader> loader = eden_new_load->loader;
    PipelineStatistics* const statistics = eden_new_load->statistics.get();
    EdenLoad* const load = eden_new_load.get();
    {
        std::scoped_lock lock{eden_loads_mutex};
        eden_loads[this] = std::move(eden_new_load);
        eden_loads_alive.fetch_add(1, std::memory_order_release);
    }

    auto load_compute{[this, loader, statistics, load](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        const u64 hash = key.Hash();
        loader->Note(hash, true, static_cast<u64>(file.tellg()));
        if (!loader->Admit(true, &load->read_wait_ns)) {
            return;
        }
        workers.QueueWork([this, loader, statistics, key, hash, env_ = std::move(env)]() mutable {
            const EdenBuildDone done{*loader};
            const ::Eden::ShaderStart::Job timing;
            if (!loader->Wanted(hash, true)) {
                return;
            }
            ShaderPools pools;
            auto pipeline{CreateComputePipeline(pools, key, env_, statistics, false)};
            pipeline = loader->OfferCompute(key, hash, std::move(pipeline));
        });
    }};
    auto load_graphics{[this, loader, statistics, load](std::ifstream& file,
                                                        std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        const u64 hash = key.Hash();
        loader->Note(hash, false, static_cast<u64>(file.tellg()));

''' + filter_block + '''        if (!loader->Admit(false, &load->read_wait_ns)) {
            return;
        }
        workers.QueueWork([this, loader, statistics, key, hash, envs_ = std::move(envs)]() mutable {
            const EdenBuildDone done{*loader};
            const ::Eden::ShaderStart::Job timing;
            if (!loader->Wanted(hash, false)) {
                return;
            }
            ShaderPools pools;
            boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
            for (auto& env : envs_) {
                env_ptrs.push_back(&env);
            }
            auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), statistics, false)};
            pipeline = loader->OfferGraphics(key, hash, std::move(pipeline));
        });
    }};

    // Reads the file and waits for the builds: on this thread before the game starts, or on a
    // thread of its own while the game plays.
    auto read_and_build = [this, loader, statistics, load, eden_start, eden_heap, eden_summary,
                           load_compute = std::move(load_compute),
                           load_graphics = std::move(load_graphics)]() mutable {
        ::Eden::ShaderStart::Summary summary = eden_summary;
        const long long read_start = ::Eden::ShaderStart::Now();
        VideoCommon::LoadPipelines(loader->StopToken(), pipeline_cache_filename, CACHE_VERSION,
                                   std::move(load_compute), std::move(load_graphics));
        summary.read_wait_ns = load->read_wait_ns;
        summary.read_ns = ::Eden::ShaderStart::Now() - read_start - summary.read_wait_ns;
        for (auto& save : loader->FinishReading(!loader->Stopping())) {
            serialization_thread.QueueWork(std::move(save));
        }
        const auto read = loader->GetCounts();
        LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", read.graphics + read.compute);
        const bool built = loader->WaitBuilt(std::chrono::seconds{5}, [&] {
            if (!summary.background) {
                return;
            }
            // While the file is still being read, its total is an estimate.
            const auto progress = ::Eden::ShaderStart::GetProgress();
            const long long seconds = (::Eden::ShaderStart::Now() - eden_start) / 1'000'000'000LL;
            ::Eden::Report("loader",
                           fmt::format("Shader cache: {} of {}{} pipelines built while the game runs ({} s)",
                                       progress.built, progress.reading ? "about " : "", progress.total,
                                       seconds)
                               .c_str());
        });
        if (built && statistics) {
            statistics->Report();
        }
        if (built && use_vulkan_pipeline_cache) {
            if (summary.background) {
                // The driver's cache with what the build added. While the game runs its own
                // flushes write the same file, on the serialization thread: so does this one.
                serialization_thread.QueueWork([this] {
                    SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                                 CACHE_VERSION);
                    size_t size = 0;
                    vulkan_pipeline_cache.Read(&size, nullptr);
                    last_cache_size.store(size, std::memory_order_relaxed);
                });
            } else {
                const long long save_start = ::Eden::ShaderStart::Now();
                SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                             CACHE_VERSION);
                size_t size = 0;
                vulkan_pipeline_cache.Read(&size, nullptr);
                last_cache_size.store(size, std::memory_order_relaxed);
                last_flush = std::chrono::steady_clock::now();
                summary.save_ns = ::Eden::ShaderStart::Now() - save_start;
            }
        }
        const auto counts = loader->GetCounts();
        summary.stopped = !built;
        summary.graphics = counts.graphics;
        summary.compute = counts.compute;
        summary.pipelines = counts.graphics + counts.compute;
        summary.kept = counts.kept;
        summary.gpu_built = counts.gpu_built;
        summary.taken = counts.taken;
        summary.wall_ns = ::Eden::ShaderStart::Now() - eden_start;
        ::Eden::ShaderStart::Report(stderr, summary, eden_heap);
        ::Eden::ShaderStart::SetProgress({});
        // What the build took of the heap and no longer uses goes back to the pool.
        ::Eden::Performance::TrimHeap("the shader cache was built");
        if (summary.background) {
            ::Eden::Report("loader",
                           fmt::format("Shader cache ready: {} pipelines in {} ms, built while the game ran{}",
                                       summary.pipelines, summary.wall_ns / 1'000'000,
                                       built ? "" : " (stopped: the game was left first)")
                               .c_str());
        }
    };

    if (eden_summary.background) {
        load->reader = std::jthread(std::move(read_and_build));
        return;
    }
    // Before the game starts: progress for the loading screen, and the caller's stop.
    loader->SetCallback([&callback](size_t built, size_t total) {
        callback(VideoCore::LoadCallbackStage::Build, built, total);
    });
    {
        const std::stop_callback stop_with_caller{stop_loading, [loader] { loader->Stop(); }};
        read_and_build();
    }
    loader->SetCallback({});
    // What was built goes to the caches now, while the GPU thread asks for none yet.
    auto [graphics_built, compute_built] = loader->TakeAll();
    for (auto& [built_key, pipeline] : graphics_built) {
        graphics_cache.emplace(built_key, std::move(pipeline));
    }
    for (auto& [built_key, pipeline] : compute_built) {
        compute_cache.emplace(built_key, std::move(pipeline));
    }
    EdenStopLoad(this);
}
'''

load_begin = shader_source.index('void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,')
load_end = shader_source.index('void PipelineCache::QueueVulkanPipelineCacheFlush() {')
load_source = shader_source[load_begin:load_end]
title_check = '    if (title_id == 0) {\n        return;\n    }\n'
tail_begin = load_source.index('    if (use_vulkan_pipeline_cache) {\n        vulkan_pipeline_cache_filename = base_dir / "vulkan_pipelines.bin";')
filter_begin = load_source.index('        if ((key.state.extended_dynamic_state != 0) !=')
filter_end = load_source.index('        workers.QueueWork([this, key, envs_ = std::move(envs), &state, &callback]() mutable {')
if (load_source.count(title_check) != 1 or not load_source.endswith('    if (state.statistics) {\n        state.statistics->Report();\n    }\n}\n\n')
        or not tail_begin < filter_begin < filter_end):
    raise RuntimeError('Shader cache load anchors changed')
load_source = (load_source[:tail_begin].replace(title_check, title_check + '    EdenStopLoad(this);\n') +
               loader_body(load_source[filter_begin:filter_end]) + '\n')
shader_source = shader_source[:load_begin] + load_source + shader_source[load_end:]
for old, new in (
    ('PipelineCache::~PipelineCache() {\n', loader_registry),
    # The GPU thread: a pipeline the load built, else its own.
    ('    if (is_new) {\n        pipeline = CreateGraphicsPipeline();\n    }\n',
     '    if (is_new) {\n'
     '        if (const auto loader = EdenLoaderOf(this)) {\n'
     '            pipeline = loader->TakeGraphics(graphics_key, graphics_key.Hash());\n'
     '        }\n'
     '        if (!pipeline) {\n'
     '            pipeline = CreateGraphicsPipeline();\n'
     '        }\n'
     '    }\n'),
    ('    if (is_new) {\n        pipeline = CreateComputePipeline(key, shader);\n    }\n',
     '    if (is_new) {\n'
     '        if (const auto loader = EdenLoaderOf(this)) {\n'
     '            pipeline = loader->TakeCompute(key, key.Hash());\n'
     '        }\n'
     '        if (!pipeline) {\n'
     '            pipeline = CreateComputePipeline(key, shader);\n'
     '        }\n'
     '    }\n'),
    # Its saves, once it is known whether the file has them.
    ('    serialization_thread.QueueWork([this, key = graphics_key, envs = std::move(environments.envs)] {\n',
     '    EdenSave(this, serialization_thread, graphics_key.Hash(), false,\n'
     '             [this, key = graphics_key, envs = std::move(environments.envs)] {\n'),
    ('    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {\n',
     '    EdenSave(this, serialization_thread, key.Hash(), true, [this, key, env_ = std::move(env)] {\n'),
    # Whether the driver can say how it made a pipeline (pipeline creation feedback, Vulkan 1.3).
    ('    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));\n',
     '    ::Eden::ShaderStart::feedback.store(device.ApiVersion() >= VK_API_VERSION_1_3,\n'
     '                                        std::memory_order_relaxed);\n'
     '    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));\n'),
    ('#include "dev_vulkan.h"\n#include "diagnostics.h"',
     '#include "dev_vulkan.h"\n#include "diagnostics.h"\n#include "shader_start.h"\n#include <unordered_map>'),
):
    if shader_source.count(old) != 1:
        raise RuntimeError(f'Shader cache load anchor changed: {old[:60]}')
    shader_source = shader_source.replace(old, new)
shader_output = output / 'vk_pipeline_cache_cost.cpp'
if not shader_output.exists() or shader_output.read_text() != shader_source:
    shader_output.write_text(shader_source)

# Scope only the existing synchronous operations; never alter their ordering.
# Report these caller costs separately from the overlapping API totals.
import re
# Development GPU fault probes (dev_vulkan.h): multi-range storage buffers are
# the one path that binds sparse ranges of the console's device memory.
probes = {
    'vk_multi_range_buffer': [
        ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
        ('    if (bind_result != VK_SUCCESS) {\n        return SparseBuffer{};\n    }',
         '    if (bind_result != VK_SUCCESS) {\n'
         '        if (::Eden::DevVulkan::trace_pipelines) ::Eden::Report("multirange", "sparse bind failed");\n'
         '        return SparseBuffer{};\n    }'),
        ('    retired.push_back(Retired{',
         '    if (::Eden::DevVulkan::trace_pipelines) {\n'
         '        static unsigned retires = 0;\n'
         '        if (++retires <= 64) {\n'
         '            char text[96];\n'
         '            std::snprintf(text, sizeof(text), "retire #%u sparse=%d tick=%llu", retires,\n'
         '                          int(bool(entry.sparse_handle)), (unsigned long long)scheduler.CurrentTick());\n'
         '            ::Eden::Report("multirange", text);\n'
         '        }\n'
         '    }\n'
         '    retired.push_back(Retired{'),
        ('    entries.emplace(key, std::move(entry));',
         '    if (::Eden::DevVulkan::trace_pipelines) {\n'
         '        static unsigned created = 0;\n'
         '        if (++created <= 256) {\n'
         '            char text[192];\n'
         '            std::snprintf(text, sizeof(text), "new #%u key=%llx total=%llu sources=%zu sparse=%d addr=%llx",\n'
         '                          created, (unsigned long long)key, (unsigned long long)total, sources.size(),\n'
         '                          int(bool(entry.sparse_handle)), (unsigned long long)entry.address);\n'
         '            ::Eden::Report("multirange", text);\n'
         '        }\n'
         '    }\n'
         '    entries.emplace(key, std::move(entry));'),
    ],
    'vk_buffer_cache': [
        ('#include <algorithm>', '#include <algorithm>\n#include "dev_vulkan.h"'),
        ('bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {\n',
         'bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {\n'
         '    if (::Eden::DevVulkan::disable_multi_range) {\n        return false;\n    }\n'),
    ],
    # With custom border colours off (the console default), report which clamp-to-border
    # samplers ask for a colour the three standard ones cannot give exactly.
    'vk_texture_cache': [
        ('#include <algorithm>', '#include <algorithm>\n#include <cstdio>\n#include "dev_vulkan.h"\n#include "diagnostics.h"'),
        ('    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();',
         '    if (::Eden::DevVulkan::trace_pipelines && samples_border) {\n'
         '        static std::vector<std::array<float, 4>> reported;\n'
         '        const auto report = [](const std::array<float, 4>& c, const char* kind) {\n'
         '            if (c == std::array<float, 4>{0, 0, 0, 0} || c == std::array<float, 4>{0, 0, 0, 1} ||\n'
         '                c == std::array<float, 4>{1, 1, 1, 1} || reported.size() >= 32 ||\n'
         '                std::find(reported.begin(), reported.end(), c) != reported.end()) return;\n'
         '            reported.push_back(c);\n'
         '            char text[128];\n'
         '            std::snprintf(text, sizeof(text), "non-standard %s border %g %g %g %g", kind, c[0], c[1],\n'
         '                          c[2], c[3]);\n'
         '            ::Eden::Report("sampler", text);\n'
         '        };\n'
         '        report(border_color, "linear");\n'
         '        if (tsc.srgb_conversion) report(srgb_border_color, "srgb");\n'
         '    }\n'
         '    has_custom_border_colors = samples_border && device.IsCustomBorderColorUsable();'),
    ],
}
for name, index, expected in (
    ('vk_fence_manager', 6, 1), ('vk_buffer_cache', 7, 3),
    ('vk_texture_cache', 8, 1), ('vk_swapchain', 9, 6),
    ('vk_descriptor_buffer', 10, 2), ('vk_multi_range_buffer', 11, 1),
    ('vk_compute_pass', 12, 1),
):
    relative = f'src/video_core/renderer_vulkan/{name}.cpp'
    destination = 'vulkan_swapchain.cpp' if name == 'vk_swapchain' else name + '_cost.cpp'
    # Preserve the earlier swapchain error-recovery adaptation.
    path = output / destination
    text = path.read_text() if name == 'vk_swapchain' else (source / relative).read_text()
    pattern = r'(?m)^( +)(scheduler\.(?:Wait|Finish)\([^\n]*\);)$'
    matches = list(re.finditer(pattern, text))
    if len(matches) != expected:
        raise RuntimeError(f'Wait-site anchors changed: {name}: {len(matches)} != {expected}')
    text = re.sub(pattern, lambda m: m[1] + '{ auto timer = Eden::Performance::VulkanTimer('
                  + str(index) + '); ' + m[2] + ' }', text)
    if name == 'vk_compute_pass':
        # ASTC decode has no CPU consumer. Its existing image barrier orders
        # later shaders, and staging/descriptor reuse is protected by ticks.
        text = text.replace('VulkanTimer(12); scheduler.Finish();',
                            'VulkanTimer(12); scheduler.Flush();')
    for probe_old, probe_new in probes.get(name, []):
        if text.count(probe_old) != 1:
            raise RuntimeError(f'Probe anchor changed: {name}: {probe_old[:60]}')
        text = text.replace(probe_old, probe_new)
    text = '#include "performance.h"\n' + text
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
# Eden loads these shaders when the renderer starts, whether frames are to be generated or not:
# the reasons are only written when the setting is on, or every session without the user's
# Lossless.dll would say so.
adapt('src/video_core/renderer_vulkan/present/lsfg_shaders.cpp', 'vulkan_lsfg_shaders.cpp', [
    ('#include "video_core/vulkan_common/vulkan_device.h"',
     '#include <string>\n#include "common/settings.h"\n#include "video_core/vulkan_common/vulkan_device.h"\n#include "diagnostics.h"'),
    ("""    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor() ||
        !device.IsFloat16Supported()) {
        return;
    }""",
     """    const bool wanted = Settings::values.frame_gen.GetValue();
    if (!device.IsVulkanMemoryModelSupported() || !device.HasNullDescriptor() ||
        !device.IsFloat16Supported()) {
        if (wanted)
            Eden::Report("framegen",
                         ("device features: vulkanMemoryModel=" +
                          std::to_string(device.IsVulkanMemoryModelSupported()) + " nullDescriptor=" +
                          std::to_string(device.HasNullDescriptor()) + " float16=" +
                          std::to_string(device.IsFloat16Supported()) +
                          " -- all are required, and dev-settings.txt null_descriptor=off turns "
                          "the second one off").c_str());
        return;
    }"""),
    ("""    VideoCore::FrameGen::ShaderModules code;
    if (VideoCore::FrameGen::LoadShaderModules(code) != VideoCore::FrameGen::LosslessStatus::Ok) {
        return;
    }""",
     """    VideoCore::FrameGen::ShaderModules code;
    const VideoCore::FrameGen::LosslessStatus status = VideoCore::FrameGen::LoadShaderModules(code);
    if (status != VideoCore::FrameGen::LosslessStatus::Ok) {
        if (wanted)
            Eden::Report("framegen",
                         ("Lossless.dll " + VideoCore::FrameGen::GetLosslessDllPath().string() +
                          " status=" + std::to_string(static_cast<u32>(status)) +
                          " (0 ok, 1 not installed, 2 unreadable, 3 not a PE, 4 no shaders in it, "
                          "5 translation failed, 6 cache unusable)").c_str());
        return;
    }"""),
    ("""    valid = true;""",
     """    valid = true;
    if (wanted)
        Eden::Report("framegen", ("shaders ready: " + std::to_string(code.size()) + " modules").c_str());"""),
])
# Two members, so that "frames are being generated" and "this image cannot take them" are said once
# per session rather than once per process or once per frame: this object is built per renderer,
# and the renderer is built per game session.
adapt('src/video_core/renderer_vulkan/present/frame_gen.h',
      'include/video_core/renderer_vulkan/present/frame_gen.h', [
    ('    bool dumped{};', '    bool dumped{};\n    bool generation_reported{};\n    bool storage_reported{};'),
])
adapt('src/video_core/renderer_vulkan/present/frame_gen.cpp', 'vulkan_frame_gen.cpp', [
    ('#include "video_core/vulkan_common/vulkan_device.h"',
     '#include "video_core/vulkan_common/vulkan_device.h"\n#include "diagnostics.h"'),
    ("""    if (!shaders->IsValid()) {
        unavailable = true;
        return;
    }""",
     """    if (!shaders->IsValid()) {
        Eden::Report("framegen", "LsfgShaders is not valid; see the lines above");
        unavailable = true;
        return;
    }"""),
    ("""    if (!frame->storage_view) {
        warm_streak = 0;
        return;
    }""",
     """    if (!frame->storage_view) {
        if (!storage_reported) {
            storage_reported = true;
            Eden::Report("framegen",
                         "the frame image has no storage view: the swapchain format lacks "
                         "VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, so frames cannot be generated");
        }
        warm_streak = 0;
        return;
    }"""),
    # The gates above only say why it is not running; this says that it is.
    ("""    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;""",
     """    generated = warm && warm_streak >= LSFG_RECURRENCE_FRAMES && plan.generations > 0;
    if (generated && !generation_reported) {
        generation_reported = true;
        Eden::Report("framegen",
                     ("generating " + std::to_string(plan.generations) +
                      " extra frame(s) per guest frame").c_str());
    }"""),
])
# Split the dispatch into its three phases (Tegra::Control::Scheduler::Push): the channel lock,
# queueing the submit list, and executing the guest's methods. The dispatch timer wraps the whole
# call from gpu_thread.cpp, so these say which of the three it is spent in.
adapt('src/video_core/control/scheduler.cpp', 'control_scheduler.cpp', [
    ('#include <memory>', '#include <memory>\n#include "performance.h"'),
    ('        std::unique_lock lk(scheduling_guard);\n'
     '        auto it = channels.find(channel);\n'
     '        ASSERT(it != channels.end());\n'
     '        channel_state = it->second;\n'
     '        gpu.BindChannel(channel_state->bind_id);\n'
     '    }\n'
     '    // Process commands outside the lock to reduce contention.\n'
     '    // Multiple channels can prepare their commands in parallel.\n'
     '    channel_state->payload->dma_pusher.Push(std::move(entries));\n'
     '    channel_state->payload->dma_pusher.DispatchCalls();',
     '        const ::Eden::Performance::Timer sched_lock_timer(::Eden::Performance::gpu_sched_lock);\n'
     '        std::unique_lock lk(scheduling_guard);\n'
     '        auto it = channels.find(channel);\n'
     '        ASSERT(it != channels.end());\n'
     '        channel_state = it->second;\n'
     '        gpu.BindChannel(channel_state->bind_id);\n'
     '    }\n'
     '    // Process commands outside the lock to reduce contention.\n'
     '    // Multiple channels can prepare their commands in parallel.\n'
     '    {\n'
     '        const ::Eden::Performance::Timer dma_push_timer(::Eden::Performance::gpu_dma_push);\n'
     '        channel_state->payload->dma_pusher.Push(std::move(entries));\n'
     '    }\n'
     '    {\n'
     '        const ::Eden::Performance::Timer dma_dispatch_timer(::Eden::Performance::gpu_dma_dispatch);\n'
     '        channel_state->payload->dma_pusher.DispatchCalls();\n'
     '    }'),
])
shader_header = '#pragma once\n#include <cstdint>\n'
for stage in ('vert', 'frag'):
    binary = output / f'vulkan_hud.{stage}.spv'
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.0',
                    '-o', str(binary), str(port / f'vulkan_hud.{stage}')], check=True)
    subprocess.run(['spirv-val', '--target-env', 'vulkan1.0', str(binary)], check=True)
    data = binary.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    shader_header += f'inline constexpr uint32_t EDEN_HUD_{stage.upper()}_SPV[] = {{\n'
    shader_header += ','.join(hex(word) for word in words) + '\n};\n'
path = output / 'vulkan_hud_shaders.h'
if not path.exists() or path.read_text() != shader_header:
    path.write_text(shader_header)
