/* Eden PS5 port: a heap that takes its memory as it grows, with per-thread arenas.
 *
 * Memory. PS5 direct memory is real from the first byte, and the CPU shares one 12 GiB pool with
 * the GPU. The heap used to take its whole 3 GiB at start; the largest game measured used
 * 1.16 GiB of it, and the rest was graphics memory that game then ran out of. So the heap now
 * reserves its range (6 GiB, PS5_OPENGL_HEAP_SIZE) as address space only and commits it a piece
 * (128 MiB) at a time, from the start of the range, when an allocation finds no room. Where no
 * range can be reserved, it takes 3 GiB at once, as it did before it grew. Every piece (or run of pieces, for one
 * request larger than a piece) is a Sony mspace of its own; an address tells its piece, and
 * the piece its mspace. Memory is not given back: a piece stays for the next game.
 *
 * Large blocks. A request of 32 MiB or more gets memory of its own, outside the range, and gives
 * it back when it is freed: a game file read whole, or a buffer that doubles as it grows, would
 * otherwise leave the heap larger for good and its pieces full of holes no other large block fits.
 *
 * Arenas. One Sony mspace serialised every allocation in the process. Its lock collapses
 * under contention: a malloc/free pair costs 225 ns alone but 33/67/138 us with
 * 2/4/8 contending threads (EDEN_PERF_ALLOC),
 * which made allocation-heavy shader compilation ~100-250x slower than on a
 * Zen 2 host. A fixed split of the heap into arenas (run 220741) exhausted a
 * partition at level load, so capacity is not partitioned here: each allocating thread (up to
 * EDEN_HEAP_FIRST_ARENAS) gets a lazily created 8 MiB arena allocated *from* the heap, aligned to
 * its own 8 MiB slot. Small requests use the thread's arena and fall back to the heap's own
 * spaces; frees and reallocs find the owner in O(1) from the address.
 *
 * More than one arena a thread. A thread that keeps more than 8 MiB of small blocks filled its
 * arena, and from then on every small request it made went to the heap's own spaces, under the
 * lock that collapses. The threads that build a game's shader cache keep what they build (a
 * pipeline and the driver's copy of its shaders, tens of KiB each): fifteen builders fill their
 * arenas after a few thousand pipelines, which is where Tears of the Kingdom's build of 16,000
 * fell from about 200 to 60 a second at every start, and where more CPUs stopped helping (that
 * this is the cause is read from the logs, not yet measured: EDEN_SHADER_START_HEAP counts it). A
 * thread whose arena is full now
 * moves to another of its own arenas with room, and makes a new one when none has (up to
 * EDEN_HEAP_THREAD_ARENAS, for requests under EDEN_HEAP_ARENA_GROW_REQUEST); a thread that
 * shares an arena because the first ones were taken makes one of its own the same way. A thread's
 * first arena is still one of EDEN_HEAP_FIRST_ARENAS: the many threads that allocate little do
 * not each take 8 MiB of the heap. Its arenas stay with
 * its record, so the next thread that takes the record uses them: they are made once for the
 * most a thread ever held, not again for each session. The small requests that still reach the
 * heap's own spaces, and the frees of blocks there, are counted with the time they took
 * (eden_heap_spill_stats; EDEN_SHADER_START_HEAP in stderr.log).
 */
#include <pthread.h>
#include <string.h>

#define EDEN_HEAP_ARENA_SHIFT 23u /* 8 MiB arenas and slots */
#define EDEN_HEAP_ARENA_SIZE (1u << EDEN_HEAP_ARENA_SHIFT)
/* Absolute 8 MiB slots covering the heap (+1: the base need not be slot aligned). */
#define EDEN_HEAP_SLOTS ((PS5_OPENGL_HEAP_SIZE >> EDEN_HEAP_ARENA_SHIFT) + 1u)
/* Threads that get an arena of their own at their first small request; later ones share them. */
#define EDEN_HEAP_FIRST_ARENAS 32u
/* Every arena, with those made because a thread's were full: at most 2 GiB of the heap's 6 GiB. An
 * arena stays with its thread's record when its blocks are freed (the next thread on the record
 * uses it), so this is also the most the heap keeps for small blocks alone. */
#define EDEN_HEAP_MAX_ARENAS 256u
/* One thread's arenas: at most 256 MiB of small blocks before they go to the heap's spaces. */
#define EDEN_HEAP_THREAD_ARENAS 32u
/* A request this large or larger that finds no room in the thread's arenas makes no new one. */
#define EDEN_HEAP_ARENA_GROW_REQUEST (64u * 1024u)
/* After a thread found all its arenas full and could make no other, the requests it makes before
 * it looks through them again (they take room in the heap's spaces meanwhile). */
#define EDEN_HEAP_ARENA_RETRY 256u
/* Larger requests (and all over-aligned ones) always use the heap's own spaces. */
#define EDEN_HEAP_SMALL_REQUEST (256u * 1024u)
/* The heap grows by pieces of this size. */
#define EDEN_HEAP_PIECE_SHIFT 27u /* 128 MiB */
#define EDEN_HEAP_PIECE_SIZE ((size_t)1 << EDEN_HEAP_PIECE_SHIFT)
#define EDEN_HEAP_PIECES (PS5_OPENGL_HEAP_SIZE >> EDEN_HEAP_PIECE_SHIFT)

