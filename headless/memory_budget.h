// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: who may hold how much of the memory pool the CPU shares with the GPU, and how
// short memory is seen. Plain functions and constants (no platform code), so the host checks run
// the same rules (tools/check-memory-soak.py).
//
// The pool is 12,416 MiB in a game (sceKernelGetDirectMemorySize: 13,019,119,616 bytes). Its
// consumers and what each may hold in a long session of a large open-world game (Tears of the
// Kingdom, the largest measured), with what was measured on v1.010.004 to v1.010.006:
//
//   guest      the game's own memory, 4 GiB at most (its layout), backed where it is written.
//              TOTK: 1.3 GB at start, 2.9 GB at most. Plus its headroom of 1 GiB (kGuestReserve),
//              which the memory service holds as blocks of its own.
//   heap       Nightshade's own memory (headless/heap_arenas.inc). Its pieces and arena slots hold
//              what its blocks use (TOTK: 2.4 GB at start with 18,881 pipelines, 2.9 GB at most)
//              and a slack of an eighth of that; the rest goes back (kHeapTarget is what the
//              checks hold a TOTK start to). v1.010.006 held 4.5 to 5.0 GB.
//   tables     the guest's page tables and Eden's other sparse tables: 56 to 150 MiB.
//   jit        the code caches: what the JIT has written, 192 MiB in TOTK.
//   driver     the graphics driver's own blocks (its command ring, the stream buffer of 256 MiB)
//              and the threads' stacks: about 600 MiB that no line of the log counts.
//   graphics   the graphics allocator's blocks (64 MiB each) and the images and buffers that
//              have memory of their own: the rest. TOTK held 1.1 to 4.6 GB.
//
// Every request must also find one free block of its size in the pool: the driver gives a block
// of graphics memory one run of the pool. So no transfer is staged in more than kStagingChunk at
// once (headless/staging_chunks.h), the caches give memory back while the largest free block is
// under kShortMemory, and the heap's free memory, the idle staging buffers and the textures go back
// in that order (performance.cpp RelieveMemory, the texture collector).
#pragma once
#include <cstddef>

namespace Eden::Performance {
inline constexpr std::size_t kMiB = std::size_t{1} << 20;
// The pool in a game, and the budget of each consumer (bytes). The graphics allocator takes what
// the others leave; kGraphicsFloor is the least it is left in the largest TOTK session measured.
inline constexpr std::size_t kPoolBytes = std::size_t{12416} * kMiB;
inline constexpr std::size_t kGuestBudget = std::size_t{4096} * kMiB;
inline constexpr std::size_t kHeapTarget = std::size_t{3072} * kMiB;
inline constexpr std::size_t kTablesBudget = std::size_t{192} * kMiB;
inline constexpr std::size_t kJitBudget = std::size_t{384} * kMiB;
inline constexpr std::size_t kDriverBudget = std::size_t{640} * kMiB;

// 1 GiB: the pool is the CPU's too, and the largest thing a game asked of it at once was a
// block of 486 MiB (a texture array of 121 layers, decoded before it is uploaded). With 384 MiB
// here the collector let the pool run down to 620 MiB in one block at 2x, and that request
// ended the session.
inline constexpr std::size_t kShortMemory = std::size_t{1} << 30;
// The guest's headroom in the pool: 1 GiB that a game's own memory may take at once while the
// graphics caches give memory back (a large open-world game wrote 0.5 to 1.8 GB within seconds).
// The memory service holds it as blocks of its own (src/guest_memory_service.inc, the reserve).
inline constexpr std::size_t kGuestReserve = std::size_t{1} << 30;
inline constexpr std::size_t kGraphicsFloor =
    kPoolBytes - kGuestBudget - kGuestReserve - kHeapTarget - kTablesBudget - kJitBudget - kDriverBudget;

// What the caches see of the pool (performance.cpp, the PS5's RefreshFreeMemory): its largest free
// block less the part of the guest's headroom the service does not hold yet (all of it when no
// service holds any: a 32-bit game, or Compact memory's service switched off), and memory short
// when that is under kShortMemory or a burst of the game's writes has taken half the headroom, so
// the caches start giving memory back before a write has to wait for them. `known`: the pool
// could be read; `held`, `target`: the service's reserve now and in full (bytes, both 0 without).
struct PoolView {
    std::size_t free;
    bool short_memory;
};
inline PoolView ViewOfPool(bool known, std::size_t largest, std::size_t held, std::size_t target) {
    if (!known) return {0, true};
    const std::size_t headroom = target == 0 ? kGuestReserve : held < target ? target - held : 0;
    const std::size_t free = largest > headroom ? largest - headroom : 0;
    return {free, free < kShortMemory || (target != 0 && held < target / 2)};
}

// Graphics memory still free for the texture and buffer caches: the pool's (ViewOfPool) and three
// quarters of what the graphics allocator's blocks hold unused (`held` less `used`; some of it is
// in runs too small for the next image). The caches evict by "memory in use", the budget less
// this. With the pool alone, a Tears of the Kingdom session whose allocator held 3.6 GB in blocks
// with 1.4 GB in use saw memory short for good: the collector evicted at its hardest every frame
// (3,000 textures freed every 10 s, those the GPU wrote copied back first), the game drew them
// again, and it ran at a few frames a second after its item menu; and freeing an image puts its
// memory back in the allocator's block, not in the pool, so the pool never looked better.
inline std::size_t CachesFree(std::size_t pool_free, unsigned long long held, unsigned long long used) {
    const unsigned long long unused = held > used ? held - used : 0;
    return pool_free + static_cast<std::size_t>(unused / 4 * 3);
}
// The caches evict textures the GPU wrote (copied back to the game's memory first, at a wait for
// the GPU each) only while this is short too: evicting them while the allocator's blocks hold
// that much unused gives the pool nothing.
inline bool CachesShort(std::size_t caches_free) {
    return caches_free < kShortMemory;
}
} // namespace Eden::Performance
