#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run actual generated async texture queue/completion methods under ASAN/UBSAN.

Usage: check-async-lifetimes.py EDEN_SOURCE

The real Common::ThreadWorker, ScratchBuffer and reservation are used. GPU memory,
conversion and upload boundaries are controlled so an active conversion can be
paused during destruction. A checked mutex wraps std::mutex to expose unlock after
context free reliably under ASAN. Source-derived member order controls teardown.
This is a host extracted-source regression, not a PS5 application build.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def definition(text, signature):
    begin = text.index(signature)
    brace = text.index('{', begin)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[begin:end]


root = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='async-lifetimes-') as temporary:
    work = Path(temporary)
    port = work / 'port'
    port.mkdir()
    subprocess.run([sys.executable, '-B', str(root / 'tools/prepare-vulkan-port.py'),
                    str(source), str(port)], check=True, stdout=subprocess.DEVNULL)
    cache = (port / 'vulkan-cache/video_core/texture_cache/texture_cache.h').read_text()
    base_path = port / 'vulkan-cache/video_core/texture_cache/texture_cache_base.h'
    base = base_path.read_text() if base_path.exists() else (
        source / 'src/video_core/texture_cache/texture_cache_base.h').read_text()
    context = definition(base, 'struct AsyncDecodeContext {') + ';'
    assert context.index('Reservation payload;') < context.index('decoded_data;'), \
        'the reservation must outlive the context buffers'
    context = context.replace('std::mutex mutex;', 'ObservedMutex mutex;')
    worker = re.search(r'    Common::ThreadWorker texture_decode_worker\{.*?;', base, re.S).group()
    budget = '    ::Eden::AstcPolicy::DecodeBudget astc_decode_budget;'
    contexts = '    std::vector<std::unique_ptr<AsyncDecodeContext>> async_decodes;'
    assert base.index(budget) < base.index(contexts), 'budget must outlive context reservations'
    members = '\n'.join(sorted((worker, budget, contexts), key=base.index))
    methods = '\n\n'.join('template<class P>\n' + definition(cache, 'void TextureCache<P>::' + name) for name in (
        'QueueAsyncDecode(', 'TickAsyncDecode()'))
    declarations = '\n'.join(method.split('{', 1)[0].replace('TextureCache<P>::', '') + ';'
                             for method in (definition(cache, 'void TextureCache<P>::QueueAsyncDecode('),
                                            definition(cache, 'void TextureCache<P>::TickAsyncDecode()')))
    # Access only is widened so the fixture can observe the actual worker's stop
    # request before releasing its active task. Its queue/stop/join code is unchanged.
    worker_header = (source / 'src/common/thread_worker.h').read_text().replace('private:', 'public:')
    (work / 'actual_thread_worker.h').write_text(worker_header)
    code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <semaphore>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <sanitizer/asan_interface.h>
