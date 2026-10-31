// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Eden PS5 port: large transfers staged in pieces (tools/prepare-vulkan-port.py, the texture and
// buffer caches).
//
// A staging buffer is one block of graphics memory, and the driver gives such a block one run of
// the pool the CPU shares with the GPU. Eden stages a whole transfer in one buffer: Tears of the
// Kingdom uploads a texture array of 121 layers (1024x1024, 11 levels; 161 MiB as BC3) whenever it
// comes back, in a staging buffer of 176 MiB. Its session on v1.010.006 ended there, 59 minutes in:
// "a staging buffer of 184549376 bytes was refused" with the pool's largest free block at 146 MiB
// and 951 MiB free in all. A transfer of more than kLimit is now staged at most kLimit at a time:
// its copies are cut into batches (a copy of many layers into runs of layers, a buffer range into
// pieces), each batch is copied into a staging buffer of its own and recorded before the next one
// is asked for. An image's data is laid out once in the heap (whose large blocks take memory in
// pieces when they must, src/memory_pages.cpp), and the GPU runs the copies in order, so what it
// sees is what one transfer gave. A staging buffer refused even after the pool's idle buffers and
// the heap's free memory went back is asked for again at half the size, down to kLeast.
// A single layer (or depth slice) larger than a batch stays one piece; no game measured has one.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Eden::StagingChunks {
// The most staged at once, and how far a refused batch is halved before the refusal goes on.
inline constexpr std::uint64_t kLimit = std::uint64_t{64} << 20;
inline constexpr std::uint64_t kLeast = std::uint64_t{4} << 20;
// Every copy starts at a multiple of this in its staging buffer (Vulkan wants a multiple of the
// texel block size and of 4).
inline constexpr std::uint64_t kAlign = 256;

constexpr std::uint64_t AlignUp(std::uint64_t value, std::uint64_t align) noexcept {
    return (value + align - 1) / align * align;
}

// Whether a transfer of `bytes` is staged in pieces.
constexpr bool Chunked(std::uint64_t bytes, std::uint64_t limit = kLimit) noexcept {
    return bytes > limit;
}

// ---- buffers ----

// A piece of a buffer transfer: `at` is where it is in the buffer, `staged` where it goes in its
// batch's staging buffer.
struct Piece {
    std::uint64_t at;
    std::uint64_t staged;
    std::uint64_t size;
};
// Where the next batch starts: the transfer, and how far into it.
struct Cursor {
    std::size_t index = 0;
    std::uint64_t done = 0;
};

// The next batch of transfers (offset in the buffer, size) given by `range(i)` for i in
// [0, count), from `cursor`, of at most `limit` staged bytes (at least one piece). Returns the
// bytes its staging buffer must hold (0: nothing is left), fills `pieces` and moves `cursor`.
template <class Range>
std::uint64_t NextBatch(std::size_t count, Range&& range, Cursor& cursor, std::uint64_t limit,
                        std::vector<Piece>& pieces) {
    pieces.clear();
    if (limit < kAlign) limit = kAlign;
    std::uint64_t used = 0;
    while (cursor.index < count) {
        const auto [offset, size] = range(cursor.index);
        if (cursor.done >= size) {
            ++cursor.index;
            cursor.done = 0;
            continue;
        }
        const std::uint64_t start = AlignUp(used, kAlign);
        if (start >= limit) break;
        const std::uint64_t part = (std::min)(size - cursor.done, limit - start);
        pieces.push_back(Piece{offset + cursor.done, start, part});
        used = start + part;
        cursor.done += part;
    }
    return used;
}

// Stages transfers in batches: `request(bytes)` gives a staging buffer of at least `bytes` (and
// throws when it is refused), `run(staging, pieces)` copies one batch. A refused request is asked
// for again with half the batch, down to kLeast; then the refusal goes on. Returns the batches.
template <class Range, class Request, class Run>
std::size_t StageBuffer(std::size_t count, Range&& range, Request&& request, Run&& run,
                        std::uint64_t limit = kLimit) {
    std::vector<Piece> pieces;
    Cursor cursor;
    std::size_t batches = 0;
    for (;;) {
        Cursor next = cursor;
        const std::uint64_t bytes = NextBatch(count, range, next, limit, pieces);
        if (bytes == 0) return batches;
        auto staging = [&] {
            try {
                return std::make_pair(request(bytes), true);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception&) {
                if (limit <= kLeast) throw;
                return std::make_pair(decltype(request(bytes)){}, false);
            }
        }();
        if (!staging.second) {
            limit /= 2;
            continue;
        }
        run(staging.first, static_cast<const std::vector<Piece>&>(pieces));
        cursor = next;
        ++batches;
    }
}

