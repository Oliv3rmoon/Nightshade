// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Guest panic recovery, panic evidence, and call-site inspection adapted from
// bf39l/ProsperoEden (Oliv3rmoon/Nightshade#6, GPL-3.0-or-later).
#pragma once
// A guest thread that jumps to an unmapped address takes a prefetch abort, or breaks with a fatal
// panic/assert/C++ exception, which Eden answers by parking that thread for a debugger; without
// one the game sits frozen. The first such fault of a session is recorded here so the frontend can
// end the session cleanly like a GPU failure (retrying transient early-boot prefetch faults, never
// deterministic guest panics).
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Eden {
inline std::mutex guest_fault_mutex;
inline std::string guest_fault;
inline void RecordGuestFault(std::string description) {
    std::lock_guard lock(guest_fault_mutex);
    if (guest_fault.empty()) guest_fault = std::move(description);
}

// Panics are terminal guest failures, not transient early-boot callback races.
inline bool GuestFaultMayRetry(const std::string& description) {
    return !description.starts_with("guest panic:");
}

inline void RecordGuestPanic(std::uint32_t reason, std::uint64_t thread) {
    char line[128];
    std::snprintf(line, sizeof(line), "guest panic: reason %#x on thread %llu", reason,
                  static_cast<unsigned long long>(thread));
    // The engine logger is asynchronous and teardown can strand its final messages.
    // Persist the terminal reason before publishing it to the frontend.
    std::fprintf(stderr, "EDEN_GUEST_PANIC %s\n", line);
    std::fflush(stderr);
    RecordGuestFault(line);
}

struct PanicContext {
    std::uint64_t pc, lr, sp, fp;
};

template <class Valid, class Read32, class Read64, class Emit>
inline void PanicEvidence(std::uint64_t info1, std::uint64_t info2, PanicContext ctx,
                          Valid valid, Read32 read32, Read64 read64, Emit emit) {
    char line[256];
    std::snprintf(line, sizeof(line),
                  "EDEN_GUEST_PANIC_CONTEXT pc=%#llx lr=%#llx sp=%#llx fp=%#llx info1=%#llx info2=%#llx",
                  static_cast<unsigned long long>(ctx.pc), static_cast<unsigned long long>(ctx.lr),
                  static_cast<unsigned long long>(ctx.sp), static_cast<unsigned long long>(ctx.fp),
                  static_cast<unsigned long long>(info1), static_cast<unsigned long long>(info2));
    emit(line);
    // Keep base + size strictly below 2^64 so Memory::IsValidVirtualAddressRange's
    // (end = base + size) loop cannot wrap end to 0 and skip page validation.
    if (info1 && info2 == 4 && info1 <= (std::numeric_limits<std::uint64_t>::max)() - 4 &&
        valid(info1, 4)) {
        std::snprintf(line, sizeof(line), "EDEN_GUEST_PANIC_RESULT value=%08x",
                      static_cast<unsigned>(read32(info1)));
        emit(line);
    }
    for (unsigned n = 0; n < 24; ++n) {
        if (!ctx.fp || (ctx.fp & 7) || ctx.fp < ctx.sp || ctx.fp - ctx.sp > 1024 * 1024 ||
            ctx.fp > (std::numeric_limits<std::uint64_t>::max)() - 16 || !valid(ctx.fp, 16)) {
            break;
        }
        const auto next = read64(ctx.fp);
        const auto lr = read64(ctx.fp + 8);
        std::snprintf(line, sizeof(line), "EDEN_GUEST_PANIC_FRAME index=%u fp=%#llx lr=%#llx",
                      n, static_cast<unsigned long long>(ctx.fp),
                      static_cast<unsigned long long>(lr));
        emit(line);
        if (next <= ctx.fp) break;
        ctx.fp = next;
    }
    emit("EDEN_GUEST_PANIC_CONTEXT_END");
}

inline std::string FaultLine(const std::string& thread_name, std::uint64_t thread_id,
                             std::uint64_t pc, std::uint64_t lr, std::uint64_t sp,
                             const std::vector<std::uint64_t>& calls) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "EDEN_GUEST_FAULT thread=%s tid=%llu pc=%#llx lr=%#llx sp=%#llx",
                  thread_name.c_str(), static_cast<unsigned long long>(thread_id),
                  static_cast<unsigned long long>(pc), static_cast<unsigned long long>(lr),
                  static_cast<unsigned long long>(sp));
    std::string out(buf);
    if (!calls.empty()) {
        out += " bt=";
        const std::size_t limit = calls.size() < 16 ? calls.size() : 16;
        for (std::size_t i = 0; i < limit; ++i) {
            std::snprintf(buf, sizeof(buf), "%s%#llx", i ? "," : "",
                          static_cast<unsigned long long>(calls[i]));
            out += buf;
        }
    }
    return out;
}

// Bounded, read-only evidence at the call site. Validate every word; never read the
// invalid branch target, dereference guest object pointers, or scan arbitrary memory.
template <class Valid, class Read, class Emit>
inline void FaultCallSite(std::uint64_t lr, Valid valid, Read read, Emit emit) {
    if (lr < 16 || (lr & 3) != 0 ||
        lr > (std::numeric_limits<std::uint64_t>::max)() - 16) {
        return;
    }
    for (int offset = -16; offset < 16; offset += 4) {
        const std::uint64_t address = offset < 0 ? lr - static_cast<std::uint64_t>(-offset)
                                                 : lr + static_cast<std::uint64_t>(offset);
        char line[128];
        if (!valid(address, 4)) {
            std::snprintf(line, sizeof(line), "EDEN_GUEST_CALL_SITE address=%#llx unmapped",
                          static_cast<unsigned long long>(address));
        } else {
            const auto word = read(address);
            std::snprintf(line, sizeof(line),
                          "EDEN_GUEST_CALL_SITE address=%#llx instruction=%08x%s",
                          static_cast<unsigned long long>(address),
                          static_cast<unsigned>(word),
                          offset == -4 ? " before_lr=1" : "");
        }
        emit(line);
    }
}

inline std::string TakeGuestFault() {
    std::lock_guard lock(guest_fault_mutex);
    return std::exchange(guest_fault, {});
}
} // namespace Eden
