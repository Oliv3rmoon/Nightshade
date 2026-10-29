#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a synchronous request's caller waits for its reply (headless/ipc_wait.h).

Compiles the header with a CMIF client that reads replies the way a game does, on the message of
IAudioOut GetReleasedAudioOutBuffersAuto (command 8, as the console log shows it):
- a caller that runs before the reply is written reads its own request, "SFCI" where "SFCO"
  belongs, and gets 2010-0212 (0x1A80A, InvalidCmifOutHeader); Eden's reply reads as success;
- only the request's own reply, or a failure, ends the wait; a reply to another request, or on
  another host thread, does not;
- a wait queue with a stray success wake before each reply: the pinned rule (any success wake ends
  the wait) stops the game every time, the new one never.
With a build directory (the host build's or the native one's), also that the derived
k_server_session.cpp carries the wait (headless/CMakeLists.txt).
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
cmake = (root / 'headless/CMakeLists.txt').read_text()
assert 'A synchronous request waits for its reply (headless/ipc_wait.h)' in cmake
assert '"${PORT_BUILD_DIR}/k_server_session.cpp"' in cmake

if len(sys.argv) > 1:
    derived = (Path(sys.argv[1]) / 'headless/k_server_session.cpp').read_text()
    assert 'using ThreadQueueImplForKServerSessionRequest = KThreadQueue;' not in derived
    assert 'class ThreadQueueImplForKServerSessionRequest final : public KThreadQueue {' in derived
    assert derived.count('::Eden::IpcWait::EndsWait(m_request, R_SUCCEEDED(wait_result))') == 1
    assert derived.count('ThreadQueueImplForKServerSessionRequest wait_queue{kernel, request};') == 1
    assert derived.count('const ::Eden::IpcWait::ScopedReply reply{request};') == 1
    assert derived.count('for (bool reported = false; !wait_queue.Ended(); reported = true) {') == 1
    assert derived.count('return eden_waiter.GetWaitResult();') == 1
    assert derived.count('KScheduler::SetSchedulerUpdateNeeded(kernel);') == 1