void *eden_heap_reserve(size_t size);             /* address space only; NULL when refused */
int eden_heap_commit(void *address, size_t size); /* zeroed memory for a part of it; 0 when done */

static void *_Atomic eden_heap_slots[EDEN_HEAP_SLOTS]; /* arena mspace owning each slot */
static void *eden_heap_arena_list[EDEN_HEAP_MAX_ARENAS];
static atomic_uint eden_heap_arena_count;
static unsigned eden_heap_first_arenas; /* first arenas made (under the creation lock) */
static atomic_uint eden_heap_next_shared;
static pthread_key_t eden_heap_arena_key;
static int eden_heap_arena_key_valid;
static atomic_flag eden_heap_arena_create_lock = ATOMIC_FLAG_INIT;
/* Who holds the creation lock and how many threads wait for it (read by the development stall
 * watchdog, headless/stall_watchdog.h). */
static _Atomic unsigned long eden_heap_create_holder;
static atomic_uint eden_heap_create_waiters;

/* The heap's own mspaces in the order they were made; each spans one or more pieces in a row. */
static void *eden_heap_spaces[EDEN_HEAP_PIECES];
static atomic_uint eden_heap_space_count;
/* The mspace each piece belongs to. */
static void *_Atomic eden_heap_piece_space[EDEN_HEAP_PIECES];
/* Where no range can be reserved, the heap takes this much at once. Its 6 GiB taken whole would
 * leave graphics too little of the 12 GiB pool. */
#define EDEN_HEAP_WHOLE_SIZE ((size_t)3072 << 20)
/* Pieces committed so far: [0, used). All of them when the heap could not be reserved to grow. */
static atomic_uint eden_heap_pieces_used;
static int eden_heap_grows;
/* The heap's range: PS5_OPENGL_HEAP_SIZE when it grows, EDEN_HEAP_WHOLE_SIZE when taken whole. */
static size_t eden_heap_range_size;
static atomic_uint eden_heap_space_hint; /* where the last allocation from the spaces succeeded */
static atomic_flag eden_heap_grow_lock = ATOMIC_FLAG_INIT;

/* Requests of this size or more get memory of their own (eden_heap_pages), freed with them. */
#define EDEN_HEAP_LARGE_REQUEST ((size_t)32 << 20)
#define EDEN_HEAP_LARGE_BLOCKS 256u
/* The large blocks alive: address 0 marks a free entry, 1 one being filled in. */
static struct {
  _Atomic uintptr_t address;
  size_t size;
} eden_heap_large[EDEN_HEAP_LARGE_BLOCKS];
static atomic_uint eden_heap_large_count;
static atomic_size_t eden_heap_large_bytes;

int sceKernelUsleep(unsigned int microseconds);

/* Arena creation is rare (once per thread) but runs under a spin flag, and its holder also takes
 * an mspace lock. The emulated cores and the GPU thread run at real-time priority pinned to
 * one CPU each; spinning there forever starves a preempted lower-priority holder queued on that
 * CPU, which hung game starts. After a short spin, sleep so the holder can run. */
static void eden_heap_create_acquire(void) {
  if (atomic_flag_test_and_set_explicit(&eden_heap_arena_create_lock, memory_order_acquire)) {
    atomic_fetch_add_explicit(&eden_heap_create_waiters, 1, memory_order_relaxed);
    for (unsigned spins = 0;
         atomic_flag_test_and_set_explicit(&eden_heap_arena_create_lock, memory_order_acquire); ++spins) {
      if (spins < 64)
        __builtin_ia32_pause();
      else
        sceKernelUsleep(50);
    }
    atomic_fetch_sub_explicit(&eden_heap_create_waiters, 1, memory_order_relaxed);
  }
  atomic_store_explicit(&eden_heap_create_holder, (unsigned long)pthread_self(), memory_order_relaxed);
}

static void eden_heap_create_release(void) {
  atomic_store_explicit(&eden_heap_create_holder, 0, memory_order_relaxed);
  atomic_flag_clear_explicit(&eden_heap_arena_create_lock, memory_order_release);
}

/* Development stall reports: the creation lock's holder (0: free) and its waiters. */
unsigned long eden_heap_create_lock_state(unsigned *waiters) {
  if (waiters)
    *waiters = atomic_load_explicit(&eden_heap_create_waiters, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_create_holder, memory_order_relaxed);
}

static void *eden_heap_owner(const void *address);

/* Per-thread allocation statistics. The live/peak/block counters are process-wide atomics, and
 * updating them on every malloc and free made all allocating threads bounce the same cache
 * lines: EDEN_PERF_ALLOC measured 210 ns per malloc/free pair alone and 874/1590 ns with 4/8
 * threads, while guest cores compile JIT blocks (allocation-heavy) together during loads. Each
 * thread now accumulates its changes and applies them every 64 operations, beyond 1 MiB, and at
 * thread exit; snapshots lag by that much. The record also keeps the thread's arena, and a
 * record freed by an exiting thread is reused with its arena by the next thread. */
/* Freed small blocks are kept per thread and reused without calling into the mspace, whose
 * calls serialise across threads even with separate arenas (EDEN_PERF_ALLOC with per-thread
 * arenas and statistics: 232 ns per pair alone, 803/1423 ns with 4/8 threads). A bin holds blocks
 * whose usable size is in [16 * bin, 16 * bin + 15], linked through their first word, with the
 * usable size in the second; a request of size s takes from bin ceil(s / 16). Bins are bounded,
 * and an exiting thread returns its blocks to their mspaces. */
