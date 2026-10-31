// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A long session of a large open-world game against the memory pool the CPU shares with the GPU,
// on the host (tools/check-memory-soak.py builds and runs it).
//
// Real code: Nightshade's heap (third_party/app_heap.c with headless/heap_arenas.inc, compiled
// as the console build derives it), the transfer chunker (headless/staging_chunks.h) and the
// pressure rules (headless/memory_budget.h ViewOfPool, CachesFree, CachesShort). Models, after the
// console's logs: the pool (12,416 MiB in 2 MiB pages; the system gives the lowest room that fits,
// the heap's memory comes from the top), the graphics allocator (blocks of 64 MiB, a block of its
// own above 32 MiB, one empty block kept, a new block asked for at 64, 32 and 16 MiB), Eden's
// staging pool, its texture collector (its marks for a 6 GiB budget), the game's memory (2 MiB
// pages from the bottom, with the 1 GiB reserve its service holds) and its writes in bursts. The
// mspace under the heap is a best-fit stand-in that keeps its records outside the memory, so a
// heap of gigabytes costs the host almost nothing.
//
// Usage: soak MODE [TRACE.csv]
//   MODE fixed     what this tree does (the default)
//   MODE control   v1.010.006's graphics rules: whole transfers, the pool's free block alone for
//                  the caches, no memory given back on a refusal, no trim but on a shortage
// Without a trace it runs a synthetic three hours of Tears of the Kingdom (TOTK): 18,881 pipelines
// built while the game starts, open-world streaming, the item menu's texture array of 121 layers
// every ten minutes, and bursts of 1.4 GB of the game's own writes. With a trace (the EDEN_MEMORY
// lines of a log as CSV: t_s,guest,heap,large,tables,graphics,free,largest_free,reserve,heap_used)
// the game's memory, the heap's use and the graphics working set follow it.
// Prints a line every simulated minute and a summary; exits 1 when an allocation failed or the
// largest free block fell under what the largest request needs.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "memory_budget.h"
#include "staging_chunks.h"

namespace {
constexpr std::uint64_t MiB = std::uint64_t{1} << 20, GiB = std::uint64_t{1} << 30, PAGE = 2 * MiB;

// ---- the pool ----
enum Kind : std::uint8_t { FREE = 0, GUEST, HEAP, GRAPHICS, TABLES, JIT, DRIVER, RESERVE };
std::mutex pool_mutex;
struct Pool {
    std::vector<std::uint8_t> page;
    explicit Pool(std::uint64_t bytes) : page(bytes / PAGE, FREE) {}
    std::int64_t Take(std::uint64_t bytes, bool high, Kind kind) {
        const std::int64_t n = static_cast<std::int64_t>((bytes + PAGE - 1) / PAGE), count = std::int64_t(page.size());
        if (n <= 0 || n > count) return -1;
        if (high) {
            std::int64_t run = 0;
            for (std::int64_t i = count - 1; i >= 0; --i) {
                run = page[std::size_t(i)] == FREE ? run + 1 : 0;
                if (run == n) {
                    std::fill(page.begin() + i, page.begin() + i + n, kind);
                    return i;
                }
            }
        } else {
            std::int64_t run = 0;
            for (std::int64_t i = 0; i < count; ++i) {
                run = page[std::size_t(i)] == FREE ? run + 1 : 0;
                if (run == n) {
                    std::fill(page.begin() + i - n + 1, page.begin() + i + 1, kind);
                    return i - n + 1;
                }
            }
        }
        return -1;
    }
    // The lowest room that fits at or above `floor` (src/direct_memory.c eden_direct_top_room).
    std::int64_t TakeAbove(std::int64_t floor, std::uint64_t bytes, Kind kind) {
        const std::int64_t n = static_cast<std::int64_t>((bytes + PAGE - 1) / PAGE), count = std::int64_t(page.size());
        std::int64_t run = 0;
        for (std::int64_t i = std::max<std::int64_t>(floor, 0); i < count; ++i) {
            run = page[std::size_t(i)] == FREE ? run + 1 : 0;
            if (run == n) {
                std::fill(page.begin() + i - n + 1, page.begin() + i + 1, kind);
                return i - n + 1;
            }
        }
        return -1;
    }
    void Give(std::int64_t first, std::uint64_t bytes) {
        const std::int64_t n = static_cast<std::int64_t>((bytes + PAGE - 1) / PAGE);
        for (std::int64_t i = first; i < first + n; ++i) {
            if (page[std::size_t(i)] == FREE) { std::fprintf(stderr, "pool: page %lld given twice\n", (long long)i); std::abort(); }
            page[std::size_t(i)] = FREE;
        }
    }
    std::uint64_t Free() const { return std::uint64_t(std::count(page.begin(), page.end(), FREE)) * PAGE; }
    std::uint64_t Held(Kind kind) const { return std::uint64_t(std::count(page.begin(), page.end(), kind)) * PAGE; }
    std::uint64_t Largest() const {
        std::uint64_t best = 0, run = 0;
        for (std::uint8_t p : page) {
            run = p == FREE ? run + 1 : 0;
            best = std::max(best, run);
        }
        return best * PAGE;
    }
    unsigned Runs() const {
        unsigned runs = 0;
        for (std::size_t i = 0; i < page.size(); ++i) runs += page[i] == FREE && (i == 0 || page[i - 1] != FREE);
        return runs;
    }
};
Pool pool(Eden::Performance::kPoolBytes);

// ---- the heap's platform: its range, committed from the pool's top ----
char* range_base = nullptr;
std::size_t range_bytes = 0;
std::vector<std::int64_t> range_page; // pool page behind each 2 MiB of the range, -1: none
struct Own { std::vector<std::pair<std::int64_t, std::uint64_t>> runs; std::size_t bytes; };
std::unordered_map<std::uintptr_t, Own> own_blocks; // large blocks with memory of their own
std::atomic<unsigned> commit_refusals{0};
std::int64_t top_floor = -1; // the lowest page memory that stays has taken (src/direct_memory.c)
} // namespace