// ---- images ----

// One piece of an image transfer (Eden's BufferImageCopy: buffer_offset, buffer_size,
// image_subresource.base_layer / num_layers, image_offset.z, image_extent.depth) and where its data
// is in the transfer's own layout.
template <class Copy>
struct Unit {
    Copy copy;
    std::uint64_t source;
    std::size_t of; // the copy it came from
    int split;      // 0: the whole copy; 1: one of its layers; 2: one of its depth slices
};

// Cuts copies of more than `split` bytes into one unit per layer (per depth slice, for one layer
// of a 3D image); smaller copies are one unit each.
template <class Copy>
std::vector<Unit<Copy>> Units(const Copy* copies, std::size_t count, std::uint64_t split = kLeast) {
    std::vector<Unit<Copy>> units;
    for (std::size_t i = 0; i < count; ++i) {
        const Copy& copy = copies[i];
        const std::uint64_t bytes = copy.buffer_size;
        const bool layered = copy.image_subresource.num_layers > 1;
        const std::uint64_t parts = layered ? static_cast<std::uint64_t>(copy.image_subresource.num_layers)
                                            : static_cast<std::uint64_t>(copy.image_extent.depth);
        if (bytes <= split || parts <= 1 || bytes % parts != 0) {
            units.push_back(Unit<Copy>{copy, copy.buffer_offset, i, 0});
            continue;
        }
        const std::uint64_t each = bytes / parts;
        for (std::uint64_t k = 0; k < parts; ++k) {
            Copy part = copy;
            part.buffer_size = static_cast<decltype(part.buffer_size)>(each);
            if (layered) {
                part.image_subresource.base_layer += static_cast<decltype(part.image_subresource.base_layer)>(k);
                part.image_subresource.num_layers = 1;
            } else {
                part.image_offset.z += static_cast<decltype(part.image_offset.z)>(k);
                part.image_extent.depth = 1;
            }
            units.push_back(Unit<Copy>{part, copy.buffer_offset + k * each, i, layered ? 1 : 2});
        }
    }
    return units;
}

// The next batch of units from `at`, at most `limit` staged bytes (at least one unit): the units
// of one copy that follow each other become one copy again. Fills `copies` (buffer_offset where
// each goes in the staging buffer) and `sources` (where its data is), moves `at`, returns the bytes.
template <class Copy>
std::uint64_t NextImageBatch(const std::vector<Unit<Copy>>& units, std::size_t& at, std::uint64_t limit,
                             std::vector<Copy>& copies, std::vector<std::uint64_t>& sources) {
    copies.clear();
    sources.clear();
    std::uint64_t used = 0;
    for (; at < units.size(); ++at) {
        const Unit<Copy>& unit = units[at];
        const std::uint64_t size = unit.copy.buffer_size;
        const bool continues = unit.split != 0 && at > 0 && !copies.empty() && units[at - 1].of == unit.of;
        const std::uint64_t start = continues ? used : AlignUp(used, kAlign);
        if (!copies.empty() && start + size > limit) break;
        if (continues) {
            Copy& merged = copies.back();
            merged.buffer_size += unit.copy.buffer_size;
            if (unit.split == 1)
                merged.image_subresource.num_layers += 1;
            else
                merged.image_extent.depth += 1;
        } else {
            Copy copy = unit.copy;
            copy.buffer_offset = static_cast<decltype(copy.buffer_offset)>(start);
            copies.push_back(copy);
            sources.push_back(unit.source);
        }
        used = start + size;
    }
    return used;
}