#define EDEN_TCACHE_BINS 128u
#define EDEN_TCACHE_LIMIT 64u
struct eden_heap_thread {
  void *arena;   /* the arena small requests try first: one of `own`, or one shared at the limit */
  void *own[EDEN_HEAP_THREAD_ARENAS]; /* the arenas this record made, [0, owned) */
  unsigned owned;
  unsigned retry; /* requests to make before looking through `own` again (EDEN_HEAP_ARENA_RETRY) */
  long live;
  long blocks;
  unsigned pending;
  /* Small requests that went to the heap's spaces, frees there, and their cycles: added to the
   * process's totals with live and blocks (eden_heap_flush). */
  unsigned long long spills, spill_cycles, shared_frees, shared_free_cycles;
  void *bins[EDEN_TCACHE_BINS];
  unsigned char counts[EDEN_TCACHE_BINS];
};
#define EDEN_HEAP_THREADS 512u
static struct eden_heap_thread eden_heap_threads[EDEN_HEAP_THREADS];
static atomic_uchar eden_heap_thread_used[EDEN_HEAP_THREADS];

static void eden_heap_apply(long bytes, long blocks) {
  if (blocks)
    atomic_fetch_add_explicit(&ps5_heap_blocks, (size_t)blocks, memory_order_relaxed);
  if (bytes >= 0)
    ps5_heap_resize_stats(0, (size_t)bytes);
  else
    ps5_heap_resize_stats((size_t)-bytes, 0);
}

/* The process's totals of the small requests the arenas could not take and of the frees of blocks
 * in the heap's spaces (eden_heap_spill_stats), with their processor cycles. */
static atomic_ullong eden_heap_spills, eden_heap_spill_cycles, eden_heap_shared_frees,
    eden_heap_shared_free_cycles;

/* The processor's cycle counter, where there is one (the console and the x86-64 host). */
static unsigned long long eden_heap_cycles(void) {
#if defined(__x86_64__)
  return __builtin_ia32_rdtsc();
#else
  return 0;
#endif
}

static void eden_heap_flush(struct eden_heap_thread *self) {
  eden_heap_apply(self->live, self->blocks);
  self->live = 0;
  self->blocks = 0;
  self->pending = 0;
  if (self->spills | self->shared_frees) {
    atomic_fetch_add_explicit(&eden_heap_spills, self->spills, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_spill_cycles, self->spill_cycles, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_frees, self->shared_frees, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_free_cycles, self->shared_free_cycles, memory_order_relaxed);
    self->spills = self->spill_cycles = self->shared_frees = self->shared_free_cycles = 0;
  }
}

/* Small requests that went to the heap's spaces and frees of blocks there since the process began,
 * and the cycles they took (the counter's rate is the caller's to find). A thread's last 64
 * operations may not be in them yet. */
void eden_heap_spill_stats(unsigned long long *spills, unsigned long long *spill_cycles,
                           unsigned long long *frees, unsigned long long *free_cycles) {
  if (spills)
    *spills = atomic_load_explicit(&eden_heap_spills, memory_order_relaxed);
  if (spill_cycles)
    *spill_cycles = atomic_load_explicit(&eden_heap_spill_cycles, memory_order_relaxed);
  if (frees)
    *frees = atomic_load_explicit(&eden_heap_shared_frees, memory_order_relaxed);
  if (free_cycles)
    *free_cycles = atomic_load_explicit(&eden_heap_shared_free_cycles, memory_order_relaxed);
}

static void eden_heap_thread_exit(void *value) {
  struct eden_heap_thread *self = value;
  for (unsigned bin = 0; bin < EDEN_TCACHE_BINS; ++bin) {
    while (self->bins[bin] != NULL) {
      void *block = self->bins[bin];
      self->bins[bin] = *(void **)block;
      sceLibcMspaceFree(eden_heap_owner(block), block);
    }
    self->counts[bin] = 0;
  }
  eden_heap_flush(self);
  atomic_store_explicit(&eden_heap_thread_used[self - eden_heap_threads], 0, memory_order_release);
}

unsigned eden_heap_arenas_created(void) {
  return atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
}

/* Memory the heap's pieces hold now: this part only grows. */
size_t eden_heap_committed(void) {
  return (size_t)atomic_load_explicit(&eden_heap_pieces_used, memory_order_relaxed) << EDEN_HEAP_PIECE_SHIFT;
}