extern "C" {
void* eden_heap_reserve(std::size_t size) {
    // 8 MiB aligned, wherever the host maps it: the same arena slots on every host.
    constexpr std::uintptr_t align = 8 * MiB;
    char* at = static_cast<char*>(mmap(nullptr, size + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (at == MAP_FAILED) return nullptr;
    range_base = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(at) + align - 1) & ~(align - 1));
    range_bytes = size;
    range_page.assign(size / PAGE, -1);
    return range_base;
}
int eden_heap_commit(void* address, std::size_t size) {
    const std::size_t first = std::size_t(static_cast<char*>(address) - range_base) / PAGE, pages = size / PAGE;
    std::lock_guard lock{pool_mutex};
    std::vector<std::pair<std::size_t, std::int64_t>> taken;
    // The room the top has again first, in blocks as large as it has (src/memory_pages.cpp), then
    // as high as there is room.
    bool top_room = top_floor >= 0;
    for (std::size_t done = 0, block = pages; done < pages;) {
        block = std::min(block, pages - done);
        const std::int64_t at = top_room ? pool.TakeAbove(top_floor, block * PAGE, HEAP) : pool.Take(block * PAGE, true, HEAP);
        if (at >= 0 && !top_room && (top_floor < 0 || at < top_floor)) top_floor = at;
        if (at < 0 && top_room) {
            if (block == 1) top_room = false, block = pages - done;
            else block = std::max<std::size_t>(1, block / 2);
            continue;
        }
        if (at < 0) {
            if (block == 1) {
                for (auto [index, page] : taken) pool.Give(page, PAGE), range_page[index] = -1;
                ++commit_refusals;
                return -1;
            }
            block = std::max<std::size_t>(1, block / 2);
            continue;
        }
        for (std::size_t k = 0; k < block; ++k) {
            range_page[first + done + k] = at + std::int64_t(k);
            taken.push_back({first + done + k, at + std::int64_t(k)});
        }
        done += block;
    }
    if (mprotect(address, size, PROT_READ | PROT_WRITE) != 0) std::abort();
    return 0;
}
int eden_heap_decommit(void* address, std::size_t size) {
    const std::size_t first = std::size_t(static_cast<char*>(address) - range_base) / PAGE, pages = size / PAGE;
    {
        std::lock_guard lock{pool_mutex};
        for (std::size_t k = first; k < first + pages; ++k)
            if (range_page[k] >= 0) pool.Give(range_page[k], PAGE), range_page[k] = -1;
    }
    if (mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != address) return 0;
    return 1;
}
static void* OwnPages(std::size_t size, bool split) {
    const std::uint64_t bytes = (size + PAGE - 1) / PAGE * PAGE;
    Own own{{}, size};
    {
        std::lock_guard lock{pool_mutex};
        if (!split) {
            const std::int64_t at = pool.Take(bytes, false, HEAP);
            if (at < 0) return nullptr;
            own.runs.push_back({at, bytes});
        } else {
            for (std::uint64_t done = 0; done < bytes; done += PAGE) {
                const std::int64_t at = pool.Take(PAGE, false, HEAP);
                if (at < 0) {
                    for (auto [first, length] : own.runs) pool.Give(first, length);
                    return nullptr;
                }
                own.runs.push_back({at, PAGE});
            }
        }
    }
    void* at = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    std::lock_guard lock{pool_mutex};
    own_blocks[reinterpret_cast<std::uintptr_t>(at)] = std::move(own);
    return at;
}
static void OwnPagesFree(void* address) {
    std::lock_guard lock{pool_mutex};
    auto it = own_blocks.find(reinterpret_cast<std::uintptr_t>(address));
    if (it == own_blocks.end()) std::abort();
    for (auto [first, length] : it->second.runs) pool.Give(first, length);
    munmap(address, (it->second.bytes + PAGE - 1) / PAGE * PAGE);
    own_blocks.erase(it);
}
void* eden_heap_pages(std::size_t size) { return OwnPages(size, false); }
void eden_heap_pages_free(void* address, std::size_t) { OwnPagesFree(address); }
void* eden_heap_split_pages(std::size_t size) { return OwnPages(size, true); }
void eden_heap_split_pages_free(void* address, std::size_t) { OwnPagesFree(address); }

// ---- the mspace: best fit, records kept outside the memory ----
namespace {
std::mutex space_mutex;
struct Space {
    std::uintptr_t start, end;
    std::map<std::uintptr_t, std::uint64_t> free_at;
    std::set<std::pair<std::uint64_t, std::uintptr_t>> free_size;
    void Insert(std::uintptr_t at, std::uint64_t size) {
        if (size == 0) return;
        auto next = free_at.find(at + size);
        if (next != free_at.end()) {
            free_size.erase({next->second, next->first});
            size += next->second;
            free_at.erase(next);
        }
        auto after = free_at.lower_bound(at);
        if (after != free_at.begin()) {
            auto before = std::prev(after);
            if (before->first + before->second == at) {
                free_size.erase({before->second, before->first});
                before->second += size;
                free_size.insert({before->second, before->first});
                return;
            }
        }
        free_at[at] = size;
        free_size.insert({size, at});
    }
    void Erase(std::uintptr_t at, std::uint64_t size) {
        free_at.erase(at);
        free_size.erase({size, at});
    }
};
std::map<std::uintptr_t, Space*> spaces;                              // by start
std::unordered_map<std::uintptr_t, std::pair<Space*, std::uint64_t>> used; // block: its space, size
std::uint64_t Round(std::uint64_t size) { return std::max<std::uint64_t>(32, (size + 15) & ~std::uint64_t{15}); }
void* Take(Space* space, std::uint64_t size, std::uint64_t alignment) {
    const std::uint64_t n = Round(size), want = alignment > 16 ? n + alignment + 32 : n;
    auto it = space->free_size.lower_bound({want, 0});
    if (it == space->free_size.end()) return nullptr;
    const std::uintptr_t chunk = it->second;
    const std::uint64_t length = it->first;
    space->Erase(chunk, length);
    std::uintptr_t at = chunk;
    if (alignment > 16) {
        at = (chunk + alignment - 1) & ~(alignment - 1);
        if (at != chunk && at - chunk < 32) at += alignment;
        space->Insert(chunk, at - chunk); // the lead goes back, as dlmalloc's memalign does
    }
    const std::uint64_t rest = chunk + length - (at + n);
    if (rest >= 32) space->Insert(at + n, rest);
    used[at] = {space, rest >= 32 ? n : n + rest};
    return reinterpret_cast<void*>(at);
}
} // namespace

void* sceLibcMspaceCreate(const char*, void* base, std::size_t size, unsigned) {
    std::lock_guard lock{space_mutex};
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    // A space over memory given back and taken again replaces the old one.
    for (auto it = spaces.lower_bound(start); it != spaces.end() && it->first < start + size;) {
        delete it->second;
        it = spaces.erase(it);
    }
    auto* space = new Space{start, start + size, {}, {}};
    space->Insert(start + 64, size - 64);
    spaces[start] = space;
    return space;
}
void* sceLibcMspaceMalloc(void* space, std::size_t size) {
    std::lock_guard lock{space_mutex};
    return Take(static_cast<Space*>(space), size, 16);
}
void* sceLibcMspaceCalloc(void* space, std::size_t count, std::size_t size) {
    std::lock_guard lock{space_mutex};
    return Take(static_cast<Space*>(space), count * size, 16);
}
int sceLibcMspacePosixMemalign(void* space, void** address, std::size_t alignment, std::size_t size) {
    std::lock_guard lock{space_mutex};
    *address = Take(static_cast<Space*>(space), size, alignment);
    return *address ? 0 : 12;
}
void sceLibcMspaceFree(void* space, void* address) {
    std::lock_guard lock{space_mutex};
    auto it = used.find(reinterpret_cast<std::uintptr_t>(address));
    if (it == used.end() || it->second.first != space) {
        std::fprintf(stderr, "mspace: %p freed to a space that does not hold it\n", address);
        std::abort();
    }
    Space* owner = it->second.first;
    const std::uint64_t size = it->second.second;
    used.erase(it);
    owner->Insert(reinterpret_cast<std::uintptr_t>(address), size);
}
std::size_t sceLibcMspaceMallocUsableSize(const void* address) {
    std::lock_guard lock{space_mutex};
    auto it = used.find(reinterpret_cast<std::uintptr_t>(address));
    if (it == used.end()) { std::fprintf(stderr, "mspace: %p is no block\n", address); std::abort(); }
    return it->second.second;
}
void* sceLibcMspaceRealloc(void* space, void* address, std::size_t size) {
    std::lock_guard lock{space_mutex};
    auto it = used.find(reinterpret_cast<std::uintptr_t>(address));
    if (it == used.end()) std::abort();
    if (Round(size) <= it->second.second) return address;
    Space* owner = static_cast<Space*>(space);
    void* moved = Take(owner, size, 16);
    if (moved == nullptr) return nullptr;
    const std::uint64_t old = it->second.second;
    used.erase(reinterpret_cast<std::uintptr_t>(address));
    owner->Insert(reinterpret_cast<std::uintptr_t>(address), old);
    return moved;
}
int sceKernelUsleep(unsigned int microseconds) { return usleep(microseconds); }
int sceKernelDebugOutText(int, const char*) { return 0; }
void* __real_malloc(std::size_t size) { return std::malloc(size); }
void* __real_calloc(std::size_t count, std::size_t size) { return std::calloc(count, size); }
void* __real_realloc(void* address, std::size_t size) { return std::realloc(address, size); }
void __real_free(void* address) { std::free(address); }
int __real_posix_memalign(void** address, std::size_t alignment, std::size_t size) {
    return posix_memalign(address, alignment, size);
}
std::size_t __real_malloc_usable_size(const void*) { return 0; }

void* __wrap_malloc(std::size_t);
void __wrap_free(void*);
std::size_t eden_heap_trim(void);
std::size_t eden_heap_trim_level(int) __attribute__((weak));
std::size_t eden_heap_committed(void);
void eden_heap_usage(std::size_t*, std::size_t*, std::size_t*);
std::size_t eden_heap_large_held(unsigned*);
std::size_t eden_heap_hole_usage(unsigned*) __attribute__((weak));
std::size_t eden_heap_arena_usage(unsigned*) __attribute__((weak));
} // extern "C"

