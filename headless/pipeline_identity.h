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
// A graphics line ends with spv=: a hash of the SPIR-V Eden made for each host stage the pipeline
// has (vs, tc, te, gs, fs; a stage it lacks is left out), e.g. spv=vs:1f..,fs:9a... Eden writes a
// stage's SPIR-V from its program and some of the pipeline's state (attribute types, alpha test,
// point size, y negation, early z...), so two pipelines with the same stage hashes can still differ
// there. A new pipeline whose every stage hash was printed before differs from earlier ones only
// in state outside its shaders: with graphics pipeline libraries it would need a link and no
// shader compile (tools/analyze-pipeline-reuse.py counts them in a stderr.log).
//
// stderr is fully buffered (log_flusher.h): a line of about 150 bytes, once per pipeline.
#pragma once
#include <array>
#include <cstddef>
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

// The SPIR-V hashes of the pipeline being made on this thread (PipelineCache::CreateGraphicsPipeline
// notes each stage as it is written; the GraphicsPipeline it then constructs takes them). Host
// stages: 0 vertex, 1 tessellation control, 2 tessellation evaluation, 3 geometry, 4 fragment.
using SpirvHashes = std::array<std::uint64_t, 5>;
inline thread_local SpirvHashes spirv_hashes{};
inline constexpr const char* kSpirvStages[] = {"vs", "tc", "te", "gs", "fs"};

// 64-bit FNV-1a taking the code a 32-bit word at a time (a stage of tens of kilobytes in a few
// microseconds), then mixed; never 0, which is a stage the pipeline lacks.
template <typename Words>
inline std::uint64_t SpirvHash(const Words& code) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto word : code) {
        hash ^= static_cast<std::uint32_t>(word);
        hash *= 0x100000001b3ULL;
    }
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    return hash != 0 ? hash : 1;
}
inline void BeginSpirv() {
    spirv_hashes = {};
}
template <typename Words>
inline void NoteSpirv(std::size_t stage, const Words& code) {
    if (stage < spirv_hashes.size()) spirv_hashes[stage] = SpirvHash(code);
}
inline SpirvHashes TakeSpirv() {
    const SpirvHashes taken = spirv_hashes;
    spirv_hashes = {};
    return taken;
}

template <typename Handle>
inline void Graphics(Handle pipeline, std::uint64_t key_hash, const std::array<std::uint64_t, 6>& stages,
                     const SpirvHashes& spirv = {}) {
    char line[320];
    int at = std::snprintf(line, sizeof(line),
                           "EDEN_PIPELINE vk=0x%llx graphics key=%llx va=%llx vb=%llx tc=%llx te=%llx gs=%llx "
                           "fs=%llx",
                           static_cast<unsigned long long>(HandleValue(pipeline)),
                           static_cast<unsigned long long>(key_hash), static_cast<unsigned long long>(stages[0]),
                           static_cast<unsigned long long>(stages[1]), static_cast<unsigned long long>(stages[2]),
                           static_cast<unsigned long long>(stages[3]), static_cast<unsigned long long>(stages[4]),
                           static_cast<unsigned long long>(stages[5]));
    const char* separator = " spv=";
    for (std::size_t stage = 0; stage < spirv.size(); ++stage) {
        if (spirv[stage] == 0 || at <= 0 || at >= static_cast<int>(sizeof(line))) continue;
        at += std::snprintf(line + at, sizeof(line) - static_cast<std::size_t>(at), "%s%s:%llx", separator,
                            kSpirvStages[stage], static_cast<unsigned long long>(spirv[stage]));
        separator = ",";
    }
    std::fprintf(stderr, "%s\n", line);
}

template <typename Handle>
inline void Compute(Handle pipeline, std::uint64_t shader_hash) {
    std::fprintf(stderr, "EDEN_PIPELINE vk=0x%llx compute cs=%llx\n",
                 static_cast<unsigned long long>(HandleValue(pipeline)),
                 static_cast<unsigned long long>(shader_hash));
}

} // namespace Eden::PipelineIdentity
