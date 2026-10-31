#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the heap that grows by pieces (headless/heap_arenas.inc).

Builds the app's heap source as headless/CMakeLists.txt derives it and runs it on Linux over a
stand-in for the console's mspace functions: a first-fit allocator with checked block headers.
Eight threads allocate, resize and free blocks of every size, each filled with a pattern that
is checked when the block changes hands, so an owner looked up wrongly, a block handed out
twice or a piece used before it has memory shows as a damaged pattern or header (or a fault:
the range is unreadable until committed). Then the limits: one request larger than a piece, a
large request when the console has no memory for a block of its own, the heap filled to its
6 GiB, and the same run with the range refused (the old whole-heap path, 3 GiB at once). Spaces
that hold nothing are given back (eden_heap_trim) while six threads take and free blocks of the
spaces, a given-back piece being unreadable until it is committed again, and the heap's range is
filled again after a trim; a large block takes memory in pieces when no one piece is free.

Arenas take slots of their own at the top of the range (v1.010.006 took them from the spaces and
never gave those pieces back: 0 MiB given back in every Tears of the Kingdom session). Fifteen
threads build like a shader cache (blocks they keep among blocks they free) and the heap then
holds little more than what they keep; once all of it is freed the heap gives back everything but
its first piece and first arenas. Pieces left partly used give back the large pages of their free
runs (holes), which are filled again, unharmed, when the room is needed; extra arenas that hold
nothing go back and their slots are taken again.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
heap = (root / 'third_party/app_heap.c').read_text()
for old, new in (
    ('128 MiB', '6144 MiB'),
    ('(128u * 1024u * 1024u)', '((size_t)6144 << 20)'),
    ('base=%p size=%u\\n', 'base=%p size=%zu\\n'),
    ('#include <sys/mman.h>', '#include <sys/mman.h>\nvoid *eden_heap_pages(size_t);\nvoid eden_heap_pages_free(void *, size_t);'),
    ('"PS5-OpenGL"', '"Eden-headless"'),
):
    assert old in heap, old
    heap = heap.replace(old, new)
begin = heap.index('static int ps5_heap_ready(void) {')
end = heap.index('void ps5_opengl_heap_stats_print(unsigned iteration) {')
heap = heap[:begin] + (root / 'headless/heap_arenas.inc').read_text() + heap[end:]