/* Memory of the large blocks alive now, and how many there are. */
size_t eden_heap_large_held(unsigned *blocks) {
  if (blocks)
    *blocks = atomic_load_explicit(&eden_heap_large_count, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
}

/* The entry of a large block, or NULL: most addresses are not one, and with none alive
 * nothing is searched. */
static int eden_heap_large_find(const void *address) {
  if (atomic_load_explicit(&eden_heap_large_count, memory_order_acquire) == 0 || address == NULL)
    return -1;
  for (unsigned i = 0; i < EDEN_HEAP_LARGE_BLOCKS; ++i)
    if (atomic_load_explicit(&eden_heap_large[i].address, memory_order_acquire) == (uintptr_t)address)
      return (int)i;
  return -1;
}

/* A large block of its own (zeroed), or NULL: no entry free, or no memory. */
static void *eden_heap_large_allocate(size_t bytes) {
  for (unsigned i = 0; i < EDEN_HEAP_LARGE_BLOCKS; ++i) {
    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&eden_heap_large[i].address, &expected, 1, memory_order_acquire,
                                                 memory_order_relaxed))
      continue;
    void *block = eden_heap_pages(bytes);
    if (block == NULL) {
      atomic_store_explicit(&eden_heap_large[i].address, 0, memory_order_release);
      return NULL;
    }
    eden_heap_large[i].size = bytes;
    atomic_fetch_add_explicit(&eden_heap_large_bytes, bytes, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_large_count, 1, memory_order_release);
    atomic_store_explicit(&eden_heap_large[i].address, (uintptr_t)block, memory_order_release);
    return block;
  }
  return NULL;
}

static void eden_heap_large_free(int entry) {
  void *block = (void *)atomic_load_explicit(&eden_heap_large[entry].address, memory_order_relaxed);
  const size_t bytes = eden_heap_large[entry].size;
  /* The entry stops naming the address before the memory goes: the next large block may be
   * given the same address at once. */
  atomic_store_explicit(&eden_heap_large[entry].address, 1, memory_order_release);
  eden_heap_pages_free(block, bytes);
  atomic_fetch_sub_explicit(&eden_heap_large_bytes, bytes, memory_order_relaxed);
  atomic_fetch_sub_explicit(&eden_heap_large_count, 1, memory_order_release);
  atomic_store_explicit(&eden_heap_large[entry].address, 0, memory_order_release);
}

static int ps5_heap_ready(void) {
  int state = atomic_load_explicit(&ps5_heap_state, memory_order_acquire);
  if (state == 2)
    return 1;
  if (state != 0)
    return 0;

  int expected = 0;
  if (!atomic_compare_exchange_strong_explicit(
          &ps5_heap_state, &expected, 1, memory_order_acq_rel,
          memory_order_acquire))
    return expected == 2;

  /* Address space for the whole heap and memory for its first piece; where a range cannot be
   * reserved, 3 GiB at once as before. */
  void *base = eden_heap_reserve(PS5_OPENGL_HEAP_SIZE);
  size_t first_size = EDEN_HEAP_PIECE_SIZE;
  unsigned first_pieces = 1;
  if (base != NULL && eden_heap_commit(base, EDEN_HEAP_PIECE_SIZE) == 0) {
    eden_heap_grows = 1;
  } else {
    base = eden_heap_pages(EDEN_HEAP_WHOLE_SIZE);
    first_size = EDEN_HEAP_WHOLE_SIZE;
    first_pieces = (unsigned)(EDEN_HEAP_WHOLE_SIZE >> EDEN_HEAP_PIECE_SHIFT);
  }
  if (base == NULL) {
    atomic_store_explicit(&ps5_heap_state, -1, memory_order_release);
    return 0;
  }

  ps5_heap_base = base;
  eden_heap_range_size = eden_heap_grows ? PS5_OPENGL_HEAP_SIZE : EDEN_HEAP_WHOLE_SIZE;
  ps5_heap_mspace = sceLibcMspaceCreate("Eden-headless", base, first_size, 0);
  if (ps5_heap_mspace == NULL) {
    ps5_heap_base = NULL;
    if (!eden_heap_grows)
      eden_heap_pages_free(base, EDEN_HEAP_WHOLE_SIZE);
    atomic_store_explicit(&ps5_heap_state, -1, memory_order_release);
    return 0;
  }
  eden_heap_spaces[0] = ps5_heap_mspace;
  for (unsigned piece = 0; piece < first_pieces; ++piece)
    atomic_store_explicit(&eden_heap_piece_space[piece], ps5_heap_mspace, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_pieces_used, first_pieces, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_space_count, 1, memory_order_release);
  /* libthr keeps specific data in its own storage; no recursion into malloc. */
  eden_heap_arena_key_valid = pthread_key_create(&eden_heap_arena_key, eden_heap_thread_exit) == 0;

  atomic_store_explicit(&ps5_heap_state, 2, memory_order_release);
  return 1;
}

/* A block inside the heap's range: an arena's or a piece's. */
static int eden_heap_in_range(const void *address) {
  /* Acquire publication before reading non-atomic heap metadata. */
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2)
    return 0;
  uintptr_t value = (uintptr_t)address;
  uintptr_t base = (uintptr_t)ps5_heap_base;
  return value >= base && value - base < eden_heap_range_size;
}

/* Any block of this heap: one in its range, or a large block with memory of its own. */
static int ps5_heap_owns(const void *address) {
  return eden_heap_in_range(address) || eden_heap_large_find(address) >= 0;
}

/* The usable size of a block this heap owns. */
static size_t eden_heap_usable(const void *address) {
  if (eden_heap_in_range(address))
    return sceLibcMspaceMallocUsableSize(address);
  const int entry = eden_heap_large_find(address);
  return entry >= 0 ? eden_heap_large[entry].size : 0;
}

static uintptr_t eden_heap_slot(const void *address) {
  return ((uintptr_t)address >> EDEN_HEAP_ARENA_SHIFT) -
         ((uintptr_t)ps5_heap_base >> EDEN_HEAP_ARENA_SHIFT);
}

