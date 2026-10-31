/* Eden PS5 port: a heap that takes its memory as it grows, with per-thread arenas.
 *
 * Memory. PS5 direct memory is real from the first byte, and the CPU shares one 12 GiB pool with
 * the GPU. The heap used to take its whole 3 GiB at start; the largest game measured used
 * 1.16 GiB of it, and the rest was graphics memory that game then ran out of. So the heap now
 * reserves its range (6 GiB, PS5_OPENGL_HEAP_SIZE) as address space only and commits it a piece
 * (128 MiB) at a time, from the start of the range, when an allocation finds no room. Where no
 * range can be reserved, it takes 3 GiB at once, as it did before it grew. Every piece (or run of
 * pieces, for one request larger than a piece) is a Sony mspace of its own; an address tells its
 * piece, and the piece its mspace.
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
 * EDEN_HEAP_FIRST_ARENAS) gets a lazily created 8 MiB arena of its own. Small requests use the
 * thread's arena and fall back to the heap's own spaces; frees and reallocs find the owner in O(1)
 * from the address. A thread whose arenas are full makes another of its own (up to
 * EDEN_HEAP_THREAD_ARENAS, for requests under EDEN_HEAP_ARENA_GROW_REQUEST): the fifteen threads
 * that build a game's shader cache keep what they build, and with one arena each they filled it
 * after a few thousand pipelines and then allocated through the lock every thread shares. The
 * small requests that still reach the heap's own spaces, and the frees of blocks there, are
 * counted with the time they took (eden_heap_spill_stats; EDEN_SHADER_START_HEAP in stderr.log).
 *
 * Where arenas live. An arena was a block of 8 MiB aligned to 8 MiB taken from the heap's spaces.
 * The aligned request cost a space 16 MiB (the 8 MiB in front of the arena, to reach the
 * alignment, was left between arenas), and an arena is never freed, so every piece that held one
 * could never be given back. A Tears of the Kingdom start on v1.010.006 made 222 arenas while its
 * shader cache was built: 4.4 GB of pieces with 2.4 GB in use at once, and 0 MiB of them given
 * back all session (2.8 GB held with 16 MiB in use when another session ended). Arenas now take
 * slots of their own at the top of the range, 8 MiB each, committed directly: no space holds one,
 * nothing is lost to alignment, and pieces and arena slots grow towards each other until they
 * meet. An arena made because a thread's were full is given back once it holds no block (its
 * slot taken again by a later arena); the first ones, which threads share at the limit, stay.
 *
 * Memory given back. A space (the mspace over one piece, or a run of pieces for a larger request)
 * that holds no block, and an extra arena that holds none, is given back by eden_heap_trim:
 * retired so no request uses it, its memory released to the system, its address space kept.
 * A space or an arena that is partly used gives back the large pages inside its free runs: a run
 * of 4 MiB or more is taken as one block (a "hole"), and the 2 MiB pages wholly inside it are
 * released while the block stays taken, so the allocator, which only writes at a block's ends,
 * never touches them. Before the heap grows, every hole of its spaces is committed again and its
 * block freed (an arena fills its own when it is full): a hole keeps its run from joining the
 * blocks freed beside it, so holes are memory lent to the pool, not a way to keep free runs.
 * Holes leave the heap a slack of its own (EDEN_HEAP_SLACK, or an eighth of what it holds in use,
 * whichever is more), so a game that allocates again right away does not wait for the system.
 * Empty spaces and arenas go back at the end of a shader cache build, at the end of a session,
 * every 30 s, while the pool the CPU shares with the GPU is short, and before an allocation is
 * refused. Holes are made at the end of a shader cache build and of a session (once each) and
 * when memory is wanted at once: a graphics allocation was refused or a write of the game's waits
 * (headless/performance.cpp TrimHeap, RelieveMemory). Made at every shortage they kept the heap
 * growing past its own free runs (tools/check-memory-soak.py).
 *
 * Large blocks the pool has no room for in one piece. A block of its own is one piece of direct
 * memory, and a growth of the heap was too: in a fragmented pool both failed for a 486 MiB request
 * with a gigabyte free in smaller pieces, and the session ended. A large block that finds no one
 * piece is now address space with memory behind it in pieces of whatever size the pool has
 * (eden_heap_split_pages), and a growth takes its pieces' memory the same way (src/memory_pages.cpp).
 */
#include <pthread.h>
#include <string.h>

/* A spin's pause (the console and the x86-64 host; the arm64 host of the checks yields). */
static inline void eden_heap_pause(void) {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#endif
}

#define EDEN_HEAP_ARENA_SHIFT 23u /* 8 MiB arenas and slots */
#define EDEN_HEAP_ARENA_SIZE ((size_t)1 << EDEN_HEAP_ARENA_SHIFT)
/* Absolute 8 MiB slots covering the heap (+1: the base need not be slot aligned). */
#define EDEN_HEAP_SLOTS ((PS5_OPENGL_HEAP_SIZE >> EDEN_HEAP_ARENA_SHIFT) + 1u)
/* Threads that get an arena of their own at their first small request; later ones share them. */
#define EDEN_HEAP_FIRST_ARENAS 32u
/* Every arena alive at once: at most 2 GiB of the heap's 6 GiB of address space. */
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
/* Memory goes back to the system in large pages. */
#define EDEN_HEAP_PAGE ((uintptr_t)2 << 20)
/* Holes: at most this many at once, and the free memory the heap keeps for itself. */
#define EDEN_HEAP_HOLES 2048u
#define EDEN_HEAP_SLACK ((size_t)128 << 20)

void *eden_heap_reserve(size_t size);             /* address space only; NULL when refused */
int eden_heap_commit(void *address, size_t size); /* zeroed memory for a part of it; 0 when done */
/* Its memory back to the system, the address space kept: 1 done, 0 nothing done, -1 in part. */
int eden_heap_decommit(void *address, size_t size);
void *eden_heap_split_pages(size_t size);         /* a large block in pieces of memory; NULL: none */
void eden_heap_split_pages_free(void *address, size_t size);

/* An arena, by the 8 MiB slot its memory is in. */
struct eden_heap_arena {
  void *_Atomic space;      /* its mspace; NULL: the slot holds none */
  atomic_long blocks;       /* an extra arena's blocks not freed (its holes too) */
  atomic_uint users;        /* threads inside an extra arena now */
  atomic_int retired;       /* 1: no request may use it */
  atomic_uint generation;   /* bumped each time the slot's arena is given back */
  atomic_uchar extra;       /* made because a thread's arenas were full: given back when empty */
  atomic_uchar lost;        /* given back in part: the slot is not used again */
  atomic_uint holes;        /* holes in it now */
};
static struct eden_heap_arena eden_heap_arenas[EDEN_HEAP_SLOTS];
static atomic_uint eden_heap_arena_count;   /* arenas alive */
static atomic_uint eden_heap_arenas_made;   /* arenas made since the process began */
static atomic_size_t eden_heap_arena_bytes; /* memory the arenas hold in their own slots */
static unsigned eden_heap_first_slots[EDEN_HEAP_FIRST_ARENAS]; /* the first arenas (they stay) */
static atomic_uint eden_heap_first_arenas;  /* first arenas made (written under the creation lock) */
static atomic_uint eden_heap_next_shared;
/* The arena slots at the top of the range: [floor, top]; floor > top while there is none. */
static unsigned eden_heap_arena_floor, eden_heap_arena_top, eden_heap_arena_low;
static pthread_key_t eden_heap_arena_key;
static int eden_heap_arena_key_valid;
static atomic_flag eden_heap_arena_create_lock = ATOMIC_FLAG_INIT;
/* Who holds the creation lock and how many threads wait for it (read by the development stall
 * watchdog, headless/stall_watchdog.h). */
