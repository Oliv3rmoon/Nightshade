// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: one line on stderr for each pipeline Eden builds, new or from the game's shader
// cache, with Eden's own hashes beside the VkPipeline handle the driver returned
// (tools/prepare-vulkan-port.py calls it from vk_graphics_pipeline.cpp and vk_compute_pipeline.cpp
// once the pipeline exists).
//
// When the GPU stops, the driver's stall dump (Belladonna, radv_ps5_winsys.c) names the
// shader code each of the last draws and dispatches ran as "pipeline <RADV hash> vk=0x<handle>".
// This line turns the handle into what the game's transferable shader cache (vulkan.bin) files the
// pipeline under: a graphics pipeline's key hash and the unique hash of each stage's program
// (va VertexA, vb VertexB, tc TessellationControl, te TessellationEval, gs Geometry, fs Fragment;
// 0 for a stage the pipeline does not use; the names the development trace EDEN_PIPELINE_FIRST_USE
// uses), a compute pipeline's program hash (cs). A handle can be reused once a pipeline is
// destroyed, so the line printed last before the stall is the one that counts.
//
// stderr is fully buffered (log_flusher.h): a line of about a hundred bytes, once per pipeline.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <type_traits>

namespace Eden::PipelineIdentity {

// A Vulkan non-dispatchable handle as the driver prints it: a pointer on 64-bit builds.
template <typename Handle>
inline std::uint64_t HandleValue(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

template <typename Handle>
inline void Graphics(Handle pipeline, std::uint64_t key_hash, const std::array<std::uint64_t, 6>& stages) {
    std::fprintf(stderr,
                 "EDEN_PIPELINE vk=0x%llx graphics key=%llx va=%llx vb=%llx tc=%llx te=%llx gs=%llx "
                 "fs=%llx\n",
                 static_cast<unsigned long long>(HandleValue(pipeline)),
                 static_cast<unsigned long long>(key_hash), static_cast<unsigned long long>(stages[0]),
                 static_cast<unsigned long long>(stages[1]), static_cast<unsigned long long>(stages[2]),
                 static_cast<unsigned long long>(stages[3]), static_cast<unsigned long long>(stages[4]),
                 static_cast<unsigned long long>(stages[5]));
}

template <typename Handle>
inline void Compute(Handle pipeline, std::uint64_t shader_hash) {
    std::fprintf(stderr, "EDEN_PIPELINE vk=0x%llx compute cs=%llx\n",
                 static_cast<unsigned long long>(HandleValue(pipeline)),
                 static_cast<unsigned long long>(shader_hash));
}

} // namespace Eden::PipelineIdentity