namespace {
bool fixed_mode = true;
unsigned failures = 0;
std::string first_failure;
double now_s = 0;
void Fail(const std::string& what) {
    if (failures++ == 0) first_failure = what;
    if (failures <= 5) std::printf("FAIL at %.0f s: %s\n", now_s, what.c_str());
}
std::size_t TrimHeap(bool holes) {
    if (eden_heap_trim_level) return eden_heap_trim_level(holes ? 1 : 0);
    return eden_heap_trim();
}

// ---- the graphics allocator ----
struct Block {
    std::int64_t first;      // pool page
    std::uint64_t size, used = 0;
    std::map<std::uint64_t, std::uint64_t> free; // offset -> size
    unsigned allocations = 0;
    bool host;
};
struct Allocation {
    int block = -1;          // -1: memory of its own
    std::int64_t first = -1; // its pool page then
    std::uint64_t offset = 0, size = 0;
};
constexpr std::uint64_t kBlock = 64 * MiB; // integrated GPU: Eden's preferredLargeHeapBlockSize
std::vector<Block> blocks;
std::vector<int> free_slots;
std::uint64_t dedicated_bytes = 0, used_bytes = 0;
std::uint64_t Held() {
    std::uint64_t held = dedicated_bytes;
    for (const Block& block : blocks) held += block.first >= 0 ? block.size : 0;
    return held;
}
bool FitIn(Block& block, std::uint64_t size, Allocation* out, int index) {
    std::uint64_t best_at = 0, best_size = ~std::uint64_t{0};
    for (auto [at, length] : block.free) {
        const std::uint64_t aligned = (at + 4095) & ~std::uint64_t{4095};
        if (aligned + size <= at + length && length < best_size) best_at = at, best_size = length;
    }
    if (best_size == ~std::uint64_t{0}) return false;
    const std::uint64_t aligned = (best_at + 4095) & ~std::uint64_t{4095};
    block.free.erase(best_at);
    if (aligned > best_at) block.free[best_at] = aligned - best_at;
    if (best_at + best_size > aligned + size) block.free[aligned + size] = best_at + best_size - aligned - size;
    block.used += size;
    ++block.allocations;
    *out = Allocation{index, -1, aligned, size};
    return true;
}
bool Allocate(std::uint64_t size, bool host, Allocation* out) {
    std::lock_guard lock{pool_mutex};
    if (size > kBlock / 2) {
        const std::int64_t at = pool.Take(size, false, GRAPHICS);
        if (at < 0) return false;
        dedicated_bytes += (size + PAGE - 1) / PAGE * PAGE;
        used_bytes += size;
        *out = Allocation{-1, at, 0, size};
        return true;
    }
    // The fullest blocks first, as the allocator sorts them.
    std::vector<int> order;
    for (int i = 0; i < int(blocks.size()); ++i)
        if (blocks[std::size_t(i)].first >= 0 && blocks[std::size_t(i)].host == host) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [](int a, int b) {
        const std::uint64_t free_a = blocks[std::size_t(a)].size - blocks[std::size_t(a)].used;
        const std::uint64_t free_b = blocks[std::size_t(b)].size - blocks[std::size_t(b)].used;
        return free_a != free_b ? free_a < free_b : a < b;
    });
    for (int i : order)
        if (FitIn(blocks[std::size_t(i)], size, out, i)) {
            used_bytes += size;
            return true;
        }
    for (std::uint64_t try_size = kBlock; try_size >= size && try_size >= 16 * MiB; try_size /= 2) {
        const std::int64_t at = pool.Take(try_size, false, GRAPHICS);
        if (at < 0) continue;
        int index;
        if (!free_slots.empty()) index = free_slots.back(), free_slots.pop_back();
        else index = int(blocks.size()), blocks.emplace_back();
        blocks[std::size_t(index)] = Block{at, try_size, 0, {{0, try_size}}, 0, host};
        if (!FitIn(blocks[std::size_t(index)], size, out, index)) std::abort();
        used_bytes += size;
        return true;
    }
    return false;
}
void Release(const Allocation& allocation) {
    std::lock_guard lock{pool_mutex};
    used_bytes -= allocation.size;
    if (allocation.block < 0) {
        pool.Give(allocation.first, allocation.size);
        dedicated_bytes -= (allocation.size + PAGE - 1) / PAGE * PAGE;
        return;
    }
    Block& block = blocks[std::size_t(allocation.block)];
    block.used -= allocation.size;
    --block.allocations;
    block.free[allocation.offset] = allocation.size;
    // Join the neighbours.
    for (auto it = block.free.begin(); it != block.free.end();) {
        auto next = std::next(it);
        if (next != block.free.end() && it->first + it->second == next->first) {
            it->second += next->second;
            block.free.erase(next);
        } else {
            it = next;
        }
    }
    if (block.allocations == 0) {
        // One empty block is kept; a second goes back.
        bool another = false;
        for (std::size_t i = 0; i < blocks.size(); ++i)
            if (int(i) != allocation.block && blocks[i].first >= 0 && blocks[i].host == block.host && blocks[i].allocations == 0)
                another = true;
        if (another) {
            pool.Give(block.first, block.size);
            block.first = -1;
            free_slots.push_back(allocation.block);
        }
    }
}

