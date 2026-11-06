// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: Tears of the Kingdom's runaway loop, bounded (main, build ID 277178B7DBA1B6D4).
//
// Every Tears of the Kingdom failure the port has a log of ends in one loop of the game's main
// module, at main+0x6E1404 (the freeze watchdog's EDEN_STUCK_CODE, v1.010.010):
//
//   main+0x6E13E8  ldr  w11, [x10]              ; count = src->count  (src + 0x58e0)
//                  cbz  w11, done
//                  ...                          ; x9 = src + 0x5aa0, x12 = dst + 0x2644, x11 = 0
//   main+0x6E1404  ldr  s2, [x9, x11, lsl #2]   ; loop: dst->v[i] = src->v[i] * s1 + s0
//                  fmadd s2, s2, s1, s0
//                  str  s2, [x12, x11, lsl #2]
//                  add  x11, x11, #1
//                  ldr  w13, [x10]              ; count read again every pass
//                  cmp  x11, x13
//                  b.lo loop
//   main+0x6E1420  done: ldr w8, [x8]; str w8, [x19, #0x2654] ...; blr through dst + 0x2af8
//
// dst's array holds 4 floats: the field right after it (dst + 0x2654) is stored after the loop.
// In two v1.010.010 sessions (61 and 84 minutes in) MainThread entered this loop with a count far
// above 4. Its stores ran past dst, through src and over src's own count (which then read as
// one of the floats it stores, about 940 million), and on through the heap: 650 million stores
// in 60 s, the watchdog's EDEN_STUCK core=0 thread=79 at 10 and 60 s. When it ended, the virtual
// call through dst + 0x2af8 (overwritten too) jumped to an unmapped address and the session
// ended. The same loop wrote 0.4-3.4 GB of new memory on the 8 GiB layout (v1.010.004), and
// v1.010.009's freeze (core 0 in the guest's code, thread 78) was very likely it as well.
//
// What puts a count above 4 into src is not known yet. Three of src's words (+0x4, +0x3c, +0x58e4)
// held other values again after the loop had overwritten them, with the GPU idle and every other
// thread waiting: most likely the GPU's copy, downloaded when the watchdog read them, so src may
// lie in memory the GPU's caches track. So the loop
// is bounded to the 4 floats dst holds, as it always is when the count is right, and the code
// says so to the emulator: when the count is above 4 it makes a system call (svcGetSystemTick,
// which only sets x0, dead here until `mov x0, x22`), and PhysicalCore reports it
// (EDEN_TOTK_LOOP_BOUNDED: the count, its address, whether the GPU's caches track that page, and
// the words around it), which is what names the writer next time.
//
//   main+0x6E13E8  ldr  w11, [x10]
//                  add  x9, x10, #0x1c0         ; src + 0x5aa0 (x10 = src + 0x58e0)
//                  mov  w12, #0x2644
//                  add  x12, x19, x12           ; dst + 0x2644
//                  cmp  w11, #4
//                  b.ls 1f
//                  svc  #0x1e                   ; count above 4: reported, then bounded
//                  mov  w11, #4
//               1: cbz  w11, done
//               2: ldr  s2, [x9], #4            ; the same stores, in the same order
//                  fmadd s2, s2, s1, s0
//                  str  s2, [x12], #4
//                  subs w11, w11, #1
//                  b.ne 2b
//   main+0x6E1420  done: (unchanged)
//
// x9-x13 are dead after the loop (the code after it reads x8, x19 and x22, then calls). The patch
// is made only to this build, only when all 32 words around it are as above, in the image the
// loader is about to map (like a mod's IPS patch); tools/check-totk-guard.py checks the words
// with an assembler and runs both loops where the host is AArch64.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Eden::TotkGuard {

inline constexpr std::uint8_t kBuildId[8] = {0x27, 0x71, 0x78, 0xB7, 0xDB, 0xA1, 0xB6, 0xD4};

// The words checked: main+0x6E13C4 to main+0x6E1440, as the watchdog read them on the console.
inline constexpr std::uint64_t kWindow = 0x6E13C4;
inline constexpr std::uint32_t kOriginal[32] = {
    0xbd263a62, 0xbd400542, 0x1f010043, 0xbd263e63, 0xbd400942, 0x528b1c0a, 0x8b0a012a, 0x1f010042,
    0xbd264262, 0xb940014b, 0x340001ab, 0x5284c88c, 0x528b540d, 0xaa1f03eb, 0x8b0c026c, 0x8b0d0129,
    0xbc6b7922, 0x1f010042, 0xbc2b7982, 0x9100056b, 0xb940014d, 0xeb0d017f, 0x54ffff43, 0xb9400108,
    0xb9265668, 0x52855f08, 0x8b080276, 0xf9557e68, 0xaa1603e0, 0xf9400908, 0xd63f0100, 0x36000140,
};

// The words replaced: main+0x6E13E8 to main+0x6E141C (words 9 to 22 of the window).
inline constexpr std::uint64_t kPatchAt = 0x6E13E8;
inline constexpr std::uint32_t kPatched[14] = {
    0xb940014b, // ldr  w11, [x10]
    0x91070149, // add  x9, x10, #0x1c0
    0x5284c88c, // mov  w12, #0x2644
    0x8b0c026c, // add  x12, x19, x12
    0x7100117f, // cmp  w11, #4
    0x54000069, // b.ls +12
    0xd40003c1, // svc  #0x1e
    0x5280008b, // mov  w11, #4
    0x340000cb, // cbz  w11, +24 (main+0x6E1420)
    0xbc404522, // ldr  s2, [x9], #4
    0x1f010042, // fmadd s2, s2, s1, s0
    0xbc004582, // str  s2, [x12], #4
    0x7100056b, // subs w11, w11, #1
    0x54ffff81, // b.ne -16
};
inline constexpr std::size_t kPatchWord = (kPatchAt - kWindow) / 4;
static_assert(kPatchWord + sizeof(kPatched) / 4 <= sizeof(kOriginal) / 4);