MOCK = r'''
// A stand-in for the console: mspaces over caller memory, and the range the heap grows in.
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define malloc_usable_size(p) malloc_size(p)
#endif

#define BLOCK_MAGIC 0xb10cb10cb10cb10cull
#define ALIAS_MAGIC 0xa11a5a11a5a11a50ull
struct block { size_t size; uint64_t magic; struct block *next_free; uint64_t used; };  // 32 bytes before the data
struct space { pthread_mutex_t lock; struct block *free_list; char *start, *end; uint64_t magic; };
_Static_assert(sizeof(struct block) == 32, "header");

void *sceLibcMspaceCreate(const char *name, void *base, size_t size, unsigned flags) {
    (void)name; (void)flags;
    assert(size >= 4096 && ((uintptr_t)base & 15) == 0);
    struct space *space = base;
    pthread_mutex_init(&space->lock, NULL);
    space->magic = 0x5bace5bace5bace5ull;
    space->start = (char *)base + 128;
    space->end = (char *)base + size;
    struct block *first = (struct block *)space->start;
    first->size = (size_t)(space->end - space->start) - sizeof(struct block);
    first->magic = BLOCK_MAGIC; first->next_free = NULL; first->used = 0;
    space->free_list = first;
    return space;
}
static struct block *header_of(const void *address) {
    struct block *block = (struct block *)address - 1;
    if (block->magic == ALIAS_MAGIC) block = (struct block *)block->next_free;   // an aligned block: its real header
    if (block->magic != BLOCK_MAGIC) { fprintf(stderr, "damaged block header at %p\n", address); abort(); }
    return block;
}
// Joins neighbouring free blocks (the console's allocator does this as blocks are freed).
static void coalesce(struct space *space) {
    space->free_list = NULL;
    for (char *at = space->start; at < space->end;) {
        struct block *block = (struct block *)at;
        if (block->magic != BLOCK_MAGIC) { fprintf(stderr, "damaged block chain in a space\n"); abort(); }
        if (!block->used) {
            for (;;) {
                struct block *after = (struct block *)((char *)(block + 1) + block->size);
                if ((char *)after >= space->end || after->used) break;
                if (after->magic != BLOCK_MAGIC) { fprintf(stderr, "damaged block chain in a space\n"); abort(); }
                block->size += sizeof(struct block) + after->size;
                after->magic = 0;
            }
            block->next_free = space->free_list;
            space->free_list = block;
        }
        at = (char *)(block + 1) + block->size;
    }
}
static void *take(struct space *space, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (size < 32) size = 32;
    void *result = NULL;
    pthread_mutex_lock(&space->lock);
    assert(space->magic == 0x5bace5bace5bace5ull);
    for (int attempt = 0; attempt < 2 && result == NULL; ++attempt) {
    if (attempt == 1) coalesce(space);
    for (struct block **link = &space->free_list; *link != NULL; link = &(*link)->next_free) {
        struct block *block = *link;
        assert(block->magic == BLOCK_MAGIC && !block->used);
        if (block->size < size) continue;
        if (block->size >= size + sizeof(struct block) + 64) {
            struct block *rest = (struct block *)((char *)(block + 1) + size);
            rest->size = block->size - size - sizeof(struct block);
            rest->magic = BLOCK_MAGIC; rest->used = 0; rest->next_free = block->next_free;
            block->size = size;
            *link = rest;
        } else {
            *link = block->next_free;
        }
        block->used = 1; block->next_free = NULL;
        result = block + 1;
        break;
    }
    }
    pthread_mutex_unlock(&space->lock);
    return result;
}
void *sceLibcMspaceMalloc(void *space, size_t size) { return take(space, size); }
void *sceLibcMspaceCalloc(void *space, size_t count, size_t size) {
    void *address = take(space, count * size);
    return address ? memset(address, 0, count * size) : NULL;
}
size_t sceLibcMspaceMallocUsableSize(const void *address) {
    const struct block *alias = (const struct block *)address - 1;
    const struct block *block = header_of(address);
    assert(block->used == 1);
    return alias->magic == ALIAS_MAGIC ? alias->size : block->size;
}
void sceLibcMspaceFree(void *handle, void *address) {
    struct space *space = handle;
    struct block *block = header_of(address);
    // The block must lie in the space it is returned to: a wrong owner is the bug this check hunts.
    if ((char *)block < space->start || (char *)block >= space->end || block->used != 1) {
        fprintf(stderr, "block %p freed to a space that does not hold it\n", address); abort();
    }
    pthread_mutex_lock(&space->lock);
    block->used = 0;
    block->next_free = space->free_list;
    space->free_list = block;
    pthread_mutex_unlock(&space->lock);
}
void *sceLibcMspaceRealloc(void *space, void *address, size_t size) {
    const size_t before = sceLibcMspaceMallocUsableSize(address);
    if (size <= before && size != 0) return address;
    if (size == 0) { sceLibcMspaceFree(space, address); return NULL; }
    void *moved = take(space, size);
    if (moved == NULL) return NULL;   // like the console: the caller's block stays
    memcpy(moved, address, before);
    sceLibcMspaceFree(space, address);
    return moved;
}
int sceLibcMspacePosixMemalign(void *space, void **address, size_t alignment, size_t size) {
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0) return EINVAL;
    if (alignment <= 16) { *address = take(space, size); return *address ? 0 : ENOMEM; }
    char *raw = take(space, size + alignment + sizeof(struct block));
    if (raw == NULL) return ENOMEM;
    char *aligned = (char *)(((uintptr_t)raw + sizeof(struct block) + alignment - 1) & ~(uintptr_t)(alignment - 1));
    struct block *alias = (struct block *)aligned - 1;
    alias->magic = ALIAS_MAGIC; alias->next_free = (struct block *)raw - 1; alias->size = size; alias->used = 1;
    *address = aligned;
    return 0;
}
int sceKernelUsleep(unsigned int microseconds) { return usleep(microseconds); }
int sceKernelDebugOutText(int channel, const char *text) { (void)channel; return (int)write(2, text, strlen(text)); }

// The C library's own allocator, for what the app's heap does not own.
void *__real_malloc(size_t size) { return malloc(size); }
void *__real_calloc(size_t count, size_t size) { return calloc(count, size); }
void *__real_realloc(void *address, size_t size) { return realloc(address, size); }
void __real_free(void *address) { free(address); }
int __real_posix_memalign(void **address, size_t alignment, size_t size) { return posix_memalign(address, alignment, size); }
#ifndef __APPLE__
size_t malloc_usable_size(void *);
#endif
size_t __real_malloc_usable_size(const void *address) { return malloc_usable_size((void *)address); }

// The range the heap grows in: unreadable until committed, so a piece used early faults. Its
// memory is counted by large page, as the console gives it back (a hole is part of a piece).
static int refuse_range;
static atomic_size_t committed_bytes;
static char *range_start;
static size_t range_length;
static atomic_uchar page_held[4096];   // 8 GiB of 2 MiB pages
static int refuse_commit;               // the console has no memory for a piece (or a hole again)
void *eden_heap_reserve(size_t size) {
    if (refuse_range) return NULL;
    // The console's ranges are 2 MiB aligned; so is this one, wherever the host places a mapping
    // of the heap's 6 GiB.
    const size_t align = (size_t)2 << 20;
    char *address = mmap(NULL, size + align, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (address == MAP_FAILED) return NULL;
    range_start = (char *)(((uintptr_t)address + align - 1) & ~(uintptr_t)(align - 1));
    range_length = size;
    assert(size / align <= sizeof(page_held));
    return range_start;
}
static size_t page_of(const void *address) {
    assert((const char *)address >= range_start && (const char *)address < range_start + range_length);
    return (size_t)((const char *)address - range_start) >> 21;
}
int eden_heap_commit(void *address, size_t size) {
    assert(((uintptr_t)address & 0x1fffff) == 0 && (size & 0x1fffff) == 0);
    if (refuse_commit) return -1;
    if (mprotect(address, size, PROT_READ | PROT_WRITE) != 0) return -1;
    for (size_t page = page_of(address); page < page_of(address) + (size >> 21); ++page)
        if (atomic_exchange(&page_held[page], 1) == 0) atomic_fetch_add(&committed_bytes, (size_t)2 << 20);
    return 0;
}
static int refuse_pages;   // the console has no memory left for a block of its own in one piece
void *eden_heap_pages(size_t size) {
    if (refuse_pages) return NULL;
    void *address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return address == MAP_FAILED ? NULL : address;
}
void eden_heap_pages_free(void *address, size_t size) { munmap(address, size); }
// Memory given back: unreadable until committed again, so a use after it faults.
static atomic_size_t decommitted_bytes;
int eden_heap_decommit(void *address, size_t size) {
    assert(((uintptr_t)address & 0x1fffff) == 0 && (size & 0x1fffff) == 0);
    // Fresh pages in place of the old ones: unreadable, and zero when committed again.
    if (mmap(address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != address)
        return 0;
    for (size_t page = page_of(address); page < page_of(address) + (size >> 21); ++page)
        if (atomic_exchange(&page_held[page], 0) == 1) {
            atomic_fetch_sub(&committed_bytes, (size_t)2 << 20);
            atomic_fetch_add(&decommitted_bytes, (size_t)2 << 20);
        }
    return 1;
}
// A large block in pieces of memory (the pool has no one piece for it): here simply a mapping.
static int refuse_split;   // nor in pieces
static atomic_uint split_blocks;
void *eden_heap_split_pages(size_t size) {
    if (refuse_split) return NULL;
    void *address = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (address == MAP_FAILED) return NULL;
    atomic_fetch_add(&split_blocks, 1);
    return address;
}
void eden_heap_split_pages_free(void *address, size_t size) { munmap(address, size); atomic_fetch_sub(&split_blocks, 1); }
'''