// ---- Eden's staging pool: buffers kept for the next transfer, the idle ones let go ----
struct Kept { Allocation memory; std::uint64_t holds; double idle_since; bool busy; };
std::vector<Kept> kept;
unsigned staging_refused = 0;
std::uint64_t largest_staging = 0;
std::uint64_t StagingSize(std::uint64_t bytes) {
    if (bytes > 16 * MiB) return (bytes + 16 * MiB - 1) / (16 * MiB) * (16 * MiB);
    std::uint64_t size = 1;
    while (size < bytes) size <<= 1;
    return size;
}
std::uint64_t ReleaseIdle(double older_than) {
    std::uint64_t bytes = 0;
    for (auto it = kept.begin(); it != kept.end();) {
        if (!it->busy && now_s - it->idle_since >= older_than) {
            bytes += it->holds;
            Release(it->memory);
            it = kept.erase(it);
        } else {
            ++it;
        }
    }
    return bytes;
}
void Relieve(bool urgent);
// A staging buffer for `bytes`: a kept idle one that holds it, else a new one; refused, the idle
// ones go and it is asked for again (the v1.010.002 rule); in this tree, then the pool's relief.
bool Stage(std::uint64_t bytes) {
    if (bytes <= 16 * MiB) return true; // the stream buffer
    for (Kept& buffer : kept)
        if (!buffer.busy && buffer.holds >= bytes && buffer.holds <= StagingSize(bytes) * 2) {
            buffer.busy = true;
            return true;
        }
    const std::uint64_t size = StagingSize(bytes);
    largest_staging = std::max(largest_staging, size);
    Allocation memory;
    if (!Allocate(size, true, &memory)) {
        ReleaseIdle(0);
        if (!Allocate(size, true, &memory)) {
            if (fixed_mode) Relieve(true);
            if (!fixed_mode || !Allocate(size, true, &memory)) {
                ++staging_refused;
                return false;
            }
        }
    }
    kept.push_back(Kept{memory, size, now_s, true});
    return true;
}
void StagingFrameDone() {
    for (Kept& buffer : kept)
        if (buffer.busy) buffer.busy = false, buffer.idle_since = now_s;
}
struct StagingRef { std::span<std::uint8_t> mapped_span; };

