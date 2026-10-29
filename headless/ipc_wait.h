// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A thread that sends a synchronous request waits until the server replies, as on Horizon: only the
// request's reply, or a failure (the session closed, the thread terminating), ends that wait. Eden
// lets the waiting thread run again on any wake that reports success, and the caller then reads its
// message buffer: still its own request when the reply is not written yet, so the game's CMIF client
// finds no "SFCO" header and stops with 2010-0212 (InvalidCmifOutHeader). Seen on the console in
// GetReleasedAudioOutBuffer and around save data (Top Racer, Fighting Force Collection): the game's
// last call before it stopped, SetTerminateResult, had not been recorded yet (headless/ipc_ring.h).
// Applied to KServerSession by headless/CMakeLists.txt ("A synchronous request waits for its reply").
#include <atomic>

namespace Eden::IpcWait {

// The request whose reply is ending its caller's wait on this host thread (KServerSession::SendReply).
inline const void*& Replying() {
    static thread_local const void* request = nullptr;
    return request;
}

class ScopedReply {
public:
    explicit ScopedReply(const void* request) : previous{Replying()} { Replying() = request; }
    ~ScopedReply() { Replying() = previous; }
    ScopedReply(const ScopedReply&) = delete;
    ScopedReply& operator=(const ScopedReply&) = delete;

private:
    const void* previous;
};

// Whether a wake ends the wait of a thread that sent `request`.
inline bool EndsWait(const void* request, bool succeeded) {
    return !succeeded || Replying() == request;
}

// Refused wakes and callers that ran on before their wait ended; each is logged for its first 16.
inline std::atomic<unsigned>& Refused() {
    static std::atomic<unsigned> count{0};
    return count;
}
inline std::atomic<unsigned>& RanOn() {
    static std::atomic<unsigned> count{0};
    return count;
}
inline bool Report(std::atomic<unsigned>& count) {
    return count.fetch_add(1, std::memory_order_relaxed) < 16;
}

} // namespace Eden::IpcWait
