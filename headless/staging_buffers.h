// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: the size of the staging buffers Eden's staging pool makes, and what the pool lets
// go of when the console refuses one (tools/prepare-vulkan-port.py, vulkan_staging_buffer_pool.cpp).
//
// Eden makes a staging buffer the next power of two above its transfer: 256 MiB for a transfer of
// 130 MiB. On the console a buffer that large is a block of its own, in one piece, in the pool the
// CPU shares with the GPU, so that was 126 MiB asked for and never used, and a request a
// fragmented pool can refuse. Metroid Prime 4 at 2x ended "out of graphics memory" asking for a
// 256 MiB staging buffer while the pool's largest free block was 64 MiB (#195). Above 16 MiB a
// staging buffer the CPU writes or reads is now its transfer rounded up to 16 MiB. Up to 16 MiB,
// and for the buffers only the GPU uses (index conversions, a few MiB), it is Eden's power of two.
//
// Eden keeps a staging buffer the GPU is done with for the next transfer of the same power of two.
// Above 16 MiB such a buffer can now be smaller than that transfer, so it is handed out only when
// it holds the transfer. A kept buffer too small for a new transfer is let go before the new one
// is made, instead of staying beside it until Eden's own release comes round to its size.
//
// When the console refuses a staging buffer, every kept one the GPU is done with is let go and the
// buffer is asked for once more. Refused again, the error goes on as before, and the session ends
// with "ran out of graphics memory".
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Eden::StagingBuffers {
// Above this a transfer gets a buffer of its own size, rounded up to kStep.
inline constexpr std::uint64_t kExactAbove = std::uint64_t{16} << 20;
inline constexpr std::uint64_t kStep = std::uint64_t{16} << 20;

// Whether a transfer of `size` bytes gets a buffer of its own size. `mapped`: the CPU writes or
// reads the buffer (Eden's Upload and Download kinds), so it is mapped, and its mapping says how
// large it is. Eden's buffers for the GPU alone are not mapped and keep their power of two.
constexpr bool Exact(std::uint64_t size, bool mapped) noexcept {
    return mapped && size > kExactAbove;
}

// The size of the buffer made for a transfer of `size` bytes; `log2` is Eden's Log2Ceil of it.
constexpr std::uint64_t Size(std::uint64_t size, unsigned log2, bool exact) noexcept {
    return exact ? (size + kStep - 1) / kStep * kStep : std::uint64_t{1} << log2;
}

// What a kept buffer holds: its mapping's size or, not mapped, Eden's power of two.
constexpr std::uint64_t Holds(std::uint64_t mapped_bytes, unsigned log2) noexcept {
    return mapped_bytes != 0 ? mapped_bytes : std::uint64_t{1} << log2;
}

struct Released {
    std::size_t buffers = 0;
    std::uint64_t bytes = 0;
    Released& operator+=(const Released& other) noexcept {
        buffers += other.buffers;
        bytes += other.bytes;
        return *this;
    }
};

// Lets go of the kept buffers of one of the pool's power-of-two levels that `drop` picks. The
// level is Eden's StagingBufferPool::StagingBuffers: its entries, and the two places in them its
// next search and its next release start from, which stay inside the list as in Eden's own
// release (ReleaseLevel).
template <class Level, class Drop>
Released ReleaseFrom(Level& level, Drop&& drop) {
    Released released;
    auto& entries = level.entries;
    const auto kept = std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
        if (!drop(entry)) return false;
        ++released.buffers;
        released.bytes += Holds(entry.mapped_span.size(), entry.log2_level);
        return true;
    });
    entries.erase(kept, entries.end());
    if (level.delete_index >= entries.size()) level.delete_index = 0;
    if (level.iterate_index > entries.size()) level.iterate_index = 0;
    return released;
}
} // namespace Eden::StagingBuffers