// ---- the game's memory and its reserve ----
std::vector<std::int64_t> guest_pages, reserve_pages;
std::mt19937_64 rng(std::getenv("SOAK_SEED") ? std::strtoull(std::getenv("SOAK_SEED"), nullptr, 10) : 20261030);
// The same numbers on every host: std::mt19937_64 is specified, its distributions are not.
double Unit(std::mt19937_64& random) { return double(random() >> 11) * (1.0 / 9007199254740992.0); }
std::size_t Pick(std::mt19937_64& random, std::size_t n) { return n == 0 ? 0 : std::size_t(random() % n); }
unsigned guest_waits = 0;
void Gc(bool hardest);
bool GuestPage() {
    std::int64_t at;
    {
        std::lock_guard lock{pool_mutex};
        at = pool.Take(PAGE, false, GUEST);
    }
    if (at < 0 && !reserve_pages.empty()) {
        std::lock_guard lock{pool_mutex};
        at = reserve_pages.back();
        reserve_pages.pop_back();
        pool.page[std::size_t(at)] = GUEST;
    }
    if (at < 0) {
        // The write waits while the caches give memory back at their hardest.
        ++guest_waits;
        Relieve(true);
        Gc(true);
        std::lock_guard lock{pool_mutex};
        at = pool.Take(PAGE, false, GUEST);
    }
    if (at < 0) return false;
    guest_pages.push_back(at);
    return true;
}
void GuestTo(std::uint64_t bytes) {
    const std::size_t want = bytes / PAGE;
    while (guest_pages.size() < want)
        if (!GuestPage()) {
            Fail("the game's write was refused: no 2 MiB block left (" + std::to_string(guest_pages.size() * 2) + " MiB held)");
            return;
        }
    while (guest_pages.size() > want) {
        const std::size_t k = Pick(rng, guest_pages.size());
        std::lock_guard lock{pool_mutex};
        pool.Give(guest_pages[k], PAGE);
        guest_pages[k] = guest_pages.back();
        guest_pages.pop_back();
    }
}
void FillReserve() {
    std::lock_guard lock{pool_mutex};
    while (reserve_pages.size() < Eden::Performance::kGuestReserve / PAGE) {
        const std::int64_t at = pool.Take(PAGE, false, RESERVE);
        if (at < 0) break;
        reserve_pages.push_back(at);
    }
}

