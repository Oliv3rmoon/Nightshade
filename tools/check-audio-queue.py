#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a full PCM ring drops its oldest audio instead of stopping the game (issue 270).

Usage: check-audio-queue.py BUILT_SINK_STREAM_CPP EDEN_SRC

The first argument is the sink_stream.cpp the build writes (headless/CMakeLists.txt, "full PCM
ring"), the second Eden's src folder. This checks that the build makes room with
headless/audio_queue.h and no longer throws, then compiles that header with Eden's own ring and
queue and feeds it more PCM than is played: the ring never overflows, the buffers left are the
newest, and each buffer's samples are still its own. It also checks the generated output
callback's underrun counts and the native output fade applied to padded frames.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
built = Path(sys.argv[1]).read_text()
src = Path(sys.argv[2])
append = built[built.index('void SinkStream::AppendBuffer('):]
append = append[:append.index('\n}\n')]
assert 'Audio PCM queue capacity exceeded' not in built, 'the build still stops the game on a full queue'
assert 'Eden::AudioQueue::DropOldest(samples_buffer, 0x10000, queue, playing_buffer, queued_buffers' in append
assert append.index('DropOldest(') < append.index('samples_buffer.Push('), 'room must be made before the push'
assert '#include "audio_queue.h"' in built
render_begin = built.index(' SinkStream::ProcessAudioOutAndRender(')
render_begin = built.rindex('\n', 0, render_begin) + 1
render_end = built.index('\n}\n', render_begin) + len('\n}')
render = built[render_begin:render_end]
native = (root / 'headless/audio.cpp').read_text()
assert (native.index('ProcessAudioOutAndRender(block, kAudioOutGrain)') <
        native.index('underrun_fade.Apply(block, padded_frames)') <
        native.index('sceAudioOutOutput(handle, block.data())'))
assert 'wake.wait(lock, [this] { return !in_flight; });\n    underrun_fade.Reset();' in native

code = r'''
#include "audio_queue.h"
#include <array>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "common/bounded_threadsafe_queue.h"
#include "common/common_types.h"
#include "common/ring_buffer.h"
struct SinkBuffer {
    u64 frames;
    u64 frames_played;
    u64 tag;
    bool consumed;
};
constexpr std::size_t kCapacity = 0x10000;
constexpr u64 kChannels = 2;
struct Stream {
    Common::RingBuffer<s16, kCapacity> ring;
    Common::SPSCQueue<SinkBuffer, 0x10000> queue;
    SinkBuffer playing{};
    std::atomic<u32> queued{};
    std::deque<u64> tags;  // what the queue should hold, oldest first
    u64 dropped = 0;
    // SinkStream::AppendBuffer as the build writes it: room first, then the buffer.
    void Append(u64 frames, u64 tag) {
        const u64 needed = frames * kChannels;
        if (needed > kCapacity - ring.Size())
            dropped += Eden::AudioQueue::DropOldest(ring, kCapacity, queue, playing, queued, kChannels, needed);
        std::vector<s16> pcm(needed, s16(tag));
        const std::size_t pushed = ring.Push(pcm);
        assert(pushed == needed);  // never cut short
        queue.EmplaceWait(SinkBuffer{frames, 0, tag, false});
        ++queued;
        tags.push_back(tag);
    }
    // ProcessAudioOutAndRender, a frame count at a time: every sample is its own buffer's.
    void Play(u64 frames) {
        while (frames) {
            if (playing.consumed || playing.frames == 0) {
                if (!queue.TryPop(playing)) return;
                --queued;
            }
            const u64 take = std::min(playing.frames - playing.frames_played, frames);
            std::vector<s16> out(take * kChannels, -1);
            const std::size_t popped = ring.Pop(out.data(), out.size());
            assert(popped == out.size());
            for (const s16 value : out) assert(value == s16(playing.tag));
            playing.frames_played += take;
            frames -= take;
            if (playing.frames_played >= playing.frames) playing.consumed = true;
        }
    }
    // The queue and the ring agree: the samples left are exactly the buffers left.
    void CheckInStep() const {
        u64 left = playing.consumed ? 0 : (playing.frames - playing.frames_played) * kChannels;
        assert(ring.Size() >= left);
        assert(ring.Size() % kChannels == 0);
        assert(queued.load() <= ring.Size() / kChannels);
    }
};

struct SinkStream {
    struct System {
        bool paused = false;
        bool shutting_down = false;
        bool IsPaused() const { return paused; }
        bool IsShuttingDown() const { return shutting_down; }
        const System& CoreTiming() const { return *this; }
        u64 GetGlobalTimeNs() const { return 0; }
    } system;
    std::mutex release_mutex, sample_count_lock;
    std::condition_variable release_cv;
    Common::RingBuffer<s16, kCapacity> samples_buffer;
    Common::SPSCQueue<SinkBuffer, 0x10000> queue;
    SinkBuffer playing_buffer{};
    std::atomic<u32> queued_buffers{};
    std::array<s16, kChannels> last_frame{};
    u64 last_sample_count_update_time = 0;
    u64 min_played_sample_count = 0;
    u64 max_played_sample_count = 0;

    std::size_t GetDeviceChannels() const { return kChannels; }
    std::size_t ProcessAudioOutAndRender(std::span<s16>, std::size_t);

    void QueueFrame(s16 left, s16 right) {
        std::array<s16, kChannels> pcm{left, right};
        assert(samples_buffer.Push(pcm) == pcm.size());
        queue.EmplaceWait(SinkBuffer{1, 0, 0, false});
        ++queued_buffers;
    }
};

// OUTPUT_CALLBACK

int main() {
    // Underrun counts are frames. The sink reports intentional silence separately
    // and supplies its held frame for the native output fade.
    {
        auto stream = std::make_unique<SinkStream>();
        constexpr std::size_t frames = 2;
        std::array<s16, frames * kChannels> pcm{};
        stream->QueueFrame(11, 12);
        stream->QueueFrame(21, 22);
        assert(stream->ProcessAudioOutAndRender(pcm, frames) == 0);
        assert((pcm == std::array<s16, 4>{11, 12, 21, 22}));
        assert(stream->ProcessAudioOutAndRender(pcm, frames) == frames);
        assert((pcm == std::array<s16, 4>{21, 22, 21, 22}));
        stream->QueueFrame(31, 32);
        assert(stream->ProcessAudioOutAndRender(pcm, frames) == 1);
        assert((pcm == std::array<s16, 4>{31, 32, 31, 32}));
        assert(stream->max_played_sample_count == 3);
        stream->system.paused = true;
        assert(stream->ProcessAudioOutAndRender(pcm, frames) == 0);
        assert((pcm == std::array<s16, 4>{}));
        stream->system.paused = false;
        stream->system.shutting_down = true;
        pcm.fill(99);
        assert(stream->ProcessAudioOutAndRender(pcm, frames) == 0);
        assert((pcm == std::array<s16, 4>{}));
        assert(stream->max_played_sample_count == 3);
    }
    // A renderer stream given PCM faster than it is played: 240-frame buffers, three appended for
    // every two played, for about a minute of audio.
    {
        auto stream = std::make_unique<Stream>();
        u64 tag = 1;
        for (int round = 0; round < 4000; ++round) {
            for (int i = 0; i < 3; ++i) stream->Append(240, tag++ % 30000 + 1);
            stream->Play(480);
            assert(stream->ring.Size() <= kCapacity);
            stream->CheckInStep();
        }
        assert(stream->dropped > 0);
        // What is queued is the newest audio: playing it out finds the last buffer appended.
        const u64 last = (tag - 1) % 30000 + 1;
        while (stream->queued > 0 || !stream->playing.consumed) {
            const u64 before = stream->ring.Size();
            stream->Play(240);
            if (stream->ring.Size() == before) break;
        }
        assert(stream->playing.tag == last);
        assert(stream->ring.Size() == 0);
    }
    // A buffer half played goes first, then whole queued buffers, oldest first.
    {
        auto stream = std::make_unique<Stream>();
        for (u64 tag = 1; tag <= 136; ++tag) stream->Append(240, tag);  // 65280 of 65536 samples
        stream->Play(120);  // buffer 1 is half played
        assert(stream->playing.tag == 1 && stream->queued == 135);
        stream->Append(400, 200);  // needs 800 of 496 free: what is left of buffer 1, then buffer 2
        assert(stream->dropped == 120 + 240);
        assert(stream->queued == 135);
        stream->Play(240);
        assert(stream->playing.tag == 3);  // buffer 2 was dropped whole
        stream->CheckInStep();
    }
    // An AudioOut buffer the size of the whole ring: everything before it goes.
    {
        auto stream = std::make_unique<Stream>();
        stream->Append(4096, 1);
        stream->Append(4096, 2);
        stream->Append(32768, 3);
        assert(stream->queued == 1 && stream->dropped == 8192 && stream->ring.Size() == kCapacity);
        stream->Play(32768);
        assert(stream->playing.tag == 3 && stream->ring.Size() == 0);
    }
    // Samples no buffer accounts for: the ring starts over rather than staying out of step.
    {
        auto stream = std::make_unique<Stream>();
        std::vector<s16> stray(kCapacity - 100, 7);
        stream->ring.Push(stray);
        stream->Append(240, 9);
        assert(stream->ring.Size() == 480 && stream->queued == 1);
        stream->Play(240);
        assert(stream->playing.tag == 9);
    }
    // Room already there: nothing is dropped.
    {
        auto stream = std::make_unique<Stream>();
        for (u64 tag = 1; tag <= 10; ++tag) stream->Append(240, tag);
        assert(stream->dropped == 0 && stream->queued == 10);
    }
    {
        auto stream = std::make_unique<SinkStream>();
        Eden::AudioQueue::UnderrunFade fade;
        std::array<s16, 128> empty_block{};
        empty_block.fill(99);
        fade.Apply(empty_block, 64);
        assert((empty_block == std::array<s16, 128>{}));

        stream->QueueFrame(-12000, 24000);
        stream->QueueFrame(24000, -12000);
        std::array<s16, 6> first{};
        const auto padded = stream->ProcessAudioOutAndRender(first, 3);
        assert(padded == 1);
        fade.Apply(first, padded);
        assert((first == std::array<s16, 6>{-12000, 24000, 24000, -12000, 23500, -11750}));

        const auto empty = stream->ProcessAudioOutAndRender(empty_block, 64);
        assert(empty == 64);
        fade.Apply(empty_block, empty);
        for (unsigned frame = 0; frame < 64; ++frame) {
            const int gain = frame < 46 ? 46 - static_cast<int>(frame) : 0;
            assert(empty_block[2 * frame] == 500 * gain);
            assert(empty_block[2 * frame + 1] == -250 * gain);
        }
        assert(stream->max_played_sample_count == 2);

        stream->QueueFrame(17000, -25000);
        std::array<s16, 2> resumed{};
        const auto resumed_padding = stream->ProcessAudioOutAndRender(resumed, 1);
        assert(resumed_padding == 0);
        fade.Apply(resumed, resumed_padding);
        assert((resumed == std::array<s16, 2>{17000, -25000}));

        fade.Reset();
        assert(stream->ProcessAudioOutAndRender(empty_block, 64) == 64);
        fade.Apply(empty_block, 64);
        assert((empty_block == std::array<s16, 128>{}));
    }
    std::puts("audio underrun counts and stereo fade across hardware blocks: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'audio-queue'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1',
                    '-I' + str(root / 'headless'), '-I' + str(src), '-x', 'c++', '-', '-o', str(exe)],
                   input=code.replace('// OUTPUT_CALLBACK', render), text=True, check=True)
    subprocess.run([str(exe)], check=True)
print('Audio queue: PASS a full PCM ring drops its oldest audio, in step with its buffers, and plays on')
