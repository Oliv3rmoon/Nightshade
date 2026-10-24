// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: a line in the log while the texture collector frees textures because memory is
// short (tools/prepare-vulkan-port.py, the Vulkan texture cache).
//
// The texture and buffer caches measure what is free from the pool the CPU shares with the GPU
// (performance.h, graphics_memory_free). Once its largest free block is under about 0.8 GiB the
// texture collector also frees ASTC textures kept as BC3 that were not drawn for 10 frames; they
// are converted again on the CPU the next time they are drawn, during play in the background, so
// they are drawn before they are ready (headless/astc_policy.h). Under about 1 GiB it also frees
// textures the GPU wrote that were not drawn for 25 frames (10 in that pass), copied back to guest
// memory first (vulkan_gc_downloads.inc). A game whose textures go and come back like that shows
// them missing for a while. Whether that is what happens when a game loses its textures after
// some minutes (#273, a large open-world game with a resolution mod) is not known, so while it
// goes on the log says so, every 10 s at most:
//
//   graphics memory: textures freed while memory is short, in 10 s: 140, 38 of them ASTC kept as
//   BC3, 12 written by the GPU; up to 9 conversions waiting; the pool's largest free block is
//   702 MiB, the allocator holds 6120 MiB
//
// (one line). Textures that go missing with no such line around were not taken by the collector.
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdio>

namespace Eden::TextureEvictions {
// What the collector freed since the last line. GPU thread only.
struct Window {
    long long begin_ns = 0;
    unsigned long long freed = 0, converted = 0, written = 0;
    std::size_t waiting = 0; // the most conversions queued at the end of a frame
};
inline Window window;
inline constexpr long long kWindowNs = 10'000'000'000;

// A texture the collector frees. `aggressive`: it frees in its pass for memory past the critical
// mark; `converted`: an ASTC texture kept as BC3; `written`: the GPU wrote it and it is copied back.
// Textures freed in the collector's other passes are loaded again in place and are not counted.
inline void Freed(bool aggressive, bool converted, bool written) noexcept {
    if (!aggressive && !written) return;
    ++window.freed;
    window.converted += converted;
    window.written += written;
}

// At the end of every frame, at `now_ns`, with `waiting` conversions queued. When 10 s have gone
// since the window began and it freed textures, writes the line to `to` with the pool's largest
// free block and what the allocator holds (either may be null) and starts a new window. Returns
// whether it wrote the line.
inline bool FrameAt(long long now_ns, std::size_t waiting, unsigned long long (*free_block)(),
                    unsigned long long (*held)(), std::FILE* to) {
    if (window.begin_ns == 0) window.begin_ns = now_ns;
    if (waiting > window.waiting) window.waiting = waiting;
    const long long elapsed = now_ns - window.begin_ns;
    if (elapsed < kWindowNs) return false;
    const bool line = window.freed != 0;
    if (line) {
        std::fprintf(to, "[ProsperoEden] graphics memory: textures freed while memory is short, in %lld s: %llu, "
                         "%llu of them ASTC kept as BC3, %llu written by the GPU; up to %zu conversions waiting",
                     elapsed / 1'000'000'000, window.freed, window.converted, window.written, window.waiting);
        if (free_block) std::fprintf(to, "; the pool's largest free block is %llu MiB", free_block() >> 20);
        if (held) std::fprintf(to, ", the allocator holds %llu MiB", held() >> 20);
        std::fputc('\n', to);
    }
    window = Window{};
    window.begin_ns = now_ns;
    return line;
}

inline bool Frame(std::size_t waiting, unsigned long long (*free_block)(), unsigned long long (*held)()) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return FrameAt(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), waiting, free_block, held,
                   stderr);
}
} // namespace Eden::TextureEvictions