// ---- textures ----
struct Texture {
    std::uint64_t size;
    bool dirty, costly;
    bool resident = false;
    Allocation memory;
    double last_use = -1e9, evicted_at = -1e9;
};
std::vector<Texture> textures;
constexpr unsigned kRegions = 400, kPerRegion = 100, kCommon = 150;
std::size_t menu_array = 0; // the item menu's texture array of 121 layers
std::uint64_t reloads = 0, expensive_evictions = 0, evictions = 0, created = 0;
unsigned gc_steps = 0, gc_hardest = 0, gc_dirty = 0; // collector runs, at its hardest, evicting dirty textures
unsigned image_refused = 0;
std::uint64_t largest_image = 0;
std::uint64_t largest_request = 0;
bool PoolShort(Eden::Performance::PoolView* out) {
    std::uint64_t largest;
    {
        std::lock_guard lock{pool_mutex};
        largest = pool.Largest();
    }
    const auto view = Eden::Performance::ViewOfPool(true, largest, reserve_pages.size() * PAGE, Eden::Performance::kGuestReserve);
    if (out) *out = view;
    return view.short_memory;
}
// performance.cpp RelieveMemory: the idle staging buffers and the heap's empty pieces and arenas;
// when memory is wanted at once, the pages of the heap's free runs too.
void Relieve(bool urgent) {
    ReleaseIdle(0);
    TrimHeap(urgent);
}
// Eden's collector with its marks for a 6 GiB budget: the caches' "memory in use" is the budget
// less what they see as free (CachesFree in this tree, the pool's free block in v1.010.006).
void Gc(bool hardest) {
    Eden::Performance::PoolView view;
    const bool pool_short = PoolShort(&view);
    std::uint64_t free_view = view.free;
    if (fixed_mode) {
        std::lock_guard lock{pool_mutex};
        free_view = Eden::Performance::CachesFree(view.free, Held(), used_bytes, Eden::Performance::CachesFloor(largest_image));
    }
    const std::uint64_t budget = 6 * GiB;
    const std::uint64_t usage = budget - std::min(budget, free_view);
    const bool caches_short = Eden::Performance::CachesShort(free_view);
    const bool keep_dirty = !pool_short || (fixed_mode && !caches_short);
    const std::uint64_t minimum = 1 * GiB, expected = std::uint64_t(4.4 * double(GiB)), critical = std::uint64_t(4.8 * double(GiB));
    if (!hardest && usage < minimum) return;
    const bool aggressive = hardest || usage >= critical, high = aggressive || usage >= expected;
    if (!hardest) {
        ++gc_steps;
        gc_hardest += aggressive;
        gc_dirty += !keep_dirty;
    }
    const double unused_for = aggressive ? 0.35 : high ? 0.85 : 1.7; // 10, 25, 50 frames at 30 fps
    for (Texture& texture : textures) {
        if (!texture.resident || now_s - texture.last_use < unused_for + 5.0 * (texture.last_use >= now_s - 0.1))
            continue;
        if (texture.costly && !aggressive) continue;
        if (texture.dirty && keep_dirty) continue;
        Release(texture.memory);
        texture.resident = false;
        texture.evicted_at = now_s;
        ++evictions;
        if (texture.dirty || texture.costly) ++expensive_evictions;
    }
}
bool CreateTexture(Texture& texture) {
    Allocation memory;
    largest_image = std::max(largest_image, texture.size);
    if (!Allocate(texture.size, false, &memory)) {
        // Refused inside and outside the budget: in this tree, what nothing uses goes back and it is
        // asked for once more.
        if (fixed_mode) Relieve(true);
        if (!fixed_mode || !Allocate(texture.size, false, &memory)) {
            ++image_refused;
            Fail("an image of " + std::to_string(texture.size >> 20) + " MiB was refused");
            return false;
        }
    }
    texture.memory = memory;
    texture.resident = true;
    ++created;
    if (now_s - texture.evicted_at < 60) ++reloads;
    // Its upload: whole in v1.010.006, in pieces of at most 64 MiB here.
    bool staged = true;
    if (fixed_mode && Eden::StagingChunks::Chunked(texture.size)) {
        struct Copy {
            std::size_t buffer_offset, buffer_size;
            unsigned buffer_row_length, buffer_image_height;
            struct { int base_level, base_layer, num_layers; } image_subresource;
            struct { int x, y, z; } image_offset;
            struct { unsigned width, height, depth; } image_extent;
        };
        std::vector<Copy> copies;
        const unsigned layers = texture.size > 100 * MiB ? 121 : 1;
        std::size_t offset = 0;
        for (unsigned level = 0, extent = 1024; level < 11 && offset < texture.size; ++level, extent = std::max(1u, extent / 2)) {
            std::size_t bytes = level == 0 ? std::size_t(texture.size * 3 / 4) : (texture.size - offset) / 2 + 16;
            bytes = std::min<std::size_t>(bytes, texture.size - offset) / layers * layers;
            if (bytes == 0) break;
            copies.push_back(Copy{offset, bytes, extent, extent, {int(level), 0, int(layers)}, {0, 0, 0}, {extent, extent, 1}});
            offset += bytes;
        }
        static std::vector<std::uint8_t> nothing(1);
        try {
            Eden::StagingChunks::StageImage<Copy>(
                std::span<const std::uint8_t>(), copies.data(), copies.size(),
                [&](std::uint64_t bytes) {
                    largest_request = std::max(largest_request, StagingSize(bytes));
                    if (!Stage(bytes)) throw std::runtime_error("refused");
                    return StagingRef{std::span<std::uint8_t>(nothing.data(), nothing.size())};
                },
                [&](StagingRef&, std::span<const Copy>) {});
        } catch (const std::runtime_error&) {
            staged = false;
        }
    } else {
        largest_request = std::max(largest_request, StagingSize(texture.size));
        staged = Stage(texture.size);
    }
    if (!staged) Fail("a staging buffer of " + std::to_string(StagingSize(texture.size) >> 20) + " MiB for a transfer of " +
                      std::to_string(texture.size >> 20) + " MiB was refused");
    return staged;
}
Texture MakeTexture(std::uint64_t size, bool dirty, bool costly) {
    Texture texture{};
    texture.size = size;
    texture.dirty = dirty;
    texture.costly = costly;
    return texture;
}
void BuildCatalog() {
    const auto size = [&] {
        const double r = Unit(rng);
        const double mib = r < 0.70 ? 0.25 + Unit(rng) * 1.75 : r < 0.95 ? 2 + Unit(rng) * 6 : r < 0.99 ? 8 + Unit(rng) * 8 : 32;
        return std::uint64_t(mib * double(MiB)) / 4096 * 4096;
    };
    for (unsigned i = 0; i < kRegions * kPerRegion + kCommon; ++i) {
        const std::uint64_t bytes = size();
        const bool dirty = Unit(rng) < 0.3, costly = Unit(rng) < 0.5;
        textures.push_back(MakeTexture(bytes, dirty, costly));
    }
    menu_array = textures.size();
    // 1024x1024, 121 layers, 11 levels, BC3: 161 MiB (ASTC kept as BC3, as TOTK's profiles do).
    textures.push_back(MakeTexture(std::uint64_t(161.3 * double(MiB)) / 4096 * 4096, false, true));
}
void Use(std::size_t index) {
    Texture& texture = textures[index];
    if (!texture.resident && !CreateTexture(texture)) return;
    texture.last_use = now_s;
}

// ---- the heap's users ----
// The shader cache's builders: 15 threads, each pipeline keeping ~95 KiB in six blocks and
// freeing what it used along the way. They run one after another (each a thread of its own, so
// each has its arenas), so every run lays the heap out the same.
std::vector<std::vector<void*>> built(15);
void Build(unsigned pipelines) {
    for (unsigned t = 0; t < 15; ++t) {
        std::thread thread([t, pipelines] {
            std::mt19937_64 random(t * 7919 + 1);
            for (unsigned p = t; p < pipelines; p += 15) {
                void* scratch[4];
                for (void*& block : scratch) block = __wrap_malloc(4096 + Pick(random, 120000 - 4096 + 1));
                for (int k = 0; k < 6; ++k) {
                    void* block = __wrap_malloc(1000 + Pick(random, 30000 - 1000 + 1));
                    if (block == nullptr) return;
                    built[t].push_back(block);
                }
                for (void* block : scratch) __wrap_free(block);
            }
        });
        thread.join();
    }
}
// The game's own use of the heap: blocks of every size, freed in any order.
std::vector<std::pair<void*, std::size_t>> game_blocks;
std::size_t game_bytes = 0;
void GameHeapTo(std::uint64_t target) {
    unsigned guard = 0;
    while (game_bytes < target && guard++ < 200000) {
        const double r = Unit(rng);
        const std::size_t size = r < 0.70 ? 64 + std::size_t(Unit(rng) * 32768)
                                : r < 0.995 ? 262144 + std::size_t(Unit(rng) * 4 * double(MiB))
                                            : std::size_t(32 * MiB + Unit(rng) * 64 * double(MiB));
        void* block = __wrap_malloc(size);
        if (block == nullptr) {
            Fail("the heap refused " + std::to_string(size) + " bytes");
            return;
        }
        game_blocks.push_back({block, size});
        game_bytes += size;
    }
    while (game_bytes > target + 8 * MiB && !game_blocks.empty()) {
        const std::size_t k = Pick(rng, game_blocks.size());
        __wrap_free(game_blocks[k].first);
        game_bytes -= game_blocks[k].second;
        game_blocks[k] = game_blocks.back();
        game_blocks.pop_back();
    }
}
// Some churn: a share of the game's blocks freed and taken again at other sizes.
void GameHeapChurn(double share) {
    const std::size_t n = std::size_t(double(game_blocks.size()) * share);
    const std::uint64_t target = game_bytes;
    for (std::size_t i = 0; i < n && !game_blocks.empty(); ++i) {
        const std::size_t k = Pick(rng, game_blocks.size());
        __wrap_free(game_blocks[k].first);
        game_bytes -= game_blocks[k].second;
        game_blocks[k] = game_blocks.back();
        game_blocks.pop_back();
    }
    GameHeapTo(target);
}

