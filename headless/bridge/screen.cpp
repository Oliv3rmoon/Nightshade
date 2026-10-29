// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// VideoOut as the PS5 Native App Boilerplate's demo uses it (src/demo_renderer.cpp, Copyright (C)
// 2026 BlackBearReloaded, GPL-3.0-or-later): its declarations, the memory it takes for two buffers,
// the buffers' tiled layout and the flip are its own, unchanged. The picture is drawn into ordinary
// memory first (picture.hpp): the buffers are write-combined, and reading them back to blend is slow.
#include "screen.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
std::size_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(std::int64_t search_start, std::int64_t search_end, std::size_t length,
                                  std::size_t alignment, int memory_type, std::int64_t* physical_address);
int sceKernelMapDirectMemory(void** address, std::size_t length, int protection, int flags,
                             std::int64_t physical_address, std::size_t alignment);
int sceVideoOutOpen(std::int32_t user_id, std::int32_t bus_type, std::int32_t index, const void* param);
int sceVideoOutSetFlipRate(std::int32_t handle, std::int32_t rate);
int sceVideoOutSubmitFlip(std::int32_t handle, std::int32_t buffer_index, std::uint32_t flip_mode,
                          std::int64_t flip_argument);
int sceVideoOutWaitVblank(std::int32_t handle);
}

namespace Eden::Bridge {
namespace {

constexpr std::size_t kFrameBytes = 0x1000000;
constexpr std::size_t kMemoryBytes = kFrameBytes * 2;
constexpr std::size_t kMemoryAlignment = 0x200000;
constexpr int kMemoryTypeWcGarlic = 3;
constexpr int kMapProtection = 0x33;
constexpr std::uint64_t kPixelFormatRgba8Srgb = UINT64_C(0x8000000022000000);

struct VideoBuffer {
    void* data;
    void* metadata;
    void* reserved0;
    void* reserved1;
};

struct VideoAttribute {
    std::uint8_t reserved[80];
};

extern "C" void sceVideoOutSetBufferAttribute2(VideoAttribute* attribute, std::uint64_t pixel_format,
                                               std::uint32_t tiling_mode, std::uint32_t width, std::uint32_t height,
                                               std::uint64_t option, std::uint32_t dcc_control,
                                               std::uint64_t dcc_clear_color);
extern "C" int sceVideoOutRegisterBuffers2(std::int32_t handle, std::int32_t set_index, std::int32_t buffer_index_start,
                                           VideoBuffer* buffers, std::int32_t buffer_count, VideoAttribute* attribute,
                                           std::int32_t category, void* option);

// Where pixel (x, y) is in a buffer: the display's tiling of 128 x 128 blocks of 64 KiB.
constexpr std::size_t TiledOffset(unsigned x, unsigned y) {
    const std::uint32_t offset = ((y << 4) & 0x70U) ^ ((y << 5) & 0xf00U) ^ ((y << 9) & 0x1000U) ^
                                 ((y << 8) & 0x4000U) ^ ((x << 2) & 0xcU) ^ ((x << 5) & 0x380U) ^
                                 ((x << 4) & 0x400U) ^ ((x << 6) & 0x800U) ^ ((x << 9) & 0xa000U);
    const std::uint32_t blocks_per_row = (static_cast<unsigned>(Picture::kWidth) + 127U) >> 7;
    const std::uint32_t block_index = (y >> 7) * blocks_per_row + (x >> 7);
    return (static_cast<std::size_t>(block_index) << 16) + offset;
}

void Flush(void* address, std::size_t length) {
    auto* at = static_cast<std::uint8_t*>(address);
    const auto* end = at + length;
    for (; at < end; at += 64) __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

} // namespace

bool Screen::open() {
    const int video = sceVideoOutOpen(0xff, 0, 0, nullptr);
    if (video < 0) return false;
    const std::size_t pool = sceKernelGetDirectMemorySize();
    std::int64_t physical = 0;
    void* mapped = nullptr;
    if (pool < kMemoryBytes ||
        sceKernelAllocateDirectMemory(0, static_cast<std::int64_t>(pool), kMemoryBytes, kMemoryAlignment,
                                      kMemoryTypeWcGarlic, &physical) < 0 ||
        sceKernelMapDirectMemory(&mapped, kMemoryBytes, kMapProtection, 0, physical, kMemoryAlignment) < 0)
        return false;
    buffers_[0] = static_cast<std::uint8_t*>(mapped);
    buffers_[1] = buffers_[0] + kFrameBytes;
    std::memset(mapped, 0, kMemoryBytes);
    Flush(mapped, kMemoryBytes);
    VideoBuffer buffers[2] = {{buffers_[0], nullptr, nullptr, nullptr}, {buffers_[1], nullptr, nullptr, nullptr}};
    VideoAttribute attribute{};
    (void)sceVideoOutSetFlipRate(video, 0);
    sceVideoOutSetBufferAttribute2(&attribute, kPixelFormatRgba8Srgb, 0, Picture::kWidth, Picture::kHeight, 0, 0, 0);
    if (sceVideoOutRegisterBuffers2(video, 0, 0, buffers, 2, &attribute, 0, nullptr) < 0) return false;
    video_ = video;
    return true;
}

bool Screen::show(const Picture& picture) {
    if (video_ < 0) return false;
    const int next = shown_ == 0 ? 1 : 0;
    std::uint8_t* buffer = buffers_[next];
    for (unsigned y = 0; y < static_cast<unsigned>(Picture::kHeight); ++y) {
        const std::uint32_t* row = &picture.pixels()[static_cast<std::size_t>(y) * Picture::kWidth];
        for (unsigned x = 0; x < static_cast<unsigned>(Picture::kWidth); ++x)
            *reinterpret_cast<std::uint32_t*>(buffer + TiledOffset(x, y)) = row[x];
    }
    Flush(buffer, kFrameBytes);
    if (sceVideoOutSubmitFlip(video_, next, 1, 1) < 0) return false;
    // The buffer shown until now is free once the flip has happened, at the next refresh.
    (void)sceVideoOutWaitVblank(video_);
    shown_ = next;
    return true;
}

} // namespace Eden::Bridge
