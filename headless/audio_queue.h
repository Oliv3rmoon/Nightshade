// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Eden::AudioQueue {
// A sink stream keeps its PCM in one ring (64 Ki samples, about 0.68 s of stereo) and the
// buffers it came in as in a queue beside it. A stream the renderer does not wait on (its
// second session's, and AudioOut's) can be given PCM faster than the hardware plays it, and
// the ring fills: the build stopped the game there (issue 270). Make room for `needed`
// samples by dropping the oldest audio, whole buffers at a time so the queue and the ring stay
// in step: first what is left of the buffer being played, then the oldest queued buffers.
// The caller excludes the consumer. Returns the frames dropped.
template <typename Ring, typename Queue, typename Buffer, typename Count>
std::uint64_t DropOldest(Ring& ring, std::uint64_t capacity, Queue& queue, Buffer& playing,
                         std::atomic<Count>& queued, std::uint64_t channels, std::uint64_t needed) {
    std::uint64_t dropped = 0;
    while (capacity - ring.Size() < needed) {
        if (!playing.consumed && playing.frames > playing.frames_played) {
            const std::uint64_t rest = playing.frames - playing.frames_played;
            ring.Pop(rest * channels);
            playing.frames_played = playing.frames;
            playing.consumed = true;
            dropped += rest;
            continue;
        }
        Buffer oldest{};
        if (!queue.TryPop(oldest)) {
            // No buffer accounts for what is left: the ring is out of step, so start it over.
            ring.Pop();
            break;
        }
        if (queued > 0) --queued;
        ring.Pop(oldest.frames * channels);
        dropped += oldest.frames;
    }
    return dropped;
}

// AudioOut sends 48 kHz stereo. Fade only padded frames for one millisecond,
// then use silence if the queue stays empty across more hardware blocks.
class UnderrunFade {
public:
    void Reset() noexcept {
        last_pcm = {};
        faded_frames = kFadeFrames;
    }

    void Apply(std::span<std::int16_t> stereo, std::size_t padded_frames) noexcept {
        constexpr std::size_t channels = 2;
        const std::size_t real_frames = stereo.size() / channels - padded_frames;
        if (real_frames != 0) {
            last_pcm = {stereo[(real_frames - 1) * channels], stereo[real_frames * channels - 1]};
            faded_frames = 0;
        }
        if (padded_frames == 0) {
            return;
        }

        auto padding = stereo.subspan(real_frames * channels);
        const std::size_t ramp_frames = std::min(padded_frames, kFadeFrames - faded_frames);
        for (std::size_t frame = 0; frame < ramp_frames; ++frame) {
            const auto gain = static_cast<std::int32_t>(kFadeFrames - ++faded_frames);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                padding[frame * channels + channel] = static_cast<std::int16_t>(
                    last_pcm[channel] * gain / static_cast<std::int32_t>(kFadeFrames));
            }
        }
        std::fill(padding.begin() + ramp_frames * channels, padding.end(), 0);
    }

private:
    static constexpr std::size_t kFadeFrames = 48;
    std::array<std::int16_t, 2> last_pcm{};
    std::size_t faded_frames = kFadeFrames;
};
} // namespace Eden::AudioQueue