#include "actual_thread_worker.h"
#include "common/scratch_buffer.h"
#include "common/scope_exit.h"
#include "astc_decode_budget.h"
namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
void SetCurrentThreadToEfficiencyCores() {}
void SetCurrentThreadToBackgroundWork() {}
void SetCurrentThreadToAllCores() {}
}
thread_local const u8* converted_input = nullptr;
struct ObservedMutex {
    std::mutex native;
    bool locked = false;
    void lock() {
        native.lock();
        locked = true;
        if (converted_input) {
            // Actual QueueAsyncDecode must release its captured input before
            // taking the completion mutex and publishing the finished output.
            assert(__asan_address_is_poisoned(converted_input));
            converted_input = nullptr;
        }
    }
    void unlock() {
        // std::mutex's system-library unlock may not be ASAN-instrumented. This
        // access makes a unique_lock outliving its mutex an observable UAF.
        assert(locked);
        locked = false;
        native.unlock();
    }
};
using ImageId = unsigned;
struct BufferImageCopy { unsigned marker = 0x1234; };
namespace boost::container { template<class T, size_t> using small_vector = std::vector<T>; }
@CONTEXT@
constexpr unsigned Converted = 1, IsDecoding = 2;
namespace ImageFlagBits { constexpr auto Converted=::Converted, IsDecoding=::IsDecoding; }
bool True(unsigned n) { return n != 0; }
bool False(unsigned n) { return n == 0; }
#define UNIMPLEMENTED_IF(condition) assert(!(condition))
#define LOG_INFO(...)
struct Info { unsigned value = 0x5a; };
struct Image;
struct Stage { std::vector<u8> storage; std::span<u8> mapped_span; };
namespace Vulkan {
struct TextureCacheRuntime {
    unsigned uploads = 0, barriers = 0, chunks = 0;
    Stage UploadStagingBuffer(size_t n) {
        Stage stage{std::vector<u8>(n), {}};
        stage.mapped_span = stage.storage;
        return stage;
    }
    void InsertUploadMemoryBarrier() { ++barriers; }
};
}
inline bool chunked = false;
inline Eden::AstcPolicy::DecodeBudget* uploading_budget = nullptr;
struct Image {
    unsigned flags = Converted, gpu_addr = 0, guest_size_bytes = 32, unswizzled_size_bytes = 32;
    Info info;
    Vulkan::TextureCacheRuntime* runtime = nullptr;
    template<class Staging> void UploadMemory(Staging& staging, const std::vector<BufferImageCopy>& copies) {
        assert(uploading_budget && uploading_budget->Held() == 4096);
        assert(copies.size() == 1 && copies[0].marker == 0x1234);
        for (u8 byte : staging.mapped_span) assert(byte == 0x5a);
        ++runtime->uploads;
    }
};
unsigned MapSizeBytes(const Image& image) { return image.unswizzled_size_bytes; }
template<class T> const T& FixSmallVectorADL(const T& value) { return value; }
namespace Tegra::Memory {
enum class GuestMemoryFlags { UnsafeRead };
template<class T, GuestMemoryFlags> struct GpuGuestMemory {
    GpuGuestMemory(int&, unsigned, unsigned, Common::ScratchBuffer<u8>*) {}
};
}
template<class Memory> std::vector<BufferImageCopy> UnswizzleImage(
    int&, unsigned, const Info& info, Memory&, std::vector<u8>& output) {
    std::fill(output.begin(), output.end(), u8(info.value));
    return {{}};
}
std::binary_semaphore conversion_started{0}, conversion_release{0};
std::atomic<bool> block_conversion{false};
std::atomic<unsigned> conversions{};
void ConvertImage(std::span<const u8> input, const Info&, std::span<u8> output,
                  std::span<BufferImageCopy>) {
    ++conversions;
    if (block_conversion.exchange(false)) {
        conversion_started.release();
        conversion_release.acquire();
    }
    assert(input.size() == output.size());
    std::copy(input.begin(), input.end(), output.begin());
    converted_input = input.data();
}
namespace Eden::StagingChunks {
bool Chunked(size_t) { return chunked; }
void Said(size_t) {}
template<class Runtime> struct Host { std::span<u8> mapped_span; Runtime* runtime; };
template<class Staging> void UploadStaged(Image& image, Staging& staging,
                                        const std::vector<BufferImageCopy>& copies) {
    ++staging.runtime->chunks;
    image.UploadMemory(staging, copies);
}
}
template<class P> struct TextureCache {
    using Runtime = P;
    Runtime runtime;
    int guest = 0;
    int* gpu_memory = &guest;
    Common::ScratchBuffer<u8> swizzle_data_buffer;
    std::vector<Image> slot_images;
    @DECLARATIONS@
    @MEMBERS@
};
@METHODS@
using Cache = TextureCache<Vulkan::TextureCacheRuntime>;
void Queue(Cache& cache, unsigned id) {
    auto reservation = cache.astc_decode_budget.Reserve(4096, false);
    assert(reservation);
    cache.QueueAsyncDecode(cache.slot_images[id], id, std::move(*reservation));
}
void Completion(bool use_chunks, bool initially_pending) {
    Cache cache;
    cache.slot_images.resize(1);
    cache.slot_images[0].runtime = &cache.runtime;
    chunked = use_chunks;
    uploading_budget = &cache.astc_decode_budget;
    block_conversion = initially_pending;
    Queue(cache, 0);
    if (initially_pending) {
        conversion_started.acquire();
        cache.TickAsyncDecode();
        assert(cache.async_decodes.size() == 1 && cache.runtime.uploads == 0);
        assert(cache.astc_decode_budget.Held() == 4096);
        assert(True(cache.slot_images[0].flags & IsDecoding));
        conversion_release.release();
    }
    cache.texture_decode_worker.WaitForRequests();
    assert(cache.async_decodes.size() == 1 && cache.astc_decode_budget.Held() == 4096);
    cache.TickAsyncDecode();
    assert(cache.runtime.uploads == 1 && cache.runtime.barriers == 1);
    assert(cache.runtime.chunks == unsigned(use_chunks));
    assert(cache.async_decodes.empty() && cache.astc_decode_budget.Held() == 0);
    assert(False(cache.slot_images[0].flags & IsDecoding));
    cache.TickAsyncDecode();
    assert(cache.runtime.uploads == 1 && cache.runtime.barriers == 1);
}
void Teardown() {
    auto cache = std::make_unique<Cache>();
    cache->slot_images.resize(129);
    for (auto& image : cache->slot_images) image.runtime = &cache->runtime;
    block_conversion = true;
    Queue(*cache, 0);
    conversion_started.acquire();
    for (unsigned i = 1; i < 129; ++i) Queue(*cache, i);
    assert(cache->astc_decode_budget.Held() == 129 * 4096);
    auto token = cache->texture_decode_worker.threads[0].get_stop_token();
    auto destroy = std::async(std::launch::async, [owned = std::move(cache)]() mutable { owned.reset(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!token.stop_requested()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    // The active callback may finish only once destruction has requested its
    // actual jthread stop. Context/output/budget must still be alive then.
    conversion_release.release();
    assert(destroy.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    destroy.get();
    assert(conversions == 1); // queued jobs were cancelled, rather than all drained
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    if (mode == "teardown") Teardown();
    else Completion(mode == "chunked", mode == "pending");
    std::printf("Async lifetime: PASS %s\n", mode.c_str());
}
'''
    for marker, value in (('@CONTEXT@', context), ('@DECLARATIONS@', declarations),
                          ('@MEMBERS@', members), ('@METHODS@', methods)):
        code = code.replace(marker, value)
    (work / 'check.cpp').write_text(code)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-g', '-pthread',
                    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-fno-omit-frame-pointer', '-I' + str(work),
                    '-I' + str(root / 'headless'), '-I' + str(source / 'src'),
                    str(work / 'check.cpp'), '-o', str(work / 'check')], check=True)
    failed = []
    for mode in ('completion', 'chunked', 'pending', 'teardown'):
        result = subprocess.run([str(work / 'check'), mode], capture_output=True, text=True, timeout=15)
        if result.returncode:
            failed.append(mode)
            sys.stderr.write(f'FAIL {mode} (exit {result.returncode})\n{result.stderr[-6500:]}')
        else:
            sys.stdout.write(result.stdout)
    assert not failed, 'actual-source async lifetime failures: ' + ', '.join(failed)
print('Async lifetimes: PASS actual generated completion, pending, chunked upload and active/queued teardown under ASAN/UBSAN')