static _Atomic unsigned long eden_heap_create_holder;
static atomic_uint eden_heap_create_waiters;

/* The heap's own mspaces, a slot each; each spans one or more pieces in a row. Slot 0 is the
 * first space and stays. A slot whose space was given back (eden_heap_trim) is retired, and a
 * later growth may use it again. */
static void *_Atomic eden_heap_spaces[EDEN_HEAP_PIECES];
static atomic_uint eden_heap_space_count;                    /* slots in use, retired ones too */
static atomic_uint eden_heap_space_users[EDEN_HEAP_PIECES];  /* threads in the slot's space now */
static atomic_int eden_heap_space_retired[EDEN_HEAP_PIECES]; /* 1: no request uses it */
static atomic_long eden_heap_space_blocks[EDEN_HEAP_PIECES]; /* blocks taken from it, not freed */
static atomic_uint eden_heap_space_holes[EDEN_HEAP_PIECES];  /* of those, holes */
/* A slot's pieces, [first, first + span); span 0: given back (both under the growth lock). */
static unsigned eden_heap_space_first[EDEN_HEAP_PIECES], eden_heap_space_span[EDEN_HEAP_PIECES];
/* Bumped by every growth: a request that found no room tries again before growing itself. */
static atomic_uint eden_heap_space_generation;
/* The mspace each piece belongs to, and its slot. */
static void *_Atomic eden_heap_piece_space[EDEN_HEAP_PIECES];
static atomic_uint eden_heap_piece_slot[EDEN_HEAP_PIECES];
/* A piece below `used`: 0 in a space, 1 given back (a growth may take it again), 2 given back in
 * part (never used again; not the heap's to free into). */
static atomic_uchar eden_heap_piece_state[EDEN_HEAP_PIECES];
/* What the pieces and arena slots hold now, and what was given back since the process began. */
static atomic_size_t eden_heap_committed_bytes, eden_heap_returned_bytes;
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

/* Holes (under the growth lock): a block taken from a space or an arena whose large pages,
 * [begin, end), went back to the system. `owner` >= 0: a space's slot; < 0: -(arena slot + 1). */
static struct {
  void *block;
  uintptr_t begin, end;
  int owner;
  unsigned char lost; /* given back in part: the block stays taken for good */
} eden_heap_holes[EDEN_HEAP_HOLES];
static unsigned eden_heap_hole_count;
static atomic_size_t eden_heap_hole_bytes;
static atomic_uint eden_heap_hole_total; /* holes made since the process began */
static atomic_uint eden_heap_holes_now;  /* holes now */

/* Requests of this size or more get memory of their own (eden_heap_pages), freed with them. */
#define EDEN_HEAP_LARGE_REQUEST ((size_t)32 << 20)
#define EDEN_HEAP_LARGE_BLOCKS 256u
/* The large blocks alive: address 0 marks a free entry, 1 one being filled in. */
static struct {
  _Atomic uintptr_t address;
  size_t size;
  int split; /* its memory in pieces (eden_heap_split_pages) */
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
        eden_heap_pause();
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

static void eden_heap_free_owned(void *address);

/* Per-thread allocation statistics. The live/peak/block counters are process-wide atomics, and
 * updating them on every malloc and free made all allocating threads bounce the same cache
 * lines: EDEN_PERF_ALLOC measured 210 ns per malloc/free pair alone and 874/1590 ns with 4/8
 * threads, while guest cores compile JIT blocks (allocation-heavy) together during loads. Each
 * thread now accumulates its changes and applies them every 64 operations, beyond 1 MiB, and at
 * thread exit; snapshots lag by that much. The record also keeps the thread's arenas, and a
 * record freed by an exiting thread is reused with its arenas by the next thread. */
/* Freed small blocks are kept per thread and reused without calling into the mspace, whose
 * calls serialise across threads even with separate arenas (EDEN_PERF_ALLOC with per-thread
 * arenas and statistics: 232 ns per pair alone, 803/1423 ns with 4/8 threads). A bin holds blocks
 * whose usable size is in [16 * bin, 16 * bin + 15], linked through their first word, with the
 * usable size in the second; a request of size s takes from bin ceil(s / 16). Bins are bounded,
 * and an exiting thread returns its blocks to their mspaces. */
#define EDEN_TCACHE_BINS 128u
#define EDEN_TCACHE_LIMIT 64u
/* One of a record's arenas: its slot, and the slot's generation when the record took it (a slot
 * given back and made again is another arena). */
struct eden_heap_own {
  unsigned slot;
  unsigned generation;
};
struct eden_heap_thread {
  /* The arena small requests try first (slot + 1; 0: none yet): one of `own`, or a first arena
   * shared at the limit. */
  unsigned current;
  unsigned current_generation;
  struct eden_heap_own own[EDEN_HEAP_THREAD_ARENAS]; /* the arenas this record made, [0, owned) */
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
      eden_heap_free_owned(block);
    }
    self->counts[bin] = 0;
  }
  eden_heap_flush(self);
  atomic_store_explicit(&eden_heap_thread_used[self - eden_heap_threads], 0, memory_order_release);
}

/* Arenas made since the process began (some may have been given back since). */
unsigned eden_heap_arenas_created(void) {
  return atomic_load_explicit(&eden_heap_arenas_made, memory_order_relaxed);
}

/* Arenas alive now, and the memory they hold in slots of their own (none when the heap was taken
 * whole: its arenas are blocks of its one space). */
size_t eden_heap_arena_usage(unsigned *count) {
  if (count)
    *count = atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_arena_bytes, memory_order_relaxed);
}

/* The memory of the holes in partly used spaces and arenas now, and how many there are. */
size_t eden_heap_hole_usage(unsigned *count) {
  if (count)
    *count = atomic_load_explicit(&eden_heap_holes_now, memory_order_relaxed);
  return atomic_load_explicit(&eden_heap_hole_bytes, memory_order_relaxed);
}

/* Memory the heap's pieces and arena slots hold now (what was given back is not counted). */
size_t eden_heap_committed(void) {
  return atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed);
}

/* What the heap holds (its pieces, its arena slots and its large blocks), what of that its blocks
 * use (its own count, which lags each thread's last 64 operations), and what it has given back. */