/* The mspace owning an owned block: its slot's arena, else the space of its piece. */
static void *eden_heap_owner(const void *address) {
  const uintptr_t slot = eden_heap_slot(address);
  void *arena = atomic_load_explicit(&eden_heap_slots[slot], memory_order_acquire);
  if (arena != NULL)
    return arena;
  const uintptr_t piece = ((uintptr_t)address - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT;
  return atomic_load_explicit(&eden_heap_piece_space[piece], memory_order_acquire);
}

/* ---- the heap's own spaces ---- */

enum { EDEN_HEAP_MALLOC, EDEN_HEAP_CALLOC, EDEN_HEAP_ALIGNED };

/* One request to one mspace: malloc(a), calloc(a, b) or an a-aligned block of b bytes. */
static void *eden_heap_space_try(void *space, int kind, size_t a, size_t b) {
  if (kind == EDEN_HEAP_MALLOC)
    return sceLibcMspaceMalloc(space, a);
  if (kind == EDEN_HEAP_CALLOC)
    return sceLibcMspaceCalloc(space, a, b);
  void *address = NULL;
  return sceLibcMspacePosixMemalign(space, &address, a, b) == 0 ? address : NULL;
}

int sceKernelDebugOutText(int channel, const char *text);

/* Why the heap could not grow, for the kernel log: the allocation that asked is about to fail.
 * No stdio here (it allocates), and only the first few times. */
static void eden_heap_grow_refused(const char *line) {
  static atomic_uint reported;
  if (atomic_fetch_add_explicit(&reported, 1, memory_order_relaxed) < 8)
    sceKernelDebugOutText(0, line);
}

/* The last allocation the heap could not make, for the crash report (headless/crash_report.cpp):
 * a std::bad_alloc names neither the size nor the reason, and the report's memory line is read
 * after the failure, when other threads may have given memory back. `why` is the last reason the
 * heap gave for not growing (EDEN_HEAP_WHY_*), on whichever thread asked last. */
enum {
  EDEN_HEAP_WHY_NONE = 0,     /* it did not need to grow, or it grew */
  EDEN_HEAP_WHY_NO_SPACE = 1, /* memory, but no allocator over it */
  EDEN_HEAP_WHY_NO_MEMORY = 2,
  EDEN_HEAP_WHY_AT_LIMIT = 3,
  EDEN_HEAP_WHY_OVERFLOW = 4  /* calloc's count * size does not fit */
};
static atomic_uint eden_heap_grow_why;
static atomic_size_t eden_heap_failed_bytes;
static atomic_uint eden_heap_failed_why;

static void eden_heap_note_failure(size_t bytes, unsigned why) {
  atomic_store_explicit(&eden_heap_failed_bytes, bytes, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_failed_why, why, memory_order_relaxed);
}

static void eden_heap_failed(size_t bytes, unsigned why) {
  eden_heap_note_failure(bytes, why);
  atomic_fetch_add_explicit(&ps5_heap_failures, 1, memory_order_relaxed);
}

/* How many allocations failed, and the size and reason of the last one (0: none). */
size_t eden_heap_last_failure(unsigned *why, size_t *count) {
  if (why)
    *why = atomic_load_explicit(&eden_heap_failed_why, memory_order_relaxed);
  if (count)
    *count = atomic_load_explicit(&ps5_heap_failures, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_failed_bytes, memory_order_relaxed);
}

/* Adds a space with room for `bytes`: memory for the next pieces of the range and an mspace
 * over them. `seen` is how many spaces the caller tried: when another thread added one
 * meanwhile, that one is tried first. Returns 0 when the heap is at its limit or the console
 * has no memory left. */
static int eden_heap_grow(size_t bytes, unsigned seen) {
  for (unsigned spins = 0; atomic_flag_test_and_set_explicit(&eden_heap_grow_lock, memory_order_acquire); ++spins) {
    if (spins < 64)
      __builtin_ia32_pause();
    else
      sceKernelUsleep(50);
  }
  int grown = 0;
  atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NONE, memory_order_relaxed);
  const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_relaxed);
  const unsigned used = atomic_load_explicit(&eden_heap_pieces_used, memory_order_relaxed);
  /* Room for the request and for the mspace's own records. */
  const size_t need = bytes + ((size_t)1 << 20);
  const size_t pieces = (need + EDEN_HEAP_PIECE_SIZE - 1) >> EDEN_HEAP_PIECE_SHIFT;
  if (count != seen) {
    grown = 1;
  } else if (eden_heap_grows && need > bytes && pieces <= EDEN_HEAP_PIECES - used) {
    void *start = (char *)ps5_heap_base + ((size_t)used << EDEN_HEAP_PIECE_SHIFT);
    const size_t size = pieces << EDEN_HEAP_PIECE_SHIFT;
    if (eden_heap_commit(start, size) == 0) {
      void *space = sceLibcMspaceCreate("Eden-heap", start, size, 0);
      /* The pieces are taken either way: memory is not given back. */
      atomic_store_explicit(&eden_heap_pieces_used, used + (unsigned)pieces, memory_order_relaxed);
      if (space != NULL) {
        for (size_t piece = 0; piece < pieces; ++piece)
          atomic_store_explicit(&eden_heap_piece_space[used + piece], space, memory_order_release);
        eden_heap_spaces[count] = space;
        atomic_store_explicit(&eden_heap_space_count, count + 1, memory_order_release);
        grown = 1;
      } else {
        atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NO_SPACE, memory_order_relaxed);
        eden_heap_grow_refused("EDEN_HEAP_GROW refused: no allocator for a new piece\n");
      }
    } else {
      atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NO_MEMORY, memory_order_relaxed);
      eden_heap_grow_refused("EDEN_HEAP_GROW refused: the console has no memory for a new piece\n");
    }
  } else {
    atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_AT_LIMIT, memory_order_relaxed);
    eden_heap_grow_refused("EDEN_HEAP_GROW refused: the heap is at its limit\n");
  }
  atomic_flag_clear_explicit(&eden_heap_grow_lock, memory_order_release);
  return grown;
}