code = r'''
#include "ipc_wait.h"
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace Eden;
using Message = std::array<std::uint32_t, 64>;
constexpr std::uint32_t kRequestMagic = 0x49434653, kReplyMagic = 0x4F434653;
constexpr std::uint32_t kInvalidOutHeader = 0x1A80A; // 2010-0212
constexpr std::uint32_t kSessionClosed = 0xF601;

// The client's message for IAudioOut command 8: one C descriptor for the released buffers.
void WriteRequest(Message& m) {
    m.fill(0);
    m[0] = 4;                                 // type Request
    m[1] = 8 | 2u << 10;                      // 8 data words, one C descriptor
    m[4] = kRequestMagic; m[5] = 1; m[6] = 8; // padding to 16 bytes, header, command 8
    m[10] = 0x10000000; m[11] = 0x10 << 16;   // the C descriptor after the data
}
// Eden's reply (the console ring: header 00000000:0000000c, SFCO at word 4, result 0).
void WriteReply(Message& m, std::uint32_t released) {
    m.fill(0);
    m[1] = 0xc;
    m[4] = kReplyMagic; m[6] = 0; m[8] = released;
}
// The client's reply parsing: the CMIF header at the first 16-byte boundary of the data.
std::uint32_t ParseReply(const Message& m, std::uint32_t* released) {
    if (m[4] != kReplyMagic) return kInvalidOutHeader;
    if (m[6]) return m[6];
    *released = m[8];
    return 0;
}

// One caller's wait for one request, with the queue rule of k_server_session.cpp.
struct Wait {
    std::mutex lock;
    std::condition_variable wake;
    bool ended = false;
    bool succeeded = false;
    bool pinned_rule = false; // upstream Eden: any success wake ends the wait
    const void* request = nullptr;
    // Whether this wake ended the wait.
    bool EndWait(bool success) {
        std::lock_guard guard{lock};
        if (ended) return false;
        if (!pinned_rule && !IpcWait::EndsWait(request, success)) {
            IpcWait::Refused().fetch_add(1);
            return false;
        }
        ended = true;
        succeeded = success;
        wake.notify_all();
        return true;
    }
};

// Each round: the caller sends its request; another thread wakes it with success (what a stray
// wake does) before the server writes the reply and replies. Returns how many rounds the
// caller read something that is not a reply.
int Rounds(bool pinned_rule, int rounds) {
    int bad = 0;
    for (int round = 0; round < rounds; ++round) {
        Message tls;
        WriteRequest(tls);
        Wait wait;
        wait.pinned_rule = pinned_rule;
        int request_object = 0;
        wait.request = &request_object;
        std::mutex parsed_lock;
        std::condition_variable parsed_wake;
        bool parsed = false;
        std::uint32_t result = 0, released = 0;
        std::thread caller([&] {
            {
                std::unique_lock guard{wait.lock};
                wait.wake.wait(guard, [&] { return wait.ended; });
            }
            const std::uint32_t parsed_result = wait.succeeded ? ParseReply(tls, &released) : kSessionClosed;
            std::lock_guard guard{parsed_lock};
            result = parsed_result;
            parsed = true;
            parsed_wake.notify_all();
        });
        bool ran_on = false;
        std::thread stray([&] { ran_on = wait.EndWait(true); });
        stray.join();
        if (ran_on) {
            // The caller runs on and reads its message before the reply is written.
            std::unique_lock guard{parsed_lock};
            parsed_wake.wait(guard, [&] { return parsed; });
        }
        {
            std::lock_guard guard{parsed_lock};
            if (!parsed) WriteReply(tls, 1);
        }
        {
            const IpcWait::ScopedReply reply{&request_object};
            wait.EndWait(true);
        }
        caller.join();
        if (result != 0) {
            assert(result == kInvalidOutHeader);
            ++bad;
        } else {
            assert(released == 1);
        }
    }
    return bad;
}

int main() {
    // The bad layout: before the reply, word 4 is still the request's header.
    Message tls;
    WriteRequest(tls);
    std::uint32_t released = 0;
    assert(tls[4] == kRequestMagic);
    assert(ParseReply(tls, &released) == kInvalidOutHeader);
    WriteReply(tls, 2);
    assert(ParseReply(tls, &released) == 0 && released == 2);
    std::printf("IPC reply wait: caller before the reply reads word 4 = 0x%08x (SFCI) -> 0x%x; after Eden's reply 0x%x\n",
                kRequestMagic, kInvalidOutHeader, 0u);

    // Which wakes end the wait.
    int request = 0, other = 0;
    assert(IpcWait::EndsWait(&request, false));  // session closed, termination: always
    assert(!IpcWait::EndsWait(&request, true));  // success from anything but the reply: never
    {
        const IpcWait::ScopedReply reply{&request};
        assert(IpcWait::EndsWait(&request, true));
        assert(!IpcWait::EndsWait(&other, true));
        {
            const IpcWait::ScopedReply nested{&other};
            assert(IpcWait::EndsWait(&other, true) && !IpcWait::EndsWait(&request, true));
        }
        assert(IpcWait::EndsWait(&request, true));
        bool elsewhere = true;
        std::thread([&] { elsewhere = IpcWait::EndsWait(&request, true); }).join();
        assert(!elsewhere);  // a reply belongs to the host thread that sends it
    }
    assert(!IpcWait::EndsWait(&request, true));

    // The race: a stray success wake before every reply.
    constexpr int kRounds = 200;
    const int pinned_bad = Rounds(true, kRounds);
    IpcWait::Refused().store(0);
    const int fixed_bad = Rounds(false, kRounds);
    std::printf("IPC reply wait: stray wake before the reply, %d rounds: pinned rule %d x 2010-0212, reply wait %d (%u wakes refused)\n",
                kRounds, pinned_bad, fixed_bad, IpcWait::Refused().load());
    assert(pinned_bad == kRounds);
    assert(fixed_bad == 0 && IpcWait::Refused().load() == kRounds);
    int reported = 0;
    for (int i = 0; i < 20; ++i) reported += IpcWait::Report(IpcWait::RanOn());
    assert(reported == 16);
    std::puts("IPC reply wait: PASS only the request's reply or a failure ends the wait; the bad reply is the caller's own request");
}
'''

with tempfile.TemporaryDirectory() as scratch:
    source = Path(scratch) / 'ipc_reply_wait_check.cpp'
    binary = Path(scratch) / 'ipc_reply_wait_check'
    source.write_text(code)
    compiler = os.environ.get('CXX', 'c++')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', f'-I{root / "headless"}', str(source), '-o', str(binary)],
                   check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode)