// The system call the bounded loop makes, and pc after it (where the JIT stops for it).
inline constexpr unsigned kTrapSvc = 0x1E;
inline constexpr std::uint64_t kTrapReturn = kPatchAt + 7 * 4;
inline constexpr std::uint32_t kMost = 4;

// Where this session's patched main module lies (0: not patched). Function-local static: inline
// variables of one header do not initialize in order on the PS5.
inline std::atomic<std::uint64_t>& PatchedBase() noexcept {
    static std::atomic<std::uint64_t> base{0};
    return base;
}

inline std::uint32_t Word(const std::uint8_t* at) noexcept {
    return std::uint32_t(at[0]) | std::uint32_t(at[1]) << 8 | std::uint32_t(at[2]) << 16 | std::uint32_t(at[3]) << 24;
}
inline void PutWord(std::uint8_t* at, std::uint32_t word) noexcept {
    for (int i = 0; i < 4; ++i) at[i] = std::uint8_t(word >> (8 * i));
}

enum class Applied { kOtherBuild, kCodeDiffers, kPatched };

// The main module's image as the loader is about to map it at `base` (image[0] is main+0):
// patched when it is this build with these words. Every main module loaded calls it, so a session
// of another game, or of another build, is not armed.
inline Applied Apply(std::uint8_t* image, std::size_t size, const std::uint8_t* build_id, std::uint64_t base) noexcept {
    PatchedBase().store(0, std::memory_order_release);
    if (build_id == nullptr || std::memcmp(build_id, kBuildId, sizeof(kBuildId)) != 0) return Applied::kOtherBuild;
    if (image == nullptr || size < kWindow + sizeof(kOriginal)) return Applied::kCodeDiffers;
    for (std::size_t i = 0; i < sizeof(kOriginal) / 4; ++i)
        if (Word(image + kWindow + 4 * i) != kOriginal[i]) return Applied::kCodeDiffers;
    for (std::size_t i = 0; i < sizeof(kPatched) / 4; ++i) PutWord(image + kPatchAt + 4 * i, kPatched[i]);
    PatchedBase().store(base, std::memory_order_release);
    return Applied::kPatched;
}

// The loader's line for it (stderr.log).
inline void ReportApplied(Applied applied, std::uint64_t base) noexcept {
    if (applied == Applied::kPatched)
        std::fprintf(stderr,
                     "[ProsperoEden] TOTK: the loop at main+0x6E1404 (%#llx) stores at most %u floats, as its "
                     "destination holds; a count above that is reported (EDEN_TOTK_LOOP_BOUNDED)\n",
                     static_cast<unsigned long long>(base + 0x6E1404), kMost);
    else if (applied == Applied::kCodeDiffers)
        std::fprintf(stderr, "[ProsperoEden] TOTK: main+0x6E1404 is not the loop this fix knows; left as it is\n");
}

// What the bounded loop's system call found (headless/guest_sample.h reads it from the thread).
struct Trap {
    std::uint32_t count = 0;        // src->count, as the loop read it
    std::uint64_t count_at = 0;     // its address (x10)
    std::uint64_t src = 0;          // src->v (x9)
    std::uint64_t dst = 0;          // the destination object (x19)
    std::uint64_t thread = 0;
    const char* page = "?";         // the count's page: memory, gpu-cached, debug or unmapped
    bool words_read = false;
    std::uint32_t words[16] = {};   // count_at - 0x20 to count_at + 0x20, read past the GPU's caches
};

// One line for the first 8 and then for every power of two: a count that stays wrong is not a
// line a frame.
inline void Report(const Trap& trap) noexcept {
    static std::atomic<std::uint64_t> seen{0};
    const std::uint64_t n = seen.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 8 && (n & (n - 1)) != 0) return;
    char line[640];
    int at = std::snprintf(line, sizeof(line),
                           "EDEN_TOTK_LOOP_BOUNDED n=%llu count=%u (stored %u) count_at=%#llx page=%s src=%#llx dst=%#llx "
                           "thread=%llu words_from=%#llx:",
                           static_cast<unsigned long long>(n), trap.count, kMost,
                           static_cast<unsigned long long>(trap.count_at), trap.page ? trap.page : "?",
                           static_cast<unsigned long long>(trap.src), static_cast<unsigned long long>(trap.dst),
                           static_cast<unsigned long long>(trap.thread),
                           static_cast<unsigned long long>(trap.count_at - 0x20));
    for (unsigned i = 0; i < 16 && at > 0 && at < int(sizeof(line)); ++i)
        at += trap.words_read ? std::snprintf(line + at, sizeof(line) - at, " %08x", trap.words[i])
                              : std::snprintf(line + at, sizeof(line) - at, " ?");
    if (at > 0 && at < int(sizeof(line)) - 1) {
        line[at] = '\n';
        line[at + 1] = '\0';
    } else {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }
    std::fputs(line, stderr);
    std::fflush(stderr);
}

} // namespace Eden::TotkGuard