/* A block from the heap's spaces, then from a new one. A small request starts at the space that
 * served the last one (it is asked for millions of times: compiled-code records); a request of
 * 1 MiB or more starts at the first space, which keeps the later ones free of long-lived blocks
 * and able to take the next large request. */
static void *eden_heap_main(int kind, size_t a, size_t b) {
  const size_t bytes = kind == EDEN_HEAP_MALLOC ? a : kind == EDEN_HEAP_CALLOC ? a * b : a + b;
  for (;;) {
    const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_acquire);
    const int small = bytes < ((size_t)1 << 20);
    const unsigned hint = small ? atomic_load_explicit(&eden_heap_space_hint, memory_order_relaxed) : 0;
    for (unsigned i = 0; i < count; ++i) {
      const unsigned index = (hint + i) % count;
      void *address = eden_heap_space_try(eden_heap_spaces[index], kind, a, b);
      if (address != NULL) {
        if (small && i != 0)
          atomic_store_explicit(&eden_heap_space_hint, index, memory_order_relaxed);
        return address;
      }
    }
    if (bytes == 0 || !eden_heap_grow(bytes, count))
      return NULL;
  }
}

/* Allocate and register one arena block from the heap (NULL on failure, or at the limit: of first
 * arenas for a thread's first, of all arenas for another). */
static void *eden_heap_create_arena(int first) {
  eden_heap_create_acquire();
  void *arena = NULL;
  const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
  if (count < EDEN_HEAP_MAX_ARENAS && (!first || eden_heap_first_arenas < EDEN_HEAP_FIRST_ARENAS)) {
    void *block = eden_heap_main(EDEN_HEAP_ALIGNED, EDEN_HEAP_ARENA_SIZE, EDEN_HEAP_ARENA_SIZE);
    if (block != NULL) {
      arena = sceLibcMspaceCreate("Eden-arena", block, EDEN_HEAP_ARENA_SIZE, 0);
      if (arena != NULL) {
        const uintptr_t slot = eden_heap_slot(block);
        eden_heap_arena_list[count] = arena;
        atomic_store_explicit(&eden_heap_slots[slot], arena, memory_order_release);
        atomic_store_explicit(&eden_heap_arena_count, count + 1, memory_order_release);
        if (first)
          eden_heap_first_arenas++;
      } else {
        sceLibcMspaceFree(eden_heap_owner(block), block);
      }
    }
  }
  eden_heap_create_release();
  return arena;
}

/* This thread's record, claimed on its first allocation. NULL: none free (process-wide counters). */
static struct eden_heap_thread *eden_heap_self(void) {
  if (!eden_heap_arena_key_valid)
    return NULL;
  struct eden_heap_thread *self = pthread_getspecific(eden_heap_arena_key);
  if (self != NULL)
    return self;
  for (unsigned i = 0; i < EDEN_HEAP_THREADS; ++i) {
    unsigned char expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&eden_heap_thread_used[i], &expected, 1,
                                                 memory_order_acquire, memory_order_relaxed))
      continue;
    self = &eden_heap_threads[i];
    self->live = 0;
    self->blocks = 0;
    self->pending = 0;
    self->retry = 0;  /* its arenas (own) stay: this thread uses them */
    if (pthread_setspecific(eden_heap_arena_key, self) != 0) {
      atomic_store_explicit(&eden_heap_thread_used[i], 0, memory_order_release);
      return NULL;
    }
    return self;
  }
  return NULL;
}

static void eden_heap_charge(struct eden_heap_thread *self, long bytes, long blocks) {
  self->live += bytes;
  self->blocks += blocks;
  if (++self->pending >= 64 || self->live > (1l << 20) || self->live < -(1l << 20))
    eden_heap_flush(self);
}

static void eden_heap_account(long bytes, long blocks) {
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    eden_heap_apply(bytes, blocks);
  else
    eden_heap_charge(self, bytes, blocks);
}

static void *eden_heap_record(void *address, size_t bytes) {
  if (address == NULL) {
    if (bytes != 0)
      eden_heap_note_failure(bytes, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
    return ps5_heap_record_allocation(address, bytes != 0); /* counts the failure */
  }
  eden_heap_account((long)eden_heap_usable(address), 1);
  return address;
}

/* This thread's arena, created on its first small allocation. NULL = the heap's own spaces. */
static void *eden_heap_thread_arena(void) {
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    return NULL;
  if (self->arena != NULL)
    return self->arena;
  void *arena = eden_heap_create_arena(1);
  if (arena == NULL) {
    /* Arena limit reached: share the existing arenas round-robin. */
    const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_acquire);
    if (count == 0)
      return NULL;
    arena = eden_heap_arena_list[atomic_fetch_add_explicit(&eden_heap_next_shared, 1, memory_order_relaxed) % count];
  } else if (self->owned < EDEN_HEAP_THREAD_ARENAS) {
    self->own[self->owned++] = arena;
  }
  self->arena = arena;
  return arena;
}