void eden_heap_usage(size_t *held, size_t *in_use, size_t *returned) {
  if (held)
    *held = atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed) +
            atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  if (in_use)
    *in_use = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  if (returned)
    *returned = atomic_load_explicit(&eden_heap_returned_bytes, memory_order_relaxed);
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
    int split = 0;
    if (block == NULL) {
      /* No free piece of the pool that large: memory in smaller pieces behind one range. */
      block = eden_heap_split_pages(bytes);
      split = 1;
    }
    if (block == NULL) {
      atomic_store_explicit(&eden_heap_large[i].address, 0, memory_order_release);
      return NULL;
    }
    eden_heap_large[i].size = bytes;
    eden_heap_large[i].split = split;
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
  if (eden_heap_large[entry].split)
    eden_heap_split_pages_free(block, bytes);
  else
    eden_heap_pages_free(block, bytes);
  atomic_fetch_sub_explicit(&eden_heap_large_bytes, bytes, memory_order_relaxed);
  atomic_fetch_sub_explicit(&eden_heap_large_count, 1, memory_order_release);
  atomic_store_explicit(&eden_heap_large[entry].address, 0, memory_order_release);
}

/* The address of an 8 MiB slot (counted from the slot the heap's base is in). */
static uintptr_t eden_heap_slot_address(unsigned slot) {
  return (((uintptr_t)ps5_heap_base >> EDEN_HEAP_ARENA_SHIFT) + slot) << EDEN_HEAP_ARENA_SHIFT;
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
  /* The arena slots wholly inside the range; arenas take them from the top down. */
  const uintptr_t start = (uintptr_t)base, end = start + eden_heap_range_size;
  eden_heap_arena_low = (start & (EDEN_HEAP_ARENA_SIZE - 1)) != 0;
  eden_heap_arena_top = (unsigned)((end >> EDEN_HEAP_ARENA_SHIFT) - (start >> EDEN_HEAP_ARENA_SHIFT)) - 1u;
  eden_heap_arena_floor = eden_heap_arena_top + 1u;
  atomic_store_explicit(&eden_heap_spaces[0], ps5_heap_mspace, memory_order_relaxed);
  eden_heap_space_first[0] = 0;
  eden_heap_space_span[0] = first_pieces;
  for (unsigned piece = 0; piece < first_pieces; ++piece)
    atomic_store_explicit(&eden_heap_piece_space[piece], ps5_heap_mspace, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_pieces_used, first_pieces, memory_order_relaxed);
  atomic_store_explicit(&eden_heap_committed_bytes, first_size, memory_order_relaxed);
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
  if (value < base || value - base >= eden_heap_range_size)
    return 0;
  /* A piece given back in part is not the heap's any more. */
  return atomic_load_explicit(&eden_heap_piece_state[(value - base) >> EDEN_HEAP_PIECE_SHIFT],
                              memory_order_relaxed) != 2;
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

/* The arena an owned block is in (its slot's), or NULL: a block of a space. */
static void *eden_heap_arena_of(const void *address) {
  return atomic_load_explicit(&eden_heap_arenas[eden_heap_slot(address)].space, memory_order_acquire);
}

/* The mspace owning an owned block: its slot's arena, else the space of its piece. */
static void *eden_heap_owner(const void *address) {
  void *arena = eden_heap_arena_of(address);
  if (arena != NULL)
    return arena;
  const uintptr_t piece = ((uintptr_t)address - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT;
  return atomic_load_explicit(&eden_heap_piece_space[piece], memory_order_acquire);
}

/* ---- the heap's own spaces ---- */

enum { EDEN_HEAP_MALLOC, EDEN_HEAP_CALLOC, EDEN_HEAP_ALIGNED };

/* Frees a block of a space (not an arena's), counting it off its space. */
static void eden_heap_space_free(void *address) {
  const uintptr_t piece = ((uintptr_t)address - (uintptr_t)ps5_heap_base) >> EDEN_HEAP_PIECE_SHIFT;
  const unsigned slot = atomic_load_explicit(&eden_heap_piece_slot[piece], memory_order_acquire);
  sceLibcMspaceFree(atomic_load_explicit(&eden_heap_piece_space[piece], memory_order_acquire), address);
  atomic_fetch_sub_explicit(&eden_heap_space_blocks[slot], 1, memory_order_release);
}

/* Frees a block of an arena, counting it off an extra arena. */
static void eden_heap_arena_free(void *arena, void *address) {
  struct eden_heap_arena *record = &eden_heap_arenas[eden_heap_slot(address)];
  sceLibcMspaceFree(arena, address);
  if (atomic_load_explicit(&record->extra, memory_order_relaxed))
    atomic_fetch_sub_explicit(&record->blocks, 1, memory_order_release);
}

/* Frees an owned block (not a large one) to its arena or its space. */
static void eden_heap_free_owned(void *address) {
  void *arena = eden_heap_arena_of(address);
  if (arena != NULL)
    eden_heap_arena_free(arena, address);
  else
    eden_heap_space_free(address);
}

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

static void eden_heap_grow_acquire(void) {
  for (unsigned spins = 0; atomic_flag_test_and_set_explicit(&eden_heap_grow_lock, memory_order_acquire); ++spins) {
    if (spins < 64)
      eden_heap_pause();
    else
      sceKernelUsleep(50);
  }
}

static void eden_heap_grow_release(void) {
  atomic_flag_clear_explicit(&eden_heap_grow_lock, memory_order_release);
}

/* ---- holes: the large pages of free runs inside spaces and arenas, given back ---- */

/* The mspace of a hole's owner (the growth lock is held). */
static void *eden_heap_hole_space(int owner) {
  return owner >= 0 ? atomic_load_explicit(&eden_heap_spaces[owner], memory_order_acquire)
                    : atomic_load_explicit(&eden_heap_arenas[-owner - 1].space, memory_order_acquire);
}

/* Commits a hole's pages again and frees its block, so its owner has the room again. 0: the system
 * has no memory for it (it stays). The growth lock is held. */
static int eden_heap_hole_fill(unsigned index) {
  const uintptr_t begin = eden_heap_holes[index].begin, end = eden_heap_holes[index].end;
  const int owner = eden_heap_holes[index].owner;
  void *const block = eden_heap_holes[index].block;
  if (eden_heap_commit((void *)begin, end - begin) != 0)
    return 0;
  atomic_fetch_add_explicit(&eden_heap_committed_bytes, end - begin, memory_order_relaxed);
  atomic_fetch_sub_explicit(&eden_heap_hole_bytes, end - begin, memory_order_relaxed);
  sceLibcMspaceFree(eden_heap_hole_space(owner), block);
  if (owner >= 0) {
    atomic_fetch_sub_explicit(&eden_heap_space_holes[owner], 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&eden_heap_space_blocks[owner], 1, memory_order_release);
  } else {
    struct eden_heap_arena *arena = &eden_heap_arenas[-owner - 1];
    atomic_fetch_sub_explicit(&arena->holes, 1, memory_order_relaxed);
    if (atomic_load_explicit(&arena->extra, memory_order_relaxed))
      atomic_fetch_sub_explicit(&arena->blocks, 1, memory_order_release);
  }
  eden_heap_holes[index] = eden_heap_holes[--eden_heap_hole_count];
  atomic_fetch_sub_explicit(&eden_heap_holes_now, 1, memory_order_relaxed);
  return 1;
}

/* Forgets the holes of an owner whose memory went back whole (the growth lock is held). */
static void eden_heap_holes_drop(int owner) {
  for (unsigned index = 0; index < eden_heap_hole_count;) {
    if (eden_heap_holes[index].owner != owner) {
      ++index;
      continue;
    }
    if (!eden_heap_holes[index].lost)
      atomic_fetch_sub_explicit(&eden_heap_hole_bytes, eden_heap_holes[index].end - eden_heap_holes[index].begin,
                                memory_order_relaxed);
    eden_heap_holes[index] = eden_heap_holes[--eden_heap_hole_count];
    atomic_fetch_sub_explicit(&eden_heap_holes_now, 1, memory_order_relaxed);
  }
  if (owner >= 0)
    atomic_store_explicit(&eden_heap_space_holes[owner], 0, memory_order_relaxed);
  else
    atomic_store_explicit(&eden_heap_arenas[-owner - 1].holes, 0, memory_order_relaxed);
}

/* Holes' bytes in an owner (the growth lock is held). */
static size_t eden_heap_holes_in(int owner) {
  size_t bytes = 0;
  for (unsigned index = 0; index < eden_heap_hole_count; ++index)
    if (eden_heap_holes[index].owner == owner && !eden_heap_holes[index].lost)
      bytes += eden_heap_holes[index].end - eden_heap_holes[index].begin;
  return bytes;
}

/* Gives back room in an owner's space: the hole whose block is the smallest that holds `need`
 * bytes (owner == EDEN_HEAP_ANY_SPACE: in any space). Returns 1 when one was filled. The growth
 * lock is held. */
#define EDEN_HEAP_ANY_SPACE (-0x7fffffff - 1)
static int eden_heap_holes_fill(int owner, size_t need) {
  unsigned best = EDEN_HEAP_HOLES;
  size_t best_size = (size_t)-1;
  for (unsigned index = 0; index < eden_heap_hole_count; ++index) {
    const int of = eden_heap_holes[index].owner;
    if (eden_heap_holes[index].lost || (owner == EDEN_HEAP_ANY_SPACE ? of < 0 : of != owner))
      continue;
    const size_t size = sceLibcMspaceMallocUsableSize(eden_heap_holes[index].block);
    if (size >= need && size < best_size) {
      best = index;
      best_size = size;
    }
  }
  return best < EDEN_HEAP_HOLES ? eden_heap_hole_fill(best) : 0;
}

/* Every hole of the spaces filled again, before the heap grows: a hole keeps its run from joining
 * the blocks freed beside it, and a heap that grew past its holes would keep both. A hole the
 * system has no memory for stays, and so does every hole as large: only a smaller one is tried
 * again in this growth (each refused commit searched the pool, src/direct_memory.c). Returns the
 * holes filled. The growth lock is held. */
static unsigned eden_heap_holes_fill_spaces(void) {
  unsigned filled = 0;
  uintptr_t refused = (uintptr_t)-1; /* the smallest hole the system had no memory for */
  for (unsigned index = eden_heap_hole_count; index-- > 0;) {
    if (index >= eden_heap_hole_count || eden_heap_holes[index].lost || eden_heap_holes[index].owner < 0)
      continue;
    const uintptr_t size = eden_heap_holes[index].end - eden_heap_holes[index].begin;
    if (size >= refused)
      continue;
    if (eden_heap_hole_fill(index))
      ++filled;
    else
      refused = size;
  }
  return filled;
}

/* How much free memory the heap keeps before it makes holes: EDEN_HEAP_SLACK, or an eighth of what
 * its blocks use, whichever is more. */
static size_t eden_heap_slack(void) {
  const size_t live = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  const size_t large = atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  const size_t small = live > large ? live - large : 0;
  return small / 8 > EDEN_HEAP_SLACK ? small / 8 : EDEN_HEAP_SLACK;
}

/* Free memory the heap holds beyond what its blocks use (an estimate: the counts lag). */
static size_t eden_heap_free_held(void) {
  const size_t committed = atomic_load_explicit(&eden_heap_committed_bytes, memory_order_relaxed);
  const size_t live = atomic_load_explicit(&ps5_heap_live_bytes, memory_order_relaxed);
  const size_t large = atomic_load_explicit(&eden_heap_large_bytes, memory_order_relaxed);
  const size_t small = live > large ? live - large : 0;
  return committed > small ? committed - small : 0;
}

/* Makes holes in one owner's free runs: takes blocks of each size (largest first) while it can
 * and the heap keeps its slack, and gives back their whole large pages. Returns the bytes given
 * back; *stop is set when the system keeps memory (dev-settings heap_return=off). The growth lock
 * is held, and the owner is not retired. */
static size_t eden_heap_holes_make(int owner, void *space, const size_t *sizes, unsigned kinds, int *stop) {
  size_t given = 0;
  for (unsigned kind = 0; kind < kinds && !*stop; ++kind) {
    for (unsigned tries = 0; tries < 16 && eden_heap_hole_count < EDEN_HEAP_HOLES; ++tries) {
      if (eden_heap_free_held() < eden_heap_slack() + sizes[kind])
        return given;
      void *block = sceLibcMspaceMalloc(space, sizes[kind]);
      if (block == NULL)
        break;
      const uintptr_t at = (uintptr_t)block, usable = sceLibcMspaceMallocUsableSize(block);
      const uintptr_t begin = (at + EDEN_HEAP_PAGE - 1) & ~(EDEN_HEAP_PAGE - 1);
      const uintptr_t end = (at + usable) & ~(EDEN_HEAP_PAGE - 1);
      const int done = end > begin ? eden_heap_decommit((void *)begin, end - begin) : 0;
      if (done == 0) {
        sceLibcMspaceFree(space, block);
        if (end > begin)
          *stop = 1;
        break;
      }
      const unsigned index = eden_heap_hole_count++;
      eden_heap_holes[index].block = block;
      eden_heap_holes[index].begin = begin;
      eden_heap_holes[index].end = end;
      eden_heap_holes[index].owner = owner;
      eden_heap_holes[index].lost = done < 0;
      atomic_fetch_add_explicit(&eden_heap_hole_total, 1, memory_order_relaxed);
      if (owner >= 0) {
        atomic_fetch_add_explicit(&eden_heap_space_blocks[owner], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&eden_heap_space_holes[owner], 1, memory_order_relaxed);
      } else {
        struct eden_heap_arena *arena = &eden_heap_arenas[-owner - 1];
        if (atomic_load_explicit(&arena->extra, memory_order_relaxed))
          atomic_fetch_add_explicit(&arena->blocks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&arena->holes, 1, memory_order_relaxed);
      }
      /* A hole given back in part stays counted as held: the space or arena it is in counts its
       * range off when it goes back whole (eden_heap_holes_in leaves lost holes out). */
      atomic_fetch_add_explicit(&eden_heap_holes_now, 1, memory_order_relaxed);
      if (done > 0) {
        atomic_fetch_sub_explicit(&eden_heap_committed_bytes, end - begin, memory_order_relaxed);
        atomic_fetch_add_explicit(&eden_heap_hole_bytes, end - begin, memory_order_relaxed);
        given += end - begin;
      }
    }
  }
  return given;
}

/* ---- growth ---- */

/* Adds a space with room for `bytes`: first the holes of the spaces (all of them filled again),
 * else memory for pieces of the range (pieces given back earlier first, else the next ones, below
 * the arenas' slots) and an mspace over them, in a slot given back earlier or a new one. `seen` is
 * the growth the caller tried after: when another thread grew the heap meanwhile, that space is
 * tried first. Returns 0 when the heap is at its limit or the console has no memory left. */
static int eden_heap_grow(size_t bytes, unsigned seen) {
  eden_heap_grow_acquire();
  int grown = 0;
  atomic_store_explicit(&eden_heap_grow_why, EDEN_HEAP_WHY_NONE, memory_order_relaxed);
  const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_relaxed);
  const unsigned used = atomic_load_explicit(&eden_heap_pieces_used, memory_order_relaxed);
  /* Room for the request and for the mspace's own records. */
  const size_t need = bytes + ((size_t)1 << 20);
  const size_t pieces = (need + EDEN_HEAP_PIECE_SIZE - 1) >> EDEN_HEAP_PIECE_SHIFT;
  /* Pieces given back, `pieces` of them in a row, else the next ones. */
  unsigned first = used;
  if (eden_heap_grows && need > bytes) {
    for (unsigned at = 0, run = 0; at < used; ++at) {
      run = atomic_load_explicit(&eden_heap_piece_state[at], memory_order_relaxed) == 1 ? run + 1 : 0;
      if (run == pieces) {
        first = at + 1 - (unsigned)pieces;
        break;
      }
    }
  }
  /* The pieces end below the lowest arena slot. */
  unsigned pieces_limit = EDEN_HEAP_PIECES;
  if (eden_heap_grows && eden_heap_arena_floor <= eden_heap_arena_top) {
    const uintptr_t floor = eden_heap_slot_address(eden_heap_arena_floor) - (uintptr_t)ps5_heap_base;
    if ((floor >> EDEN_HEAP_PIECE_SHIFT) < pieces_limit)
      pieces_limit = (unsigned)(floor >> EDEN_HEAP_PIECE_SHIFT);
  }
  /* A slot given back, else a new one. */
  unsigned slot = count;
  for (unsigned at = 1; at < count; ++at)
    if (eden_heap_space_span[at] == 0) {
      slot = at;
      break;
    }
  if (atomic_load_explicit(&eden_heap_space_generation, memory_order_relaxed) != seen) {
    grown = 1;
  } else if (eden_heap_hole_count != 0 && need > bytes && eden_heap_holes_fill_spaces() != 0) {
    /* The spaces have their room back: the request tries them again. */
    atomic_fetch_add_explicit(&eden_heap_space_generation, 1, memory_order_release);
    grown = 1;
  } else if (eden_heap_grows && need > bytes && slot < EDEN_HEAP_PIECES &&
             (first < used || (used <= pieces_limit && pieces <= pieces_limit - used))) {
    void *start = (char *)ps5_heap_base + ((size_t)first << EDEN_HEAP_PIECE_SHIFT);
    const size_t size = pieces << EDEN_HEAP_PIECE_SHIFT;
    if (eden_heap_commit(start, size) == 0) {
      void *space = sceLibcMspaceCreate("Eden-heap", start, size, 0);
      atomic_fetch_add_explicit(&eden_heap_committed_bytes, size, memory_order_relaxed);
      if (first == used)
        atomic_store_explicit(&eden_heap_pieces_used, used + (unsigned)pieces, memory_order_relaxed);
      if (space != NULL) {
        for (size_t piece = first; piece < first + pieces; ++piece) {
          atomic_store_explicit(&eden_heap_piece_space[piece], space, memory_order_release);
          atomic_store_explicit(&eden_heap_piece_slot[piece], slot, memory_order_release);
          atomic_store_explicit(&eden_heap_piece_state[piece], 0, memory_order_relaxed);
        }
        eden_heap_space_first[slot] = first;
        eden_heap_space_span[slot] = (unsigned)pieces;
        atomic_store_explicit(&eden_heap_space_blocks[slot], 0, memory_order_relaxed);
        atomic_store_explicit(&eden_heap_space_holes[slot], 0, memory_order_relaxed);
        atomic_store_explicit(&eden_heap_spaces[slot], space, memory_order_release);
        if (slot == count)
          atomic_store_explicit(&eden_heap_space_count, count + 1, memory_order_release);
        else
          atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
        atomic_fetch_add_explicit(&eden_heap_space_generation, 1, memory_order_release);
        grown = 1;
      } else {
        /* The memory stays committed but no space is over it: given back at once. */
        if (eden_heap_decommit(start, size) == 1) {
          atomic_fetch_sub_explicit(&eden_heap_committed_bytes, size, memory_order_relaxed);
          for (size_t piece = first; piece < first + pieces; ++piece)
            atomic_store_explicit(&eden_heap_piece_state[piece], 1, memory_order_relaxed);
        }
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
  eden_heap_grow_release();
  return grown;
}

/* Memory for an arena in a slot of its own at the top of the range: a slot given back earlier,
 * else the one below the lowest, above the pieces. Returns the slot, or -1. The growth lock is
 * held. */
static int eden_heap_arena_slot(void) {
  if (eden_heap_arena_top < eden_heap_arena_low)
    return -1;
  /* A slot an arena gave back. */
  for (unsigned slot = eden_heap_arena_top + 1u; slot-- > eden_heap_arena_floor;) {
    if (atomic_load_explicit(&eden_heap_arenas[slot].space, memory_order_relaxed) != NULL ||
        atomic_load_explicit(&eden_heap_arenas[slot].lost, memory_order_relaxed))
      continue;
    return eden_heap_commit((void *)eden_heap_slot_address(slot), EDEN_HEAP_ARENA_SIZE) == 0 ? (int)slot : -1;
  }
  /* The next one down, above the pieces. */
  if (eden_heap_arena_floor <= eden_heap_arena_low)
    return -1;
  const unsigned slot = eden_heap_arena_floor - 1u;
  const uintptr_t at = eden_heap_slot_address(slot);
  const uintptr_t pieces_end = (uintptr_t)ps5_heap_base +
      ((uintptr_t)atomic_load_explicit(&eden_heap_pieces_used, memory_order_relaxed) << EDEN_HEAP_PIECE_SHIFT);
  if (at < pieces_end || eden_heap_commit((void *)at, EDEN_HEAP_ARENA_SIZE) != 0)
    return -1;
  eden_heap_arena_floor = slot;
  return (int)slot;
}

/* Gives back every space (but the first) and every extra arena that holds no block but its holes,
 * and, with `holes`, makes holes in what is left: their memory is released to the system, their
 * address space kept for a later growth. Returns the bytes given back. */
size_t eden_heap_trim_level(int holes) {
  if (atomic_load_explicit(&ps5_heap_state, memory_order_acquire) != 2 || !eden_heap_grows)
    return 0;
  eden_heap_create_acquire();
  eden_heap_grow_acquire();
  size_t returned = 0;
  const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_relaxed);
  for (unsigned slot = 1; slot < count; ++slot) {
    const long empty = (long)atomic_load_explicit(&eden_heap_space_holes[slot], memory_order_relaxed);
    if (eden_heap_space_span[slot] == 0 ||
        atomic_load_explicit(&eden_heap_space_blocks[slot], memory_order_relaxed) != empty)
      continue;
    atomic_store_explicit(&eden_heap_space_retired[slot], 1, memory_order_seq_cst);
    if (atomic_load_explicit(&eden_heap_space_users[slot], memory_order_seq_cst) != 0 ||
        atomic_load_explicit(&eden_heap_space_blocks[slot], memory_order_acquire) != empty) {
      atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
      continue;
    }
    const unsigned first = eden_heap_space_first[slot], span = eden_heap_space_span[slot];
    const size_t size = (size_t)span << EDEN_HEAP_PIECE_SHIFT;
    const size_t in_holes = eden_heap_holes_in((int)slot);
    const int done = eden_heap_decommit((char *)ps5_heap_base + ((size_t)first << EDEN_HEAP_PIECE_SHIFT), size);
    if (done == 0) {
      /* Kept as it was (dev-settings heap_return=off, or nothing to give back): usable again. */
      atomic_store_explicit(&eden_heap_space_retired[slot], 0, memory_order_seq_cst);
      continue;
    }
    for (unsigned piece = first; piece < first + span; ++piece) {
      atomic_store_explicit(&eden_heap_piece_space[piece], NULL, memory_order_release);
      atomic_store_explicit(&eden_heap_piece_state[piece], done > 0 ? 1 : 2, memory_order_relaxed);
    }
    eden_heap_holes_drop((int)slot);
    eden_heap_space_span[slot] = 0;
    atomic_store_explicit(&eden_heap_space_blocks[slot], 0, memory_order_relaxed);
    atomic_store_explicit(&eden_heap_spaces[slot], NULL, memory_order_release);
    atomic_fetch_sub_explicit(&eden_heap_committed_bytes, size - in_holes, memory_order_relaxed);
    if (done > 0)
      returned += size - in_holes;
  }
  /* Extra arenas with no block: their slots go back (and are taken again by a later arena). */
  for (unsigned slot = eden_heap_arena_floor; slot <= eden_heap_arena_top && slot < EDEN_HEAP_SLOTS; ++slot) {
    struct eden_heap_arena *arena = &eden_heap_arenas[slot];
    if (atomic_load_explicit(&arena->space, memory_order_relaxed) == NULL ||
        !atomic_load_explicit(&arena->extra, memory_order_relaxed))
      continue;
    const long empty = (long)atomic_load_explicit(&arena->holes, memory_order_relaxed);
    if (atomic_load_explicit(&arena->blocks, memory_order_relaxed) != empty)
      continue;
    atomic_store_explicit(&arena->retired, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&arena->users, memory_order_seq_cst) != 0 ||
        atomic_load_explicit(&arena->blocks, memory_order_acquire) != empty) {
      atomic_store_explicit(&arena->retired, 0, memory_order_seq_cst);
      continue;
    }
    const int owner = -(int)slot - 1;
    const size_t in_holes = eden_heap_holes_in(owner);
    const int done = eden_heap_decommit((void *)eden_heap_slot_address(slot), EDEN_HEAP_ARENA_SIZE);
    if (done == 0) {
      atomic_store_explicit(&arena->retired, 0, memory_order_seq_cst);
      continue;
    }
    eden_heap_holes_drop(owner);
    atomic_fetch_add_explicit(&arena->generation, 1, memory_order_release);
    atomic_store_explicit(&arena->lost, done < 0, memory_order_relaxed);
    atomic_store_explicit(&arena->space, NULL, memory_order_release);
    atomic_fetch_sub_explicit(&eden_heap_arena_count, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&eden_heap_arena_bytes, EDEN_HEAP_ARENA_SIZE, memory_order_relaxed);
    atomic_fetch_sub_explicit(&eden_heap_committed_bytes, EDEN_HEAP_ARENA_SIZE - in_holes, memory_order_relaxed);
    if (done > 0)
      returned += EDEN_HEAP_ARENA_SIZE - in_holes;
  }
  /* The lowest slots free again: the pieces may grow into them. */
  while (eden_heap_arena_floor <= eden_heap_arena_top &&
         atomic_load_explicit(&eden_heap_arenas[eden_heap_arena_floor].space, memory_order_relaxed) == NULL &&
         !atomic_load_explicit(&eden_heap_arenas[eden_heap_arena_floor].lost, memory_order_relaxed))
    ++eden_heap_arena_floor;
  if (holes) {
    /* The large pages of the free runs in what is left, while the heap keeps its slack: the
     * spaces' runs from 64 MiB down, an arena's of 4 MiB (two pages fit in 8 MiB at most). */
    static const size_t space_sizes[] = {(size_t)64 << 20, (size_t)32 << 20, (size_t)16 << 20,
                                         (size_t)8 << 20, (size_t)4 << 20, (size_t)3 << 20};
    static const size_t arena_sizes[] = {(size_t)4 << 20};
    int stop = 0;
    size_t made = 0;
    for (unsigned slot = 0; slot < count && !stop; ++slot) {
      void *space = atomic_load_explicit(&eden_heap_spaces[slot], memory_order_acquire);
      if (space != NULL && eden_heap_space_span[slot] != 0 &&
          !atomic_load_explicit(&eden_heap_space_retired[slot], memory_order_relaxed))
        made += eden_heap_holes_make((int)slot, space, space_sizes, 6u, &stop);
    }
    for (unsigned slot = eden_heap_arena_floor; slot <= eden_heap_arena_top && slot < EDEN_HEAP_SLOTS && !stop;
         ++slot) {
      void *space = atomic_load_explicit(&eden_heap_arenas[slot].space, memory_order_acquire);
      if (space != NULL && !atomic_load_explicit(&eden_heap_arenas[slot].retired, memory_order_relaxed))
        made += eden_heap_holes_make(-(int)slot - 1, space, arena_sizes, 1u, &stop);
    }
    returned += made;
  }
  eden_heap_grow_release();
  eden_heap_create_release();
  atomic_fetch_add_explicit(&eden_heap_returned_bytes, returned, memory_order_relaxed);
  return returned;
}

/* Everything empty given back, and the free runs of what is left made holes. */
size_t eden_heap_trim(void) {
  return eden_heap_trim_level(1);
}

/* A block from the heap's spaces, then from a new one. A small request starts at the space that
 * served the last one (it is asked for millions of times: compiled-code records); a request of
 * 1 MiB or more starts at the first space, which keeps the later ones free of long-lived blocks
 * and able to take the next large request. */
/* One request to one slot's space, unless the slot is retired. A thread counts itself in the
 * slot first and then looks at the flag; eden_heap_trim sets the flag first and then looks at the
 * count (both sequentially consistent): a space is given back only when no thread is in it, and
 * none can enter it once it is retired. */
static void *eden_heap_space_use(unsigned slot, int kind, size_t a, size_t b) {
  atomic_fetch_add_explicit(&eden_heap_space_users[slot], 1, memory_order_seq_cst);
  void *address = NULL;
  if (atomic_load_explicit(&eden_heap_space_retired[slot], memory_order_seq_cst) == 0) {
    void *space = atomic_load_explicit(&eden_heap_spaces[slot], memory_order_acquire);
    address = space != NULL ? eden_heap_space_try(space, kind, a, b) : NULL;
    if (address != NULL)
      atomic_fetch_add_explicit(&eden_heap_space_blocks[slot], 1, memory_order_relaxed);
  }
  atomic_fetch_sub_explicit(&eden_heap_space_users[slot], 1, memory_order_release);
  return address;
}

static void *eden_heap_main(int kind, size_t a, size_t b) {
  const size_t bytes = kind == EDEN_HEAP_MALLOC ? a : kind == EDEN_HEAP_CALLOC ? a * b : a + b;
  for (;;) {
    const unsigned generation = atomic_load_explicit(&eden_heap_space_generation, memory_order_acquire);
    const unsigned count = atomic_load_explicit(&eden_heap_space_count, memory_order_acquire);
    const int small = bytes < ((size_t)1 << 20);
    const unsigned hint = small ? atomic_load_explicit(&eden_heap_space_hint, memory_order_relaxed) : 0;
    for (unsigned i = 0; i < count; ++i) {
      const unsigned index = (hint + i) % count;
      void *address = eden_heap_space_use(index, kind, a, b);
      if (address != NULL) {
        if (small && i != 0)
          atomic_store_explicit(&eden_heap_space_hint, index, memory_order_relaxed);
        return address;
      }
    }
    if (bytes == 0 || !eden_heap_grow(bytes, generation))
      return NULL;
  }
}

/* ---- arenas ---- */

/* Makes one arena: in a slot of its own at the top of the range (in a block of the heap's first
 * space when the heap was taken whole). `first`: a thread's first arena (at most
 * EDEN_HEAP_FIRST_ARENAS; they stay, and threads share them at the limit), else an extra one,
 * given back when it holds no block. Returns its slot, or -1 (at a limit, or no memory). */
static int eden_heap_create_arena(int first) {
  eden_heap_create_acquire();
  int made = -1;
  const unsigned count = atomic_load_explicit(&eden_heap_arena_count, memory_order_relaxed);
  const unsigned firsts = atomic_load_explicit(&eden_heap_first_arenas, memory_order_relaxed);
  if (count < EDEN_HEAP_MAX_ARENAS && (!first || firsts < EDEN_HEAP_FIRST_ARENAS)) {
    void *block = NULL;
    int slot = -1;
    if (eden_heap_grows) {
      eden_heap_grow_acquire();
      slot = eden_heap_arena_slot();
      eden_heap_grow_release();
      if (slot >= 0)
        block = (void *)eden_heap_slot_address((unsigned)slot);
    } else {
      block = eden_heap_main(EDEN_HEAP_ALIGNED, EDEN_HEAP_ARENA_SIZE, EDEN_HEAP_ARENA_SIZE);
      if (block != NULL)
        slot = (int)eden_heap_slot(block);
    }
    if (block != NULL) {
      void *arena = sceLibcMspaceCreate("Eden-arena", block, EDEN_HEAP_ARENA_SIZE, 0);
      struct eden_heap_arena *record = &eden_heap_arenas[slot];
      if (arena != NULL) {
        /* Extra arenas are given back only where they have slots of their own. */
        const int extra = !first && eden_heap_grows;
        atomic_store_explicit(&record->blocks, 0, memory_order_relaxed);
        atomic_store_explicit(&record->holes, 0, memory_order_relaxed);
        atomic_store_explicit(&record->extra, (unsigned char)extra, memory_order_relaxed);
        atomic_store_explicit(&record->space, arena, memory_order_release);
        atomic_store_explicit(&record->retired, 0, memory_order_seq_cst);
        atomic_fetch_add_explicit(&eden_heap_arena_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&eden_heap_arenas_made, 1, memory_order_relaxed);
        if (eden_heap_grows) {
          atomic_fetch_add_explicit(&eden_heap_arena_bytes, EDEN_HEAP_ARENA_SIZE, memory_order_relaxed);
          atomic_fetch_add_explicit(&eden_heap_committed_bytes, EDEN_HEAP_ARENA_SIZE, memory_order_relaxed);
        }
        if (first) {
          eden_heap_first_slots[firsts] = (unsigned)slot;
          atomic_store_explicit(&eden_heap_first_arenas, firsts + 1, memory_order_release);
        }
        made = slot;
      } else if (eden_heap_grows) {
        if (eden_heap_decommit(block, EDEN_HEAP_ARENA_SIZE) <= 0)
          atomic_store_explicit(&record->lost, 1, memory_order_relaxed);
      } else {
        eden_heap_space_free(block);
      }
    }
  }
  eden_heap_create_release();
  return made;
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

/* An arena the record took is still that arena: not given back since (and its slot not made
 * into another). */
static int eden_heap_arena_alive(unsigned slot, unsigned generation) {
  return atomic_load_explicit(&eden_heap_arenas[slot].generation, memory_order_acquire) == generation &&
         atomic_load_explicit(&eden_heap_arenas[slot].space, memory_order_acquire) != NULL;
}

static void eden_heap_use_arena(struct eden_heap_thread *self, unsigned slot) {
  self->current = slot + 1u;
  self->current_generation = atomic_load_explicit(&eden_heap_arenas[slot].generation, memory_order_acquire);
}

/* This thread's arena (its slot), made on its first small allocation; -1: the heap's own spaces. */
static int eden_heap_thread_arena(struct eden_heap_thread *self) {
  if (self == NULL)
    return -1;
  if (self->current != 0) {
    if (eden_heap_arena_alive(self->current - 1u, self->current_generation))
      return (int)(self->current - 1u);
    self->current = 0;
  }
  /* One of its own still alive (the one it used was given back). */
  for (unsigned i = 0; i < self->owned;) {
    if (eden_heap_arena_alive(self->own[i].slot, self->own[i].generation)) {
      self->current = self->own[i].slot + 1u;
      self->current_generation = self->own[i].generation;
      return (int)self->own[i].slot;
    }
    self->own[i] = self->own[--self->owned];
  }
  int slot = eden_heap_create_arena(1);
  if (slot < 0) {
    /* Arena limit reached: share the first arenas round-robin. */
    const unsigned count = atomic_load_explicit(&eden_heap_first_arenas, memory_order_acquire);
    if (count == 0)
      return -1;
    slot = (int)eden_heap_first_slots[atomic_fetch_add_explicit(&eden_heap_next_shared, 1, memory_order_relaxed) %
                                      count];
    eden_heap_use_arena(self, (unsigned)slot);
    return slot;
  }
  eden_heap_use_arena(self, (unsigned)slot);
  if (self->owned < EDEN_HEAP_THREAD_ARENAS) {
    self->own[self->owned].slot = (unsigned)slot;
    self->own[self->owned].generation = self->current_generation;
    ++self->owned;
  }
  return slot;
}

/* One request to one arena. An extra arena counts the thread in first and then looks whether it
 * was given back (as eden_heap_space_use), and counts the block; a first arena stays. */
static void *eden_heap_arena_take(unsigned slot, int kind, size_t a, size_t b) {
  struct eden_heap_arena *arena = &eden_heap_arenas[slot];
  if (!atomic_load_explicit(&arena->extra, memory_order_relaxed)) {
    void *space = atomic_load_explicit(&arena->space, memory_order_acquire);
    return space != NULL ? eden_heap_space_try(space, kind, a, b) : NULL;
  }
  atomic_fetch_add_explicit(&arena->users, 1, memory_order_seq_cst);
  void *address = NULL;
  if (atomic_load_explicit(&arena->retired, memory_order_seq_cst) == 0) {
    void *space = atomic_load_explicit(&arena->space, memory_order_acquire);
    address = space != NULL ? eden_heap_space_try(space, kind, a, b) : NULL;
    if (address != NULL)
      atomic_fetch_add_explicit(&arena->blocks, 1, memory_order_relaxed);
  }
  atomic_fetch_sub_explicit(&arena->users, 1, memory_order_release);
  return address;
}

static void *eden_heap_arena_try(unsigned slot, size_t count, size_t size, int zero) {
  return zero ? eden_heap_arena_take(slot, EDEN_HEAP_CALLOC, count, size)
              : eden_heap_arena_take(slot, EDEN_HEAP_MALLOC, count * size, 0);
}

/* An arena's holes filled again, so it has its room back. */
static int eden_heap_arena_refill(unsigned slot) {
  if (atomic_load_explicit(&eden_heap_arenas[slot].holes, memory_order_relaxed) == 0)
    return 0;
  eden_heap_grow_acquire();
  const int filled = eden_heap_holes_fill(-(int)slot - 1, 0);
  eden_heap_grow_release();
  return filled;
}

/* A small block from this thread's arenas: the one it uses (its holes filled again when it is
 * full); when that one is full, another of its own with room, which it then uses; when none has, a
 * new one (for a request under EDEN_HEAP_ARENA_GROW_REQUEST, while the thread and the process are
 * under their limits). NULL: the heap's own spaces take the request. */
static void *eden_heap_arena_allocate(size_t count, size_t size, int zero) {
  struct eden_heap_thread *self = eden_heap_self();
  const int slot = eden_heap_thread_arena(self);
  if (slot < 0)
    return NULL;
  void *address = eden_heap_arena_try((unsigned)slot, count, size, zero);
  if (address != NULL)
    return address;
  if (self->retry != 0) {
    --self->retry;
    return NULL;
  }
  if (eden_heap_arena_refill((unsigned)slot) &&
      (address = eden_heap_arena_try((unsigned)slot, count, size, zero)) != NULL)
    return address;
  for (unsigned i = 0; i < self->owned;) {
    const unsigned other = self->own[i].slot;
    if (!eden_heap_arena_alive(other, self->own[i].generation)) {
      self->own[i] = self->own[--self->owned];
      continue;
    }
    ++i;
    if ((int)other == slot)
      continue;
    address = eden_heap_arena_try(other, count, size, zero);
    if (address == NULL && eden_heap_arena_refill(other))
      address = eden_heap_arena_try(other, count, size, zero);
    if (address != NULL) {
      eden_heap_use_arena(self, other);
      return address;
    }
  }
  if (count * size >= EDEN_HEAP_ARENA_GROW_REQUEST)
    return NULL;
  const int fresh = self->owned < EDEN_HEAP_THREAD_ARENAS ? eden_heap_create_arena(0) : -1;
  if (fresh < 0) {
    /* At a limit, or no memory for one: each look through the arenas costs a request to each,
     * so the next few requests go straight to the heap's spaces. */
    self->retry = EDEN_HEAP_ARENA_RETRY;
    return NULL;
  }
  eden_heap_use_arena(self, (unsigned)fresh);
  self->own[self->owned].slot = (unsigned)fresh;
  self->own[self->owned].generation = self->current_generation;
  ++self->owned;
  return eden_heap_arena_try((unsigned)fresh, count, size, zero);
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
  void *arena = eden_heap_arena_of(address);
  if (arena != NULL) {
    eden_heap_arena_free(arena, address);
    return;
  }
  const unsigned long long start = eden_heap_cycles();
  eden_heap_space_free(address);
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

/* Keeps a freed owned block of this usable size; 0 when the bin is full or out of range. Only a
 * block of an arena that stays (a first one) or of the arena the thread uses now is kept: a block
 * held here keeps its arena or space from being given back, and the GPU thread frees thousands of
 * blocks the shader cache's builders made in arenas of their own. */
static int eden_tcache_keep(void *address, size_t usable) {
  const size_t bin = usable >> 4;
  if (bin == 0 || bin >= EDEN_TCACHE_BINS)
    return 0;
  struct eden_heap_thread *self = eden_heap_self();
  if (self == NULL || self->counts[bin] >= EDEN_TCACHE_LIMIT)
    return 0;
  const uintptr_t slot = eden_heap_slot(address);
  if (atomic_load_explicit(&eden_heap_arenas[slot].space, memory_order_relaxed) == NULL ||
      (atomic_load_explicit(&eden_heap_arenas[slot].extra, memory_order_relaxed) && self->current != slot + 1u))
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
    /* In place, in its own arena or space (which holds it, so neither is given back meanwhile):
     * the block count stays. */
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
    eden_heap_free_owned(address);
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
    const int slot = eden_heap_thread_arena(eden_heap_self());
    if (slot >= 0) {
      void *block = eden_heap_arena_take((unsigned)slot, EDEN_HEAP_ALIGNED, alignment, size);
      if (block != NULL) {
        *address = block;
        result = 0;
      }
    }
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