// ---- traces ----
struct Row { double t; double guest, heap, large, graphics, free, largest, heap_used; };
std::vector<Row> ReadTrace(const char* path) {
    std::vector<Row> rows;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line) && rows.size() < 100000) {
        if (line.size() > 4096) continue;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::vector<std::string> cells;
        std::stringstream cut(line);
        for (std::string cell; std::getline(cut, cell, ',');) cells.push_back(cell);
        if (!line.empty() && line.back() == ',') cells.emplace_back(); // the empty last field
        if (cells.size() < 10) continue;
        const auto number = [&](std::size_t k) {
            char* end = nullptr;
            const double value = cells[k].empty() ? -1 : std::strtod(cells[k].c_str(), &end);
            return std::isfinite(value) && value >= 0 && value < 1e6 ? value : -1.0;
        };
        Row row{number(0), number(1), number(2), number(3), number(5), number(6), number(7), number(9)};
        if (row.t < 0 || row.guest < 0) continue;
        rows.push_back(row);
    }
    return rows;
}
} // namespace

int main(int argc, char** argv) {
    fixed_mode = !(argc > 1 && std::strcmp(argv[1], "control") == 0);
    const std::vector<Row> trace = argc > 2 ? ReadTrace(argv[2]) : std::vector<Row>{};
    if (argc > 2 && trace.empty()) {
        std::fprintf(stderr, "no rows in %s\n", argv[2]);
        return 2;
    }
    // Three hours by default; SOAK_HOURS for longer (with six, this tree failed with one seed of
    // three, at 3 h 20 min: the pool fragments under the game's bursts and the textures' churn),
    // SOAK_SEED for another session.
    const char* hours = std::getenv("SOAK_HOURS");
    const double synthetic_hours = hours ? std::clamp(std::atof(hours), 0.1, 24.0) : 3.0;
    const double end_s = trace.empty() ? synthetic_hours * 3600.0 : trace.back().t;
    std::printf("%s, %s\n", fixed_mode ? "this tree" : "control (v1.010.006's graphics rules)",
                trace.empty() ? "synthetic 3 hours of TOTK" : argv[2]);
    // What stays: the driver's blocks and the threads' stacks (the stream buffer of 256 MiB among
    // them), the page tables, the JIT's code; the guest's memory before the game runs.
    {
        std::lock_guard lock{pool_mutex};
        pool.Take(Eden::Performance::kDriverBudget, true, DRIVER);
        pool.Take(150 * MiB, false, TABLES);
        pool.Take(192 * MiB, false, JIT);
    }
    Allocation stream;
    if (!Allocate(256 * MiB, true, &stream)) std::abort();
    FillReserve();
    BuildCatalog();
    GuestTo(std::uint64_t((trace.empty() ? 1290 : trace.front().guest) * double(MiB)));
    // The game is loaded (640 MB of the heap), then its 18,881 cached pipelines are built while it
    // starts, and the heap gives back what that left free.
    GameHeapTo(trace.empty() || trace.front().heap_used < 0 ? 640 * MiB : 640 * MiB);
    Build(18881);
    TrimHeap(true);
    std::size_t held = 0, in_use = 0, returned = 0;
    eden_heap_usage(&held, &in_use, &returned);
    const std::size_t start_held = eden_heap_committed() + eden_heap_large_held(nullptr);
    std::printf("start: the heap holds %zu MiB with %zu MiB in use (v1.010.004: 2560 MiB; v1.010.006: 4480 MiB)\n",
                start_held >> 20, in_use >> 20);
    std::uint64_t worst_largest = ~std::uint64_t{0}, worst_free = ~std::uint64_t{0};
    double worst_at = 0;
    std::uint64_t peak_heap = 0, peak_graphics = 0;
    unsigned region = 0;
    std::size_t row = 0;
    for (now_s = 5; now_s <= end_s + 0.1 && failures == 0; now_s += 5) {
        // The game's memory and the heap's use: the trace's, or bursts of the game's writes and slow growth.
        std::uint64_t guest_target, heap_target;
        double graphics_mib = 0;
        if (!trace.empty()) {
            while (row + 1 < trace.size() && trace[row + 1].t <= now_s) ++row;
            guest_target = std::uint64_t(trace[row].guest * double(MiB));
            const double used = trace[row].heap_used >= 0 ? trace[row].heap_used : trace[row].heap * 0.6;
            heap_target = std::uint64_t(std::max(0.0, used - 2400.0 + 640.0) * double(MiB));
            graphics_mib = trace[row].graphics;
        } else {
            const double minutes = now_s / 60;
            // 1.3 GB at start, 2.3 GB after an hour and a half (TOTK: 2.7 to 2.9 GB at most), and every
            // 20 minutes a burst of 1.2 GB written within 5 s, given back over a minute.
            double guest = 1290 + 1010 * std::min(1.0, minutes / 90);
            const double burst = std::fmod(now_s, 1200.0);
            if (burst < 60) guest += 1200 * std::max(0.0, 1.0 - burst / 60) * (burst >= 5 ? 1.0 : burst / 5);
            guest_target = std::uint64_t(std::min(guest, 4096.0) * double(MiB));
            heap_target = std::uint64_t((640 + 450 * std::min(1.0, minutes / 120)) * double(MiB));
        }
        GuestTo(guest_target);
        GameHeapTo(heap_target);
        GameHeapChurn(0.01);
        // Streaming: a region of the world every 20 s, six around the player; the menu's array.
        if (std::fmod(now_s, 20.0) < 1) region = (region + 1 + unsigned(Unit(rng) * 2)) % kRegions;
        const unsigned near = trace.empty() ? 4 : unsigned(std::clamp(graphics_mib * 0.6 / 280.0, 2.0, 10.0));
        for (unsigned r = 0; r < near && failures == 0; ++r)
            for (unsigned k = 0; k < kPerRegion && failures == 0; ++k) Use(((region + r) % kRegions) * kPerRegion + k);
        for (unsigned k = 0; k < kCommon && failures == 0; ++k) Use(kRegions * kPerRegion + k);
        if (std::fmod(now_s, 600.0) < 30) Use(menu_array);
        StagingFrameDone();
        ReleaseIdle(3);
        // The pool short: what nothing uses goes back (every 2 s; in v1.010.006 the heap's empty
        // pieces alone), and the caches evict.
        if (PoolShort(nullptr)) {
            if (fixed_mode) Relieve(false);
            else TrimHeap(false);
        }
        // Every 30 s the empty pieces and arenas (performance.cpp ReportMemory); the pages of free
        // runs only while the pool is short.
        if (fixed_mode && std::fmod(now_s, 30.0) < 1) TrimHeap(false);
        Gc(false);
        FillReserve();
        std::uint64_t largest, free_bytes;
        unsigned runs;
        {
            std::lock_guard lock{pool_mutex};
            largest = pool.Largest();
            free_bytes = pool.Free();
            runs = pool.Runs();
        }
        const std::size_t heap_held = eden_heap_committed() + eden_heap_large_held(nullptr);
        peak_heap = std::max<std::uint64_t>(peak_heap, heap_held);
        peak_graphics = std::max<std::uint64_t>(peak_graphics, Held());
        if (largest < worst_largest) worst_largest = largest, worst_at = now_s;
        worst_free = std::min(worst_free, free_bytes);
        // The largest request this tree makes: an image (the menu's array has memory of its own),
        // a staging buffer, a block of the allocator.
        const std::uint64_t need = std::max({largest_image, fixed_mode ? Eden::StagingChunks::kLimit : largest_request, kBlock});
        if (largest < need) Fail("the largest free block (" + std::to_string(largest >> 20) + " MiB) is under the largest request (" +
                                 std::to_string(need >> 20) + " MiB)");
        if (std::fmod(now_s, 600.0) < 1 || failures != 0) {
            eden_heap_usage(&held, &in_use, &returned);
            unsigned arenas = 0, holes = 0;
            const std::size_t arena_bytes = eden_heap_arena_usage ? eden_heap_arena_usage(&arenas) : 0;
            const std::size_t hole_bytes = eden_heap_hole_usage ? eden_heap_hole_usage(&holes) : 0;
            std::printf("%5.0f s guest=%zuMiB heap=%zuMiB/%zuMiB (arenas %zuMiB/%u, large %zuMiB, holes %zuMiB, game %zuMiB in %zu) "
                        "graphics=%lluMiB/%lluMiB free=%lluMiB largest=%lluMiB runs=%u "
                        "evictions=%llu expensive=%llu reloads=%llu\n",
                        now_s, guest_pages.size() * 2, heap_held >> 20, in_use >> 20, arena_bytes >> 20, arenas,
                        eden_heap_large_held(nullptr) >> 20, hole_bytes >> 20, game_bytes >> 20, game_blocks.size(),
                        (unsigned long long)(Held() >> 20),
                        (unsigned long long)(used_bytes >> 20), (unsigned long long)(free_bytes >> 20),
                        (unsigned long long)(largest >> 20), runs, (unsigned long long)evictions,
                        (unsigned long long)expensive_evictions, (unsigned long long)reloads);
        }
    }
    eden_heap_usage(&held, &in_use, &returned);
    const double minutes = std::max(1.0, (now_s - 5) / 60);
    std::printf("SUMMARY mode=%s seconds=%.0f failures=%u heap_start=%zuMiB heap_peak=%lluMiB heap_returned=%zuMiB "
                "graphics_peak=%lluMiB worst_largest_free=%lluMiB(at %.0f s) worst_free=%lluMiB largest_request=%lluMiB "
                "largest_image=%lluMiB staging_refused=%u images_refused=%u guest_waits=%u evictions_per_min=%.0f "
                "expensive_evictions_per_min=%.0f reloads_per_min=%.0f collector_hardest=%.0f%% "
                "collector_dirty=%.0f%%%s%s\n",
                fixed_mode ? "fixed" : "control", now_s - 5, failures, start_held >> 20,
                (unsigned long long)(peak_heap >> 20), returned >> 20, (unsigned long long)(peak_graphics >> 20),
                (unsigned long long)(worst_largest >> 20), worst_at, (unsigned long long)(worst_free >> 20),
                (unsigned long long)(largest_request >> 20), (unsigned long long)(largest_image >> 20), staging_refused,
                image_refused, guest_waits, double(evictions) / minutes, double(expensive_evictions) / minutes,
                double(reloads) / minutes, 100.0 * gc_hardest / std::max(1u, gc_steps),
                100.0 * gc_dirty / std::max(1u, gc_steps), failures ? " first_failure=" : "",
                failures ? first_failure.c_str() : "");
    return failures == 0 ? 0 : 1;
}