static void *eden_heap_arena_try(void *arena, size_t count, size_t size, int zero) {
  return zero ? sceLibcMspaceCalloc(arena, count, size) : sceLibcMspaceMalloc(arena, count * size);
}

/* A small block from this thread's arenas: the one it uses; when that one is full, another of its
 * own with room, which it then uses; when none has, a new one (for a request under
 * EDEN_HEAP_ARENA_GROW_REQUEST, while the thread and the process are under their limits). NULL:
 * the heap's own spaces take the request. */
static void *eden_heap_arena_allocate(size_t count, size_t size, int zero) {
  void *arena = eden_heap_thread_arena();
  if (arena == NULL)
    return NULL;
  void *address = eden_heap_arena_try(arena, count, size, zero);
  if (address != NULL)
    return address;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL)
    return NULL;
  if (self->retry != 0) {
    --self->retry;
    return NULL;
  }
  for (unsigned i = 0; i < self->owned; ++i) {
    void *other = self->own[i];
    if (other == arena)
      continue;
    address = eden_heap_arena_try(other, count, size, zero);
    if (address != NULL) {
      self->arena = other;
      return address;
    }
  }
  if (count * size >= EDEN_HEAP_ARENA_GROW_REQUEST)
    return NULL;
  void *fresh = self->owned < EDEN_HEAP_THREAD_ARENAS ? eden_heap_create_arena(0) : NULL;
  if (fresh == NULL) {
    /* At a limit, or no memory for one: each look through the arenas costs a request to each,
     * so the next few requests go straight to the heap's spaces. */
    self->retry = EDEN_HEAP_ARENA_RETRY;
    return NULL;
  }
  self->own[self->owned++] = fresh;
  self->arena = fresh;
  return eden_heap_arena_try(fresh, count, size, zero);
}

/* A small request no arena took, from the heap's spaces, counted with its cycles. */
static void *eden_heap_spill(size_t count, size_t size, int zero) {
  const unsigned long long start = eden_heap_cycles();
  void *address = zero ? eden_heap_main(EDEN_HEAP_CALLOC, count, size)
                       : eden_heap_main(EDEN_HEAP_MALLOC, count * size, 0);
  const unsigned long long cycles = eden_heap_cycles() - start;
  struct eden_heap_thread *self = eden_heap_self();
  if (self != NULL) {
    self->spills++;
    self->spill_cycles += cycles;
  } else {
    atomic_fetch_add_explicit(&eden_heap_spills, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_spill_cycles, cycles, memory_order_relaxed);
  }
  return address;
}

/* Gives an owned block back to its arena, or to the heap's space, counting the latter (its lock is
 * the one every thread shares). */
static void eden_heap_release(void *address) {
  void *arena = atomic_load_explicit(&eden_heap_slots[eden_heap_slot(address)], memory_order_acquire);
  if (arena != NULL) {
    sceLibcMspaceFree(arena, address);
    return;
  }
  const unsigned long long start = eden_heap_cycles();
  sceLibcMspaceFree(eden_heap_owner(address), address);
  const unsigned long long cycles = eden_heap_cycles() - start;
  struct eden_heap_thread *self = eden_heap_self();
  if (self != NULL) {
    self->shared_frees++;
    self->shared_free_cycles += cycles;
  } else {
    atomic_fetch_add_explicit(&eden_heap_shared_frees, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&eden_heap_shared_free_cycles, cycles, memory_order_relaxed);
  }
}

/* A cached block for bytes (0 < bytes <= 16 * (EDEN_TCACHE_BINS - 1)), charged as allocated. */
static void *eden_tcache_take(size_t bytes) {
  const size_t bin = (bytes + 15) >> 4;
  if (bytes == 0 || bin >= EDEN_TCACHE_BINS)
    return NULL;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL || self->bins[bin] == NULL)
    return NULL;
  void *block = self->bins[bin];
  self->bins[bin] = ((void **)block)[0];
  self->counts[bin]--;
  eden_heap_charge(self, (long)((size_t *)block)[1], 1);
  return block;
}

/* Keeps a freed owned block of this usable size; 0 when the bin is full or out of range. */
static int eden_tcache_keep(void *address, size_t usable) {
  const size_t bin = usable >> 4;
  if (bin == 0 || bin >= EDEN_TCACHE_BINS)
    return 0;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL || self->counts[bin] >= EDEN_TCACHE_LIMIT)
    return 0;
  ((void **)address)[0] = self->bins[bin];
  ((size_t *)address)[1] = usable;
  self->bins[bin] = address;
  self->counts[bin]++;
  eden_heap_charge(self, -(long)usable, -1);
  return 1;
}