// Stages an image transfer laid out in `data` in batches (as StageBuffer): `request(bytes)` gives a
// staging buffer with a `mapped_span`, `upload(staging, copies)` records the batch's copies.
template <class Copy, class Request, class Upload>
std::size_t StageImage(std::span<const std::uint8_t> data, const Copy* copies, std::size_t count,
                       Request&& request, Upload&& upload, std::uint64_t limit = kLimit) {
    const auto units = Units(copies, count);
    std::vector<Copy> batch;
    std::vector<std::uint64_t> sources;
    std::size_t at = 0, batches = 0;
    while (at < units.size()) {
        std::size_t next = at;
        const std::uint64_t bytes = NextImageBatch(units, next, limit, batch, sources);
        bool staged = false;
        decltype(request(bytes)) staging{};
        try {
            staging = request(bytes);
            staged = true;
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception&) {
            if (limit <= kLeast) throw;
        }
        if (!staged) {
            limit /= 2;
            continue;
        }
        for (std::size_t k = 0; k < batch.size(); ++k) {
            const std::uint64_t size = batch[k].buffer_size;
            if (sources[k] > data.size() || size > data.size() - sources[k]) continue; // not in the data: as Eden's
            std::memcpy(staging.mapped_span.data() + batch[k].buffer_offset, data.data() + sources[k], size);
        }
        upload(staging, std::span<const Copy>(batch.data(), batch.size()));
        at = next;
        ++batches;
    }
    return batches;
}

// A transfer's data laid out in the heap instead of a staging buffer: the texture cache writes it
// as it would a staging buffer's, and UploadStaged stages it in batches.
template <class Runtime>
struct Host {
    std::span<std::uint8_t> mapped_span;
    Runtime* runtime;
};

template <class T>
inline constexpr bool kIsHost = false;
template <class Runtime>
inline constexpr bool kIsHost<Host<Runtime>> = true;

// The 1st, 2nd, 4th ... transfer staged in pieces, in stderr.log.
inline void Said(std::uint64_t bytes) {
    static std::atomic<unsigned long long> times{0};
    const unsigned long long n = times.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0)
        std::fprintf(stderr, "[ProsperoEden] graphics memory: a transfer of %llu MiB staged in pieces of at most %llu MiB "
                             "(%llu so far)\n",
                     static_cast<unsigned long long>(bytes >> 20), static_cast<unsigned long long>(kLimit >> 20), n);
}

// Eden's upload of an image's copies, from a staging buffer as before or from the heap in batches.
template <class Image, class Staging, class Copies>
void UploadStaged(Image& image, Staging& staging, const Copies& copies) {
    image.UploadMemory(staging, copies);
}
// The copies' sizes, from where their data lies. The texture cache lays copies out one after
// another with nothing between them (UnswizzleImage, ConvertImage, the partial rows): a copy's
// bytes run to the next one's data. Its layers are cut by its size, and a size left at what the
// data was before a conversion (Eden's ConvertImage did so for decoded ASTC and BCn) would stage a
// quarter of a decoded texture. The last copy's runs to the end of the data when it says more,
// or less by a whole number of its layers (and slices); otherwise it stays.
template <class Copy>
std::vector<Copy> Sized(const Copy* copies, std::size_t count, std::uint64_t data_bytes) {
    std::vector<Copy> sized(copies, copies + count);
    bool in_order = true;
    for (std::size_t k = 0; k + 1 < count; ++k)
        in_order = in_order && copies[k + 1].buffer_offset > copies[k].buffer_offset;
    if (count == 0 || !in_order || std::uint64_t(copies[count - 1].buffer_offset) >= data_bytes) return sized;
    for (std::size_t k = 0; k + 1 < count; ++k)
        sized[k].buffer_size = static_cast<decltype(sized[k].buffer_size)>(copies[k + 1].buffer_offset - copies[k].buffer_offset);
    Copy& last = sized[count - 1];
    const std::uint64_t room = data_bytes - std::uint64_t(last.buffer_offset);
    const std::uint64_t units = std::uint64_t(last.image_subresource.num_layers > 0 ? last.image_subresource.num_layers : 1) *
                                std::uint64_t(last.image_extent.depth > 0 ? last.image_extent.depth : 1);
    if (std::uint64_t(last.buffer_size) > room || (std::uint64_t(last.buffer_size) < room && room % units == 0))
        last.buffer_size = static_cast<decltype(last.buffer_size)>(room);
    return sized;
}
template <class Image, class Runtime, class Copies>
void UploadStaged(Image& image, Host<Runtime>& host, const Copies& copies) {
    using Copy = std::remove_cv_t<std::remove_reference_t<decltype(*copies.data())>>;
    const auto sized = Sized<Copy>(copies.data(), copies.size(), host.mapped_span.size());
    StageImage<Copy>(std::span<const std::uint8_t>(host.mapped_span.data(), host.mapped_span.size()), sized.data(),
                     sized.size(), [&](std::uint64_t bytes) { return host.runtime->UploadStagingBuffer(bytes); },
                     [&](auto& staging, std::span<const Copy> batch) { image.UploadMemory(staging, batch); });
}
} // namespace Eden::StagingChunks