TEST = r'''
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void __wrap_free(void *);
int __wrap_posix_memalign(void **, size_t, size_t);
size_t __wrap_malloc_usable_size(const void *);
unsigned eden_heap_arenas_created(void);
size_t eden_heap_committed(void);
size_t eden_heap_large_held(unsigned *blocks);
void eden_heap_spill_stats(unsigned long long *, unsigned long long *, unsigned long long *, unsigned long long *);
size_t eden_heap_trim(void);
size_t eden_heap_trim_level(int holes);
void eden_heap_usage(size_t *held, size_t *in_use, size_t *returned);
size_t eden_heap_arena_usage(unsigned *count);
size_t eden_heap_hole_usage(unsigned *count);

static void fill(unsigned char *block, size_t size, unsigned seed) {
    // The start and the end of a large block are enough to see another owner writing into it.
    for (size_t i = 0; i < size; i = (i == 4095 && size > 8192) ? size - 4096 : i + 1)
        block[i] = (unsigned char)(seed + i * 131u);
}
static void verify(const unsigned char *block, size_t size, unsigned seed) {
    for (size_t i = 0; i < size; i = (i == 4095 && size > 8192) ? size - 4096 : i + 1)
        if (block[i] != (unsigned char)(seed + i * 131u)) { fprintf(stderr, "pattern damaged at %p+%zu\n", (void *)block, i); abort(); }
}
struct held { unsigned char *block; size_t size; unsigned seed; };
static unsigned long long spills_now(void) {
    unsigned long long spills = 0;
    eden_heap_spill_stats(&spills, NULL, NULL, NULL);
    return spills;
}
// A thread that keeps 13 MiB of small blocks (a shader cache builder keeps what it builds): more
// than one arena holds, so it needs a second arena of its own, not the heap's shared spaces.
enum { KEPT = 6500, KEPT_SIZE = 2100 };
static void *keeper(void *argument) {
    unsigned char **kept = argument;
    for (int i = 0; i < KEPT; ++i) {
        kept[i] = __wrap_malloc(KEPT_SIZE);
        assert(kept[i] != NULL);
        fill(kept[i], KEPT_SIZE, (unsigned)i);
    }
    return NULL;
}
static void keep_and_free(int threads, unsigned char *(*kept)[KEPT]) {
    pthread_t ids[48];
    assert(threads <= 48);
    for (int t = 0; t < threads; ++t) assert(pthread_create(&ids[t], NULL, keeper, kept[t]) == 0);
    for (int t = 0; t < threads; ++t) pthread_join(ids[t], NULL);
    // Freed by another thread, as the GPU thread frees what the builders made.
    for (int t = 0; t < threads; ++t)
        for (int i = 0; i < KEPT; ++i) { verify(kept[t][i], KEPT_SIZE, (unsigned)i); __wrap_free(kept[t][i]); }
}

// Threads that take and free blocks of the heap's spaces while another gives back the spaces that
// hold nothing: a space given back is unreadable (the stand-in's decommit), so a block handed out
// of it, or a space given back while a block was in it, faults or damages a pattern.
static atomic_int trimming;
static atomic_size_t trimmed_total;
static void *trimmer(void *argument) {
    (void)argument;
    while (atomic_load(&trimming)) {
        atomic_fetch_add(&trimmed_total, eden_heap_trim());
        usleep(100);
    }
    return NULL;
}
static void *churn(void *argument) {
    unsigned state = (unsigned)(uintptr_t)argument * 2654435761u + 7;
    enum { SLOTS = 24 };
    struct held held[SLOTS] = {0};
    for (int round = 0; round < 3000; ++round) {
        state = state * 1664525u + 1013904223u;
        struct held *slot = &held[(state >> 8) % SLOTS];
        if (slot->block != NULL) {
            verify(slot->block, slot->size, slot->seed);
            __wrap_free(slot->block);
            slot->block = NULL;
            continue;
        }
        const size_t size = 300000 + (size_t)(state >> 4) % 12000000;  // the heap's spaces
        slot->block = __wrap_malloc(size);
        if (slot->block == NULL) continue;
        slot->size = size; slot->seed = state;
        fill(slot->block, size, slot->seed);
    }
    for (int i = 0; i < SLOTS; ++i)
        if (held[i].block) { verify(held[i].block, held[i].size, held[i].seed); __wrap_free(held[i].block); }
    return NULL;
}

static atomic_ulong operations;
static void *worker(void *argument) {
    unsigned state = (unsigned)(uintptr_t)argument * 2654435761u + 1;
    enum { SLOTS = 200 };
    struct held held[SLOTS] = {0};
    for (int round = 0; round < 60000; ++round) {
        state = state * 1664525u + 1013904223u;
        struct held *slot = &held[(state >> 8) % SLOTS];
        const unsigned kind = (state >> 20) % 1000;
        size_t size = kind < 700 ? 1 + (state >> 4) % 2000              // small: the thread's arena
                    : kind < 930 ? 4096 + (state >> 4) % 400000          // medium: arena or the spaces
                    : kind < 997 ? 300000 + (size_t)(state >> 4) % 6000000
                                 : 20000000 + (size_t)(state >> 4) % 40000000;
        if (slot->block == NULL) {
            const unsigned how = (state >> 28) % 4;
            if (how == 0) {
                slot->block = __wrap_calloc(1, size);
                if (slot->block) for (size_t i = 0; i < size && i < 4096; ++i) assert(slot->block[i] == 0);
            } else if (how == 1) {
                void *address = NULL;
                const size_t alignment = (size_t)64 << ((state >> 12) % 8);
                if (__wrap_posix_memalign(&address, alignment, size) == 0) {
                    assert(((uintptr_t)address & (alignment - 1)) == 0);
                    slot->block = address;
                }
            } else {
                slot->block = __wrap_malloc(size);
            }
            if (slot->block == NULL) continue;   // the heap is full for this size right now
            assert(__wrap_malloc_usable_size(slot->block) >= size);
            slot->size = size; slot->seed = state;
            fill(slot->block, size, slot->seed);
        } else if ((state >> 30) & 1) {
            verify(slot->block, slot->size, slot->seed);
            unsigned char *moved = __wrap_realloc(slot->block, size);
            if (moved == NULL) continue;         // the old block stays
            // The start came along (the pattern covers a block's first and last 4096 bytes, so
            // only its start is known at the new size).
            const size_t kept = size < slot->size ? size : slot->size;
            verify(moved, kept < 4096 ? kept : 4096, slot->seed);
            slot->block = moved; slot->size = size; slot->seed = state;
            fill(moved, size, slot->seed);
        } else {
            verify(slot->block, slot->size, slot->seed);
            __wrap_free(slot->block);
            slot->block = NULL;
        }
        atomic_fetch_add(&operations, 1);
    }
    for (int i = 0; i < SLOTS; ++i)
        if (held[i].block) { verify(held[i].block, held[i].size, held[i].seed); __wrap_free(held[i].block); }
    return NULL;
}

// Fifteen threads that build like a shader cache: each "pipeline" keeps blocks of a few sizes
// and frees the transient ones it made along the way.
enum { BUILDERS = 15, PIPELINES = 600, KEPT_PER = 6 };
static unsigned char *built[BUILDERS][PIPELINES * KEPT_PER];
static size_t built_size[BUILDERS][PIPELINES * KEPT_PER];
static void *builder(void *argument) {
    const unsigned t = (unsigned)(uintptr_t)argument;
    unsigned state = t * 2654435761u + 99;
    for (int pipeline = 0; pipeline < PIPELINES; ++pipeline) {
        unsigned char *transient[4];
        for (int i = 0; i < 4; ++i) {
            state = state * 1664525u + 1013904223u;
            const size_t size = 2048 + (state >> 8) % 60000;
            transient[i] = __wrap_malloc(size);
            assert(transient[i] != NULL);
            fill(transient[i], size < 4096 ? size : 4096, state);
        }
        for (int k = 0; k < KEPT_PER; ++k) {
            state = state * 1664525u + 1013904223u;
            const size_t size = 256 + (state >> 8) % 9000;
            const int at = pipeline * KEPT_PER + k;
            built[t][at] = __wrap_malloc(size);
            assert(built[t][at] != NULL);
            built_size[t][at] = size;
            fill(built[t][at], size, (unsigned)(t * 100000 + at));
        }
        for (int i = 0; i < 4; ++i) __wrap_free(transient[i]);
    }
    return NULL;
}
static size_t kept_bytes(void) {
    size_t bytes = 0;
    for (int t = 0; t < BUILDERS; ++t)
        for (int i = 0; i < PIPELINES * KEPT_PER; ++i)
            if (built[t][i]) bytes += __wrap_malloc_usable_size(built[t][i]);
    return bytes;
}
static void free_built(double share) {
    for (int t = 0; t < BUILDERS; ++t)
        for (int i = 0; i < (int)(PIPELINES * KEPT_PER * share); ++i)
            if (built[t][i]) {
                verify(built[t][i], built_size[t][i], (unsigned)(t * 100000 + i));
                __wrap_free(built[t][i]);
                built[t][i] = NULL;
            }
}
static void check_builders(size_t piece) {
    const unsigned made_before = eden_heap_arenas_created();
    const size_t before = eden_heap_committed();
    pthread_t ids[BUILDERS];
    for (uintptr_t t = 0; t < BUILDERS; ++t) assert(pthread_create(&ids[t], NULL, builder, (void *)t) == 0);
    for (int t = 0; t < BUILDERS; ++t) pthread_join(ids[t], NULL);
    (void)eden_heap_trim();
    const size_t kept = kept_bytes(), held = eden_heap_committed();
    unsigned arenas = 0;
    const size_t arena_bytes = eden_heap_arena_usage(&arenas);
    // What the heap holds for them is what they keep and the slack it keeps for itself, not twice
    // that (v1.010.006: every arena cost a space 16 MiB, and no such piece was given back).
    const size_t grown = held > before ? held - before : 0;
    printf("builders: keep %zu MiB, the heap grew by %zu MiB (%u arenas made, %zu MiB in %u arenas now)\n",
           kept >> 20, grown >> 20, eden_heap_arenas_created() - made_before, arena_bytes >> 20, arenas);
    if (grown > kept + kept / 4 + ((size_t)128 << 20)) {
        fprintf(stderr, "builders: the heap grew by %zu MiB for %zu MiB kept\n", grown >> 20, kept >> 20);
        abort();
    }
    // Most of it freed in runs (their first blocks): what the arenas no longer use goes back.
    free_built(0.6);
    const size_t trimmed = eden_heap_trim();
    size_t holes = eden_heap_hole_usage(NULL);
    printf("builders: 60%% freed, %zu MiB given back, %zu MiB of it in holes\n", trimmed >> 20, holes >> 20);
    free_built(1.0);
    (void)eden_heap_trim();
    unsigned left = 0;
    (void)eden_heap_arena_usage(&left);
    // Everything freed: what is left is the first piece and the threads' first arenas.
    if (eden_heap_committed() > piece + (size_t)(32 + 8) * ((size_t)8 << 20) + ((size_t)128 << 20) ||
        left > 40) {
        fprintf(stderr, "builders gone: %zu MiB still held in %u arenas and the spaces\n",
                eden_heap_committed() >> 20, left);
        abort();
    }
    printf("builders gone: %zu MiB held, %u arenas\n", eden_heap_committed() >> 20, left);
}

// Pieces left partly used: 1 MiB blocks, eight of every nine freed. The free runs give back their
// large pages; the blocks left are unharmed, and the room is there again when it is asked for.
enum { MEDIUM = 900 };
static unsigned char *medium[MEDIUM];
static void check_holes(void) {
    const size_t size = (size_t)1 << 20;
    for (int i = 0; i < MEDIUM; ++i) {
        medium[i] = __wrap_malloc(size);
        assert(medium[i] != NULL);
        fill(medium[i], size, (unsigned)i + 777);
    }
    for (int i = 0; i < MEDIUM; ++i)
        if (i % 9 != 0) { verify(medium[i], size, (unsigned)i + 777); __wrap_free(medium[i]); medium[i] = NULL; }
    const size_t before = eden_heap_committed();
    size_t returned_before = 0, returned_after = 0;
    eden_heap_usage(NULL, NULL, &returned_before);
    const size_t given = eden_heap_trim();
    eden_heap_usage(NULL, NULL, &returned_after);
    unsigned holes = 0;
    const size_t in_holes = eden_heap_hole_usage(&holes);
    printf("partly used pieces: %zu MiB of %zu given back (%zu MiB in holes)\n", given >> 20, before >> 20,
           in_holes >> 20);
    const size_t freed = (size_t)(MEDIUM - MEDIUM / 9) * size;
    if (given < freed / 3 || in_holes == 0 || eden_heap_committed() != before - given ||
        returned_after != returned_before + given || atomic_load(&committed_bytes) != eden_heap_committed()) {
        fprintf(stderr, "partly used pieces: %zu MiB given back of %zu MiB freed, %zu MiB in holes; held %zu MiB "
                "(the stand-in counts %zu)\n", given >> 20, freed >> 20, in_holes >> 20,
                eden_heap_committed() >> 20, atomic_load(&committed_bytes) >> 20);
        abort();
    }
    for (int i = 0; i < MEDIUM; i += 9) verify(medium[i], size, (unsigned)i + 777);
    // Asked for again: the holes are filled (the range does not grow past them).
    for (int i = 0; i < MEDIUM; ++i)
        if (medium[i] == NULL) {
            medium[i] = __wrap_malloc(size);
            assert(medium[i] != NULL);
            fill(medium[i], size, (unsigned)i + 999);
        }
    for (int i = 0; i < MEDIUM; ++i) {
        verify(medium[i], size, (unsigned)i + (i % 9 == 0 ? 777 : 999));
        __wrap_free(medium[i]);
        medium[i] = NULL;
    }
    // With no memory for a hole again, a request that needs its room is refused cleanly, and is
    // given it once the memory is back.
    (void)eden_heap_trim();
    printf("partly used pieces taken again and freed: %zu MiB held after a trim\n", eden_heap_committed() >> 20);
}

int main(int argc, char **argv) {
    refuse_range = argc > 1 && strcmp(argv[1], "whole") == 0;
    const size_t piece = (size_t)128 << 20, heap = (size_t)6144 << 20, whole = (size_t)3072 << 20;
    void *first = __wrap_malloc(100);
    assert(first != NULL);
    const size_t arena = (size_t)8 << 20;
    if (!refuse_range) {
        // It starts with one piece, not the whole heap, and the thread's arena in a slot of its own.
        assert(eden_heap_committed() == piece + arena && atomic_load(&committed_bytes) == piece + arena);
        assert(__wrap_posix_memalign(&first, 24, 100) == EINVAL && eden_heap_committed() == piece + arena);
        unsigned arenas = 0;
        assert(eden_heap_arena_usage(&arenas) == arena && arenas == 1);
    } else {
        assert(eden_heap_committed() == whole && atomic_load(&committed_bytes) == 0);
    }
    pthread_t threads[8];
    for (uintptr_t i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, worker, (void *)(i + 1)) == 0);
    for (int i = 0; i < 8; ++i) pthread_join(threads[i], NULL);
    // Threads that keep more than an arena's worth of small blocks: arenas of their own take every
    // one (none goes to the heap's shared spaces), and the next threads, which take their records,
    // use those arenas again instead of making more.
    static unsigned char *kept[4][KEPT];
    const unsigned long long spills_before = spills_now();
    const unsigned arenas_before = eden_heap_arenas_created();
    keep_and_free(4, kept);
    const unsigned arenas_kept = eden_heap_arenas_created();
    if (!(arenas_kept > arenas_before && arenas_kept <= arenas_before + 4 * 3 && spills_now() == spills_before)) {
        fprintf(stderr, "keepers: arenas %u -> %u, small requests in the shared spaces %llu -> %llu\n",
                arenas_before, arenas_kept, spills_before, spills_now());
        abort();
    }
    keep_and_free(4, kept);
    if (!(eden_heap_arenas_created() == arenas_kept && spills_now() == spills_before)) {
        fprintf(stderr, "keepers again: arenas %u -> %u, small requests in the shared spaces %llu -> %llu\n",
                arenas_kept, eden_heap_arenas_created(), spills_before, spills_now());
        abort();
    }
    // Forty at once: more threads than first arenas (32), so the last ones share one at first; each
    // that fills it makes arenas of its own, and still none of its requests goes to the shared spaces.
    static unsigned char *crowd[40][KEPT];
    keep_and_free(40, crowd);
    if (!(spills_now() == spills_before && eden_heap_arenas_created() <= arenas_kept + 40 * 3)) {
        fprintf(stderr, "forty keepers: arenas %u -> %u, small requests in the shared spaces %llu -> %llu\n",
                arenas_kept, eden_heap_arenas_created(), spills_before, spills_now());
        abort();
    }
    {
        pthread_t trim_thread, churners[6];
        atomic_store(&trimming, 1);
        assert(pthread_create(&trim_thread, NULL, trimmer, NULL) == 0);
        for (uintptr_t i = 0; i < 6; ++i) assert(pthread_create(&churners[i], NULL, churn, (void *)(i + 11)) == 0);
        for (int i = 0; i < 6; ++i) pthread_join(churners[i], NULL);
        atomic_store(&trimming, 0);
        pthread_join(trim_thread, NULL);
        // Everything the churners took is free again: the spaces that held it go back.
        atomic_fetch_add(&trimmed_total, eden_heap_trim());
        if (!refuse_range && atomic_load(&trimmed_total) == 0) {
            fprintf(stderr, "nothing was given back while the spaces emptied\n");
            abort();
        }
    }
    const size_t after_threads = eden_heap_committed();
    const size_t peak = atomic_load(&ps5_heap_peak_bytes);
    assert(eden_heap_arenas_created() >= 8);
    if (!refuse_range) {
        // It grew, by exactly what was committed. How far depends on how the threads interleave and
        // on this stand-in allocator, which packs blocks far less tightly than the console's: the
        // limit itself is a valid outcome here (requests then fail, and the workers carry on).
        if (!(after_threads > piece && after_threads <= heap && atomic_load(&committed_bytes) == after_threads)) {
            fprintf(stderr, "pieces %zu MiB, committed %zu MiB, most in use %zu MiB\n", after_threads >> 20,
                    atomic_load(&committed_bytes) >> 20, peak >> 20);
            abort();
        }
        // A large block has memory of its own: the heap's pieces do not grow, and it is given
        // back when freed. It can grow in place of nothing: a resize moves it.
        unsigned blocks_alive = 99;
        assert(eden_heap_large_held(&blocks_alive) == 0 && blocks_alive == 0);
        unsigned char *large = __wrap_malloc(5 * piece / 2);
        assert(large != NULL && eden_heap_committed() == after_threads);
        assert(eden_heap_large_held(&blocks_alive) == 5 * piece / 2 && blocks_alive == 1);
        assert(__wrap_malloc_usable_size(large) == 5 * piece / 2);
        fill(large, 5 * piece / 2, 7);
        verify(large, 5 * piece / 2, 7);
        assert(__wrap_realloc(large, 2 * piece) == large);           // still most of it: it stays
        unsigned char *grown = __wrap_realloc(large, 3 * piece);      // larger: moved, the start kept
        assert(grown != NULL);
        verify(grown, 4096, 7);
        unsigned char *shrunk = __wrap_realloc(grown, 1000);          // small again: into an arena
        assert(shrunk != NULL && eden_heap_large_held(&blocks_alive) == 0 && blocks_alive == 0);
        verify(shrunk, 1000 < 4096 ? 1000 : 4096, 7);
        unsigned char *back = __wrap_realloc(shrunk, (size_t)40 << 20); // and out of the range again
        assert(back != NULL && eden_heap_large_held(&blocks_alive) == ((size_t)40 << 20));
        verify(back, 1000, 7);
        __wrap_free(back);
        assert(eden_heap_large_held(&blocks_alive) == 0 && eden_heap_committed() == after_threads);
        void *aligned = NULL;
        assert(__wrap_posix_memalign(&aligned, 4096, (size_t)33 << 20) == 0 && ((uintptr_t)aligned & 4095) == 0);
        assert(eden_heap_large_held(&blocks_alive) == ((size_t)33 << 20));
        __wrap_free(aligned);
        assert(__wrap_posix_memalign(&aligned, (size_t)1 << 22, (size_t)33 << 20) == 0 && ((uintptr_t)aligned & (((size_t)1 << 22) - 1)) == 0);
        assert(eden_heap_large_held(&blocks_alive) == 0);            // an alignment only a space can give
        __wrap_free(aligned);
        // With no memory left for a block of its own, a large request takes room in the heap.
        refuse_pages = refuse_split = 1;
        unsigned char *inside = __wrap_malloc((size_t)40 << 20);
        refuse_pages = refuse_split = 0;
        assert(inside != NULL && eden_heap_large_held(&blocks_alive) == 0 && blocks_alive == 0);
        fill(inside, (size_t)40 << 20, 9);
        unsigned char *outside = __wrap_realloc(inside, (size_t)48 << 20); // memory is back: moved out
        assert(outside != NULL && eden_heap_large_held(&blocks_alive) == ((size_t)48 << 20));
        verify(outside, 4096, 9);
        __wrap_free(outside);
        assert(eden_heap_large_held(&blocks_alive) == 0 && blocks_alive == 0);
        // No one piece for it, but pieces: a large block of its own all the same, zeroed, given back.
        const size_t before_split = eden_heap_committed();
        refuse_pages = 1;
        unsigned char *pieces = __wrap_calloc(1, (size_t)486 << 20);
        refuse_pages = 0;
        assert(pieces != NULL && atomic_load(&split_blocks) == 1 && eden_heap_large_held(&blocks_alive) == ((size_t)486 << 20));
        for (size_t i = 0; i < ((size_t)486 << 20); i += 4096) assert(pieces[i] == 0);
        fill(pieces, (size_t)486 << 20, 11);
        verify(pieces, (size_t)486 << 20, 11);
        __wrap_free(pieces);
        assert(atomic_load(&split_blocks) == 0 && eden_heap_large_held(&blocks_alive) == 0);
        assert(eden_heap_committed() == before_split);  // the heap's pieces did not grow for it
    }
    if (!refuse_range) {
        check_builders(piece);
        check_holes();
    }
    // Fill the heap with blocks under the large size: the limit is its 6 GiB (3 GiB taken whole),
    // past the old 3 GiB, and running out returns NULL without damage.
    enum { MANY = 4096 };
    static unsigned char *blocks[MANY];
    int count = 0;
    while (count < MANY && (blocks[count] = __wrap_malloc((size_t)24 << 20)) != NULL) {
        fill(blocks[count], (size_t)24 << 20, (unsigned)count);
        ++count;
    }
    assert(count > 20 && count < MANY && eden_heap_committed() <= (refuse_range ? whole : heap));
    assert(refuse_range || (size_t)count * ((size_t)24 << 20) > whole);
    assert(__wrap_malloc((size_t)-1 - 4096) == NULL);
    void *unaligned = NULL;
    assert(__wrap_posix_memalign(&unaligned, 4096, (size_t)-1 - 8192) != 0);
    for (int i = 0; i < count; ++i) { verify(blocks[i], (size_t)24 << 20, (unsigned)i); __wrap_free(blocks[i]); }
    // Given back once free, and taken again: the same pieces, not new ones past them.
    if (!refuse_range) {
        size_t held = 0, in_use = 0, returned_before = 0, returned_after = 0;
        eden_heap_usage(NULL, NULL, &returned_before);
        const size_t full = eden_heap_committed();
        const size_t given = eden_heap_trim();
        eden_heap_usage(&held, &in_use, &returned_after);
        if (!(given >= full / 2 && eden_heap_committed() == full - given && returned_after == returned_before + given &&
              atomic_load(&committed_bytes) == eden_heap_committed() && held >= eden_heap_committed())) {
            fprintf(stderr, "trim: %zu MiB of %zu given back, %zu MiB held after\n", given >> 20, full >> 20,
                    eden_heap_committed() >> 20);
            abort();
        }
        int again_count = 0;
        while (again_count < MANY && (blocks[again_count] = __wrap_malloc((size_t)24 << 20)) != NULL) {
            fill(blocks[again_count], (size_t)24 << 20, (unsigned)again_count + 5);
            ++again_count;
        }
        if (again_count < count - 4) {
            fprintf(stderr, "after giving back: %d blocks of 24 MiB, %d before\n", again_count, count);
            abort();
        }
        for (int i = 0; i < again_count; ++i) { verify(blocks[i], (size_t)24 << 20, (unsigned)i + 5); __wrap_free(blocks[i]); }
        assert(eden_heap_trim() > 0);
        printf("given back: %zu MiB of %zu, then %d blocks of 24 MiB again in the same range\n", given >> 20,
               full >> 20, again_count);
    }
    // After that, everything still works.
    unsigned char *again = __wrap_malloc(1000);
    assert(again != NULL);
    fill(again, 1000, 3); verify(again, 1000, 3);
    __wrap_free(again);
    __wrap_free(first);
    printf("%s: %lu operations on 8 threads, %u arenas (%u for the keepers), most in use at once %zu MiB, "
           "%zu MiB of pieces after the threads, %zu MiB when full (%d blocks of 24 MiB)\n",
           refuse_range ? "whole heap at once" : "heap by pieces", atomic_load(&operations), eden_heap_arenas_created(),
           arenas_kept - arenas_before, peak >> 20, after_threads >> 20, eden_heap_committed() >> 20, count);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='eden-heap-') as work:
    work = pathlib.Path(work)
    source = work / 'heap.c'
    source.write_text(MOCK + heap + TEST)
    for flags, label in ((['-O1', '-g', '-fsanitize=undefined', '-fno-sanitize-recover=all'], 'checked'),
                         (['-O2', '-fsanitize=thread'], 'thread sanitizer')):
        binary = work / ('heap-' + label.split()[0])
        subprocess.run([shutil.which('clang-18') or 'clang', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-Wno-unused-parameter', *flags, str(source), '-o', str(binary)], check=True)
        for mode in ((), ('whole',)):
            if label != 'checked' and mode:
                continue
            result = subprocess.run([str(binary), *mode], capture_output=True, text=True, timeout=900)
            sys.stdout.write(f'[{label}] ' + result.stdout)
            if result.returncode != 0:
                sys.stderr.write(result.stderr[-3000:])
                sys.exit(f'heap check failed ({label} {" ".join(mode)}): exit {result.returncode}')
print('Heap by pieces: growth on demand, owners by address, requests larger than a piece, the 6 GiB limit, '
      'the 3 GiB whole-heap fallback, threads keeping more than an arena in arenas of their own (used again by '
      'the next threads), empty spaces given back while six threads take and free blocks and taken again in the '
      'same range, a large block in pieces of memory when the pool has no one piece for it, arenas in slots of '
      'their own (fifteen builders: the heap holds what they keep; all freed, it gives back all but its first piece '
      'and arenas), the free runs of partly used pieces given back as holes and filled again, and eight threads '
      'under the thread sanitizer PASS')