static void *eden_heap_allocate(size_t count, size_t size, int zero) {
  const size_t bytes = count * size;
  if (bytes >= EDEN_HEAP_LARGE_REQUEST && eden_heap_grows) {
    /* Memory of its own (it arrives zeroed). With every entry taken, or no memory left to take,
     * the heap's spaces: a piece committed earlier may still have the room. */
    void *block = eden_heap_large_allocate(bytes);
    if (block != NULL)
      return block;
  }
  if (bytes < EDEN_HEAP_SMALL_REQUEST) {
    void *address = eden_heap_arena_allocate(count, size, zero);
    return address != NULL ? address : eden_heap_spill(count, size, zero);
  }
  return zero ? eden_heap_main(EDEN_HEAP_CALLOC, count, size) : eden_heap_main(EDEN_HEAP_MALLOC, bytes, 0);
}

void *__wrap_malloc(size_t size) {
  if (!ps5_heap_ready())
    return __real_malloc(size);
  void *cached = eden_tcache_take(size);
  return cached != NULL ? cached : eden_heap_record(eden_heap_allocate(1, size, 0), size);
}

void *__wrap_calloc(size_t count, size_t size) {
  if (!ps5_heap_ready())
    return __real_calloc(count, size);
  if (size != 0 && count > (size_t)-1 / size) {
    eden_heap_failed((size_t)-1, EDEN_HEAP_WHY_OVERFLOW);
    return NULL;
  }
  void *cached = eden_tcache_take(count * size);
  if (cached != NULL)
    return memset(cached, 0, count * size);
  return eden_heap_record(eden_heap_allocate(count, size, 1), count * size);
}

void *__wrap_realloc(void *address, size_t size) {
  if (address == NULL)
    return __wrap_malloc(size);
  const int in_range = eden_heap_in_range(address);
  const int large = in_range ? -1 : eden_heap_large_find(address);
  if (!in_range && large < 0)
    return __real_realloc(address, size);
  const size_t before = in_range ? sceLibcMspaceMallocUsableSize(address) : eden_heap_large[large].size;
  if (large >= 0) {
    if (size == 0) {
      eden_heap_account(-(long)before, -1);
      eden_heap_large_free(large);
      return NULL;
    }
    /* A large block keeps its memory while the new size still needs most of it. */
    if (size <= before && size >= before / 2)
      return address;
  } else if (size < EDEN_HEAP_LARGE_REQUEST || !eden_heap_grows) {
    void *owner = eden_heap_owner(address);
    void *result = sceLibcMspaceRealloc(owner, address, size);
    if (result) {
      eden_heap_account((long)sceLibcMspaceMallocUsableSize(result) - (long)before, 0);
      return result;
    }
    if (size == 0) {
      /* Preserve platform realloc(p,0) behavior; NULL does not tell us whether
       * p was freed. Mark the counters inconclusive instead of guessing. */
      atomic_fetch_add_explicit(&ps5_heap_ambiguous_zero_reallocs, 1, memory_order_relaxed);
      return NULL;
    }
  }
  /* The block cannot stay where it is (its arena or its piece of the heap is full, or it
   * crosses the size at which blocks get memory of their own): move it. */
  void *moved = eden_heap_allocate(1, size, 0);
  if (moved == NULL) {
    eden_heap_failed(size, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
    return NULL;
  }
  memcpy(moved, address, before < size ? before : size);
  eden_heap_account((long)eden_heap_usable(moved) - (long)before, 0);
  if (large >= 0)
    eden_heap_large_free(large);
  else
    sceLibcMspaceFree(eden_heap_owner(address), address);
  return moved;
}

void __wrap_free(void *address) {
  if (eden_heap_in_range(address)) {
    const size_t usable = sceLibcMspaceMallocUsableSize(address);
    if (eden_tcache_keep(address, usable))
      return;
    eden_heap_account(-(long)usable, -1);
    eden_heap_release(address);
    return;
  }
  const int large = eden_heap_large_find(address);
  if (large >= 0) {
    eden_heap_account(-(long)eden_heap_large[large].size, -1);
    eden_heap_large_free(large);
  } else
    __real_free(address);
}

int __wrap_posix_memalign(void **address, size_t alignment, size_t size) {
  if (!ps5_heap_ready())
    return __real_posix_memalign(address, alignment, size);
  /* An alignment no space accepts must not make the heap grow looking for one that does. */
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  int result = ENOMEM;
  if (size < EDEN_HEAP_SMALL_REQUEST && alignment <= 4096) {
    void *arena = eden_heap_thread_arena();
    if (arena != NULL)
      result = sceLibcMspacePosixMemalign(arena, address, alignment, size);
  }
  if (result != 0) {
    /* A large block's own memory starts on a page; the heap's spaces align anything. */
    void *block = NULL;
    if (alignment <= 4096 && size >= EDEN_HEAP_LARGE_REQUEST && eden_heap_grows)
      block = eden_heap_large_allocate(size);
    if (block == NULL)
      block = eden_heap_main(EDEN_HEAP_ALIGNED, alignment, size);
    if (block != NULL) {
      *address = block;
      result = 0;
    } else {
      result = ENOMEM;
    }
  }
  if (result == 0)
    eden_heap_record(*address, size);
  else
    eden_heap_failed(size, atomic_load_explicit(&eden_heap_grow_why, memory_order_relaxed));
  return result;
}

size_t __wrap_malloc_usable_size(const void *address) {
  return ps5_heap_owns(address) ? eden_heap_usable(address) : __real_malloc_usable_size(address);
}

