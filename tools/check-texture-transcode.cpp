// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

// Link with Eden's astc.cpp, bcn.cpp and stb_dxt.cpp. Keep the production worker
// queue and codec implementations, and substitute only the platform thread hooks.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <string_view>
#include <vector>

#include "video_core/textures/astc.h"
#include "video_core/textures/bcn.h"
#include "video_core/textures/workers.h"

namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadToAllCores() {}
void SetCurrentThreadToEfficiencyCores() {}
void SetCurrentThreadToBackgroundWork() {}
} // namespace Common

Common::ThreadWorker& Tegra::Texture::GetThreadWorkers() {
    static Common::ThreadWorker workers{4, "test transcode"};
    return workers;
}

namespace {
using Clock = std::chrono::steady_clock;

struct Texture {
    Texture(u32 width, u32 height, u32 astc_width, u32 astc_height, u32 depth, bool bc1)
        : width(width), height(height), astc_width(astc_width), astc_height(astc_height),
          depth(depth), bc1(bc1),
          input(size_t{(width + astc_width - 1) / astc_width} *
                ((height + astc_height - 1) / astc_height) * depth * 16),
          rgba(size_t{width} * height * depth * 4),
          output(size_t{(width + 3) / 4} * ((height + 3) / 4) * depth * (bc1 ? 8 : 16)) {
        for (size_t offset = 0; offset < input.size(); offset += 16) {
            // Valid LDR constant-color ASTC blocks with varied channels and alpha.
            std::fill_n(input.data() + offset, 8, 0xff);
            input[offset] = 0xfc;
            input[offset + 1] = 0xfd;
            for (unsigned channel = 0; channel < 4; ++channel) {
                const auto color = static_cast<u8>((offset / 16) * (1 + 8 * channel) + 23 * channel);
                input[offset + 8 + 2 * channel] = color;
                input[offset + 9 + 2 * channel] = color;
            }
        }
    }

    void Decode() {
        Tegra::Texture::ASTC::Decompress(input, width, height, depth, astc_width, astc_height, rgba);
    }

    void Compress() {
        auto compress = bc1 ? Tegra::Texture::BCN::CompressBC1 : Tegra::Texture::BCN::CompressBC3;
        compress(rgba, width, height, depth, output);
    }

    void Transcode() {
        const auto input_size = input.size() / depth;
        const auto rgba_size = rgba.size() / depth;
        const auto output_size = output.size() / depth;
        for (u32 layer = 0; layer < depth; ++layer) {
            Tegra::Texture::BCN::TranscodeASTCLayer(
                std::span{input}.subspan(layer * input_size, input_size), width, height, astc_width,
                astc_height, bc1, std::span{rgba}.subspan(layer * rgba_size, rgba_size),
                std::span{output}.subspan(layer * output_size, output_size));
        }
    }

    u32 width, height, astc_width, astc_height, depth;
    bool bc1;
    std::vector<u8> input, rgba, output;
};

u64 Checksum(const std::vector<u8>& data) {
    u64 sum = 14695981039346656037ULL;
    for (const auto byte : data) {
        sum = (sum ^ byte) * 1099511628211ULL;
    }
    return sum;
}

template <typename Work>
bool CompletesIndependently(Work work) {
    using namespace std::chrono_literals;
    std::promise<void> entered, release;
    auto gate = release.get_future();
    auto& workers = Tegra::Texture::GetThreadWorkers();
    workers.QueueWork([&] {
        entered.set_value();
        gate.wait();
    });
    entered.get_future().wait();
    auto completed = std::async(std::launch::async, work);
    const bool independent = completed.wait_for(2s) == std::future_status::ready;
    release.set_value();
    completed.get();
    workers.WaitForRequests();
    return independent;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--wait-isolation") {
        Texture texture{129, 131, 5, 4, 1, false};
        const bool decode = CompletesIndependently([&] { texture.Decode(); });
        const bool compress = CompletesIndependently([&] { texture.Compress(); });
        const bool transcode = CompletesIndependently([&] { texture.Transcode(); });
        std::printf("ASTC own-work completion=%d, BC3 own-work completion=%d, row transcode completion=%d\n",
                    decode, compress, transcode);
        return decode && compress && transcode ? 0 : 1;
    }
    const std::string_view mode = argc == 2 ? argv[1] : "";
    if (mode == "--benchmark" || mode == "--benchmark-two-stage") {
        Clock::duration elapsed{};
        u64 hash = 0;
        for (unsigned size = 1024; size != 0; size /= 2) {
            Texture texture{size, size, 4, 4, 1, false};
            const auto start = Clock::now();
            for (unsigned layer = 0; layer < 121; ++layer) {
                if (mode == "--benchmark") {
                    texture.Transcode();
                } else {
                    texture.Decode();
                    texture.Compress();
                }
            }
            elapsed += Clock::now() - start;
            hash = hash * 1099511628211ULL ^ Checksum(texture.output);
        }
        std::printf("1024x1024, 121 layers, 11 mips, ASTC 4x4 to BC3: %.3f ms, checksum=%016llx\n",
                    std::chrono::duration<double, std::milli>(elapsed).count(),
                    static_cast<unsigned long long>(hash));
        return 0;
    }
    std::ofstream bytes;
    if (argc == 3 && std::string_view(argv[1]) == "--bytes") {
        bytes.open(argv[2], std::ios::binary);
        if (!bytes) {
            return 2;
        }
    }
    for (auto texture : {Texture{1, 1, 4, 4, 2, false}, Texture{7, 13, 5, 4, 3, true},
                        Texture{127, 129, 8, 8, 3, false}, Texture{257, 261, 4, 4, 2, true},
                        Texture{79, 91, 8, 5, 3, false}, Texture{31, 35, 6, 6, 2, true}}) {
        texture.Decode();
        texture.Compress();
        const auto decoded = texture.rgba;
        const auto compressed = texture.output;
        std::fill(texture.rgba.begin(), texture.rgba.end(), 0);
        std::fill(texture.output.begin(), texture.output.end(), 0);
        texture.Transcode();
        assert(texture.rgba == decoded && texture.output == compressed);
        assert(texture.rgba[0] == 0 && texture.rgba[1] == 23 &&
               texture.rgba[2] == 46 && texture.rgba[3] == 69);
        std::printf("%ux%ux%u ASTC %ux%u RGBA=%016llx BC%d=%016llx\n", texture.width, texture.height,
                    texture.depth, texture.astc_width, texture.astc_height,
                    static_cast<unsigned long long>(Checksum(texture.rgba)), texture.bc1 ? 1 : 3,
                    static_cast<unsigned long long>(Checksum(texture.output)));
        if (bytes.is_open()) {
            bytes.write(reinterpret_cast<const char*>(texture.rgba.data()), texture.rgba.size());
            bytes.write(reinterpret_cast<const char*>(texture.output.data()), texture.output.size());
        }
    }
    if (bytes.is_open()) {
        bytes.close();
        return bytes ? 0 : 2;
    }
}
