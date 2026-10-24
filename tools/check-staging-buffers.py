#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the staging buffer sizes and the second request for a refused one (#195).

Usage: check-staging-buffers.py EDEN_SRC [BUILT_STAGING_BUFFER_POOL_CPP]

EDEN_SRC is Eden's src folder at the pinned revision. This applies the replacements
tools/prepare-vulkan-port.py makes to its renderer_vulkan/vk_staging_buffer_pool.cpp, exactly as
the build does (each must match once), and checks that the file the build wrote is that one when
it is given. Then it compiles the result with Eden's own header and its common/ helpers, and with
stand-ins for the allocator (a pool of fixed size that refuses what does not fit), the scheduler
and the device, and runs it under ASan and UBSan:

- up to 16 MiB, and for buffers only the GPU uses, a staging buffer is Eden's power of two; above
  16 MiB a buffer the CPU writes or reads is its transfer rounded up to 16 MiB (130 MiB: 144, not
  256);
- a kept buffer is handed out only when it holds the request, and an idle kept buffer too small
  for a new one goes before the new one is made;
- a refused buffer is asked for once more after every idle kept buffer is let go, buffers the GPU
  still uses and deferred ones stay, and a second refusal or another error goes on to the caller.

Eden's own file runs the same way as the control: for a transfer of 130 MiB it asks for the
268435456-byte buffer with usage 0x8f3 that #195's log shows refused, and it does not ask again.
"""
from pathlib import Path
import ast, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
src = Path(sys.argv[1])
relative = 'src/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp'
original = (src / relative.removeprefix('src/')).read_text()

# The replacements, read out of the port script's adapt() call for this file.
script = (root / 'tools/prepare-vulkan-port.py').read_text()
calls = [node for node in ast.walk(ast.parse(script)) if isinstance(node, ast.Call) and
         getattr(node.func, 'id', None) == 'adapt' and isinstance(node.args[0], ast.Constant) and
         node.args[0].value == relative]
assert len(calls) == 1, 'one adapt() call for the staging buffer pool'
assert calls[0].args[1].value == 'vulkan_staging_buffer_pool.cpp'
replacements = eval(compile(ast.Expression(calls[0].args[2]), 'prepare-vulkan-port.py', 'eval'),
                    {'port': root / 'headless'})
patched = original
for old, new in replacements:
    assert patched.count(old) == 1, f'pinned anchor must match once: {old[:60]!r}'
    patched = patched.replace(old, new)
if len(sys.argv) > 2:
    assert Path(sys.argv[2]).read_text() == patched, 'the built staging buffer pool is not the patched pinned one'
cmake = (root / 'headless/vulkan.cmake').read_text()
assert 'list(REMOVE_ITEM video_sources renderer_vulkan/vk_staging_buffer_pool.cpp)' in cmake
assert '"${PORT_BUILD_DIR}/vulkan_staging_buffer_pool.cpp"' in cmake
assert 'check-staging-buffers.py' in (root / 'tools/build-headless-native.sh').read_text()

# Holds() takes a mapped buffer's size from its mapping, and an unmapped one's as Eden's power of
# two. That holds while Eden maps the Upload and Download kinds, and only those, with their size.
allocator = (src / 'video_core/vulkan_common/vulkan_memory_allocator.cpp').read_text()
flags = allocator[allocator.index('VmaAllocationCreateFlags MemoryUsageVmaFlags('):]
flags = flags[:flags.index('\n    }\n')]
assert flags.count('VMA_ALLOCATION_CREATE_MAPPED_BIT') == 2
assert flags.index('case MemoryUsage::Upload:') < flags.index('VMA_ALLOCATION_CREATE_MAPPED_BIT')
assert flags.index('case MemoryUsage::Download:') < flags.rindex('VMA_ALLOCATION_CREATE_MAPPED_BIT')
assert flags.rindex('VMA_ALLOCATION_CREATE_MAPPED_BIT') < flags.index('case MemoryUsage::DeviceLocal:\n                return {};')
assert allocator.count('std::span<u8>{data, ci.size}') == 2

stubs = {
    'fmt/ranges.h': r'''
#pragma once
#include <string>
namespace fmt { template <class... A> std::string format(const char*, A&&...) { return {}; } }
''',
    'common/assert.h': r'''
#pragma once
#include <cstdio>
#include <cstdlib>
#define ASSERT(cond) do { if (!(cond)) { std::fprintf(stderr, "ASSERT %s (%s:%d)\n", #cond, __FILE__, __LINE__); std::abort(); } } while (0)
#define ASSERT_MSG(cond, ...) ASSERT(cond)
''',
    # The Vulkan names the pool uses, with the values of the Vulkan specification.
    'video_core/vulkan_common/vulkan_wrapper.h': r'''
#pragma once
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include "common/common_types.h"
using VkFlags = std::uint32_t;
using VkDeviceSize = std::uint64_t;
using VkDeviceAddress = std::uint64_t;
using VkBuffer = struct VkBuffer_T*;
enum VkResult { VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2 };
enum VkStructureType { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO = 12 };
enum VkSharingMode { VK_SHARING_MODE_EXCLUSIVE = 0 };
enum VkBufferUsageFlagBits : VkFlags {
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 0x1, VK_BUFFER_USAGE_TRANSFER_DST_BIT = 0x2,
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT = 0x10, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT = 0x20,
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT = 0x40, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT = 0x80,
    VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT = 0x800,
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT = 0x20000,
};
using VkBufferUsageFlags = VkFlags;
struct VkBufferCreateInfo {
    VkStructureType sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkBufferUsageFlags usage;
    VkSharingMode sharingMode; std::uint32_t queueFamilyIndexCount; const std::uint32_t* pQueueFamilyIndices;
};
struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; };
namespace Vulkan::vk {
class Exception final : public std::exception {
public:
    explicit Exception(VkResult result_) : result{result_} {}
    VkResult GetResult() const noexcept { return result; }
    const char* what() const noexcept override { return "VkResult"; }
private:
    VkResult result;
};
// A buffer of the check's pool, which takes it back when the buffer goes.
class Buffer {
public:
    using Give = void (*)(VkBuffer, VkDeviceSize, std::span<u8>);
    Buffer() = default;
    Buffer(VkBuffer handle_, VkDeviceSize size_, std::span<u8> mapped_, Give give_)
        : handle{handle_}, size{size_}, mapped{mapped_}, give{give_} {}
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& rhs) noexcept
        : handle{std::exchange(rhs.handle, nullptr)}, size{rhs.size}, mapped{rhs.mapped}, give{rhs.give} {}
    Buffer& operator=(Buffer&& rhs) noexcept {
        Release();
        handle = std::exchange(rhs.handle, nullptr);
        size = rhs.size;
        mapped = rhs.mapped;
        give = rhs.give;
        return *this;
    }
    ~Buffer() { Release(); }
    VkBuffer operator*() const noexcept { return handle; }
    std::span<u8> Mapped() noexcept { return mapped; }
    void SetObjectNameEXT(const char*) const {}
private:
    void Release() noexcept {
        if (handle != nullptr) give(handle, size, mapped);
        handle = nullptr;
    }
    VkBuffer handle{};
    VkDeviceSize size{};
    std::span<u8> mapped{};
    Give give{};
};
} // namespace Vulkan::vk
''',
    'video_core/vulkan_common/vulkan_memory_allocator.h': r'''
#pragma once
#include "video_core/vulkan_common/vulkan_wrapper.h"
namespace Vulkan {
class Device;
enum class MemoryUsage { DeviceLocal, Upload, Download, Stream };
class MemoryAllocator {
public:
    vk::Buffer CreateBuffer(const VkBufferCreateInfo& ci, MemoryUsage usage) const;
};
template <typename F> void ForEachDeviceLocalHostVisibleHeap(const Device&, F&&) {}
} // namespace Vulkan
''',
    # Eden's ticks: the one being recorded, and the last the GPU is known to have finished.
    'video_core/renderer_vulkan/vk_scheduler.h': r'''
#pragma once
#include "common/common_types.h"
namespace Vulkan {
class MasterSemaphore {
public:
    u64 KnownGpuTick() const noexcept { return known; }
    u64 known = 0;
};
class Scheduler {
public:
    u64 CurrentTick() const noexcept { return current; }
    bool IsFree(u64 tick) const noexcept { return semaphore.known >= tick; }
    MasterSemaphore& GetMasterSemaphore() const noexcept { return semaphore; }
    void Submit() noexcept { ++current; }                     // Flush: a new tick is recorded
    void Finish() noexcept { Submit(); semaphore.known = current - 1; }  // and the GPU ran it
    u64 current = 1;
    mutable MasterSemaphore semaphore;
};
} // namespace Vulkan
''',
    # The console's device: transform feedback, no buffer device address (#195's usage 0x8f3).
    'video_core/vulkan_common/vulkan_device.h': r'''
#pragma once
#include "video_core/vulkan_common/vulkan_wrapper.h"
namespace Vulkan {
struct Logical { VkDeviceAddress GetBufferDeviceAddress(VkBuffer) const { return 0; } };
class Device {
public:
    bool IsExtTransformFeedbackSupported() const noexcept { return true; }
    bool IsBufferDeviceAddressSupported() const noexcept { return false; }
    bool HasDebuggingToolAttached() const noexcept { return false; }
    Logical GetLogical() const noexcept { return {}; }
};
} // namespace Vulkan
''',
}

harness = r'''
#include <cassert>
#include <cstdio>
#include <deque>
#include <memory>
#include <random>
#include <sys/mman.h>
using namespace Vulkan;
constexpr VkDeviceSize MiB = 1ULL << 20;

// The pool the buffers come from: it refuses what does not fit, as the console's does.
struct Pool {
    VkDeviceSize capacity = ~0ULL, live = 0;
    unsigned attempts = 0, made = 0;
    VkResult refusal = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkDeviceSize last_size = 0;
    VkFlags last_usage = 0;
} pool;

static void Give(VkBuffer handle, VkDeviceSize size, std::span<u8> mapped) {
    pool.live -= size;
    if (!mapped.empty()) munmap(mapped.data(), size);
    delete reinterpret_cast<char*>(handle);
}

vk::Buffer MemoryAllocator::CreateBuffer(const VkBufferCreateInfo& ci, MemoryUsage usage) const {
    ++pool.attempts;
    if (ci.size > pool.capacity - pool.live) throw vk::Exception(pool.refusal);
    // As Eden's allocator: the Upload, Download and Stream kinds are mapped, with their size.
    std::span<u8> mapped;
    if (usage != MemoryUsage::DeviceLocal) {
        void* const data = mmap(nullptr, ci.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        assert(data != MAP_FAILED);
        mapped = std::span<u8>{static_cast<u8*>(data), ci.size};
    }
    ++pool.made;
    pool.live += ci.size;
    pool.last_size = ci.size;
    pool.last_usage = ci.usage;
    return vk::Buffer(reinterpret_cast<VkBuffer>(new char), ci.size, mapped, &Give);
}

// A pool of staging buffers over a pool of `room` bytes beside its stream buffer.
struct Rig {
    Device device;
    MemoryAllocator allocator;
    Scheduler scheduler;
    std::unique_ptr<StagingBufferPool> staging;
    VkDeviceSize stream = 0;
    explicit Rig(VkDeviceSize room = ~0ULL >> 1) {
        assert(pool.live == 0);
        pool = Pool{};
        staging = std::make_unique<StagingBufferPool>(device, allocator, scheduler);
        stream = pool.live;
        pool.capacity = stream + room;
    }
    // A request, and the size of the buffer it made (0: a kept one was handed out).
    VkDeviceSize Made(size_t size, MemoryUsage usage, bool deferred = false) {
        const unsigned before = pool.made;
        const StagingBufferRef ref = staging->Request(size, usage, deferred);
        if (usage != MemoryUsage::DeviceLocal) assert(ref.mapped_span.size() >= size);
        return pool.made == before ? 0 : pool.last_size;
    }
};
constexpr VkFlags kStagingUsage = 0x8f3;  // #195's refused buffer
'''

patched_main = r'''
int main() {
    // Sizes: Eden's power of two up to 16 MiB and for GPU-only buffers, 16 MiB steps above.
    struct Case { size_t size; MemoryUsage usage; VkDeviceSize made; };
    for (const Case c : {Case{9 * MiB, MemoryUsage::Download, 16 * MiB}, Case{16 * MiB, MemoryUsage::Download, 16 * MiB},
                         Case{4096, MemoryUsage::Download, 4096}, Case{3000, MemoryUsage::DeviceLocal, 4096},
                         Case{16 * MiB + 1, MemoryUsage::Download, 32 * MiB}, Case{33 * MiB, MemoryUsage::Download, 48 * MiB},
                         Case{130 * MiB, MemoryUsage::Upload, 144 * MiB}, Case{256 * MiB, MemoryUsage::Upload, 256 * MiB},
                         Case{257 * MiB, MemoryUsage::Download, 272 * MiB},
                         Case{130 * MiB, MemoryUsage::DeviceLocal, 256 * MiB}}) {
        Rig rig;
        assert(rig.Made(c.size, c.usage) == c.made);
        assert(pool.last_usage == kStagingUsage);
    }
    // Reuse: a kept buffer that holds the request, and only that.
    {
        Rig rig;
        assert(rig.Made(130 * MiB, MemoryUsage::Upload) == 144 * MiB);
        rig.scheduler.Finish();
        assert(rig.Made(130 * MiB, MemoryUsage::Upload) == 0);
        rig.scheduler.Finish();
        assert(rig.Made(140 * MiB, MemoryUsage::Upload) == 0);          // 144 holds 140
        rig.scheduler.Finish();
        assert(rig.Made(200 * MiB, MemoryUsage::Upload) == 208 * MiB);  // 144 does not: it goes first
        assert(pool.live == rig.stream + 208 * MiB);
        rig.scheduler.Finish();
        assert(rig.Made(150 * MiB, MemoryUsage::Upload) == 0);          // the 208 one
        // A buffer too small for the next request that the GPU still uses stays beside the new one.
        assert(rig.Made(129 * MiB, MemoryUsage::Upload) == 144 * MiB);
        rig.scheduler.Finish();
        assert(rig.Made(160 * MiB, MemoryUsage::Upload) == 0);          // 208 again; 144 idle
        assert(rig.Made(250 * MiB, MemoryUsage::Upload) == 256 * MiB);  // 208 in use stays, 144 goes
        assert(pool.live == rig.stream + 208 * MiB + 256 * MiB);
        // GPU-only buffers keep Eden's reuse of the whole power of two.
        assert(rig.Made(130 * MiB, MemoryUsage::DeviceLocal) == 256 * MiB);
        rig.scheduler.Finish();
        assert(rig.Made(250 * MiB, MemoryUsage::DeviceLocal) == 0);
    }
    // Any mix of sizes, kinds, deferred downloads and frames: never a buffer smaller than its
    // request (Made checks every one).
    {
        Rig rig;
        std::mt19937_64 random{195};
        std::deque<StagingBufferRef> deferred;
        for (int i = 0; i < 4000; ++i) {
            const size_t size = 1 + random() % (300 * MiB);
            if (random() % 8 == 0) {
                deferred.push_back(rig.staging->Request(size, MemoryUsage::Download, true));
                assert(deferred.back().mapped_span.size() >= size);
                if (deferred.size() > 4) {
                    rig.staging->FreeDeferred(deferred.front());
                    deferred.pop_front();
                }
            } else {
                rig.Made(size, (random() & 1) ? MemoryUsage::Upload : MemoryUsage::Download);
            }
            if (random() % 3 == 0) rig.scheduler.Finish();
            else rig.scheduler.Submit();
            rig.staging->TickFrame();
        }
        for (auto& ref : deferred) rig.staging->FreeDeferred(ref);
    }
    // Refused: the idle kept buffers go and the buffer is asked for once more.
    {
        Rig rig{400 * MiB};
        assert(rig.Made(100 * MiB, MemoryUsage::Upload) == 112 * MiB);
        assert(rig.Made(120 * MiB, MemoryUsage::Download) == 128 * MiB);
        rig.scheduler.Finish();                                         // both idle, 160 MiB left
        unsigned attempts = pool.attempts;
        assert(rig.Made(200 * MiB, MemoryUsage::Upload) == 208 * MiB);
        assert(pool.attempts == attempts + 2);
        assert(pool.live == rig.stream + 208 * MiB);
        // Buffers the GPU still uses and deferred ones stay, and a second refusal reaches the caller.
        auto deferred = rig.staging->Request(40 * MiB, MemoryUsage::Download, true);  // 48 MiB
        assert(rig.Made(60 * MiB, MemoryUsage::Upload) == 64 * MiB);               // 80 MiB left
        attempts = pool.attempts;
        bool refused = false;
        try {
            rig.Made(100 * MiB, MemoryUsage::Upload);                   // 112 MiB, none idle
        } catch (const vk::Exception& error) {
            refused = error.GetResult() == VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        assert(refused && pool.attempts == attempts + 2);
        assert(pool.live == rig.stream + 208 * MiB + 48 * MiB + 64 * MiB);
        rig.staging->FreeDeferred(deferred);
        rig.scheduler.Finish();
        assert(rig.Made(100 * MiB, MemoryUsage::Upload) == 112 * MiB);  // once those three go
        assert(pool.live == rig.stream + 112 * MiB);
        // Another error is not asked again.
        pool.refusal = VK_ERROR_OUT_OF_HOST_MEMORY;
        pool.capacity = pool.live;
        attempts = pool.attempts;
        refused = false;
        try {
            rig.Made(300 * MiB, MemoryUsage::Upload);
        } catch (const vk::Exception& error) {
            refused = error.GetResult() == VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        assert(refused && pool.attempts == attempts + 1);
    }
    std::puts("patched: PASS");
    return 0;
}
'''

control_main = r'''
int main() {
    // Eden's own pool: a transfer a little over 128 MiB asks for #195's buffer, and a refusal ends it.
    {
        Rig rig;
        assert(rig.Made(130 * MiB, MemoryUsage::Upload) == 256 * MiB && pool.last_usage == kStagingUsage);
        rig.scheduler.Finish();
        assert(rig.Made(250 * MiB, MemoryUsage::Upload) == 0);
    }
    {
        Rig rig{200 * MiB};
        assert(rig.Made(100 * MiB, MemoryUsage::Upload) == 128 * MiB);
        rig.scheduler.Finish();
        const unsigned attempts = pool.attempts;
        bool refused = false;
        try {
            rig.Made(130 * MiB, MemoryUsage::Upload);
        } catch (const vk::Exception&) {
            refused = true;
        }
        assert(refused && pool.attempts == attempts + 1);
    }
    std::puts("control: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, text in stubs.items():
        (tmp / 'stubs' / name).parent.mkdir(parents=True, exist_ok=True)
        (tmp / 'stubs' / name).write_text(text)
    outputs = {}
    for name, pool_source, main in (('patched', patched, patched_main), ('control', original, control_main)):
        (tmp / f'{name}_pool.cpp').write_text(pool_source)
        (tmp / f'{name}.cpp').write_text(f'#include "{name}_pool.cpp"\n' + harness + main)
        exe = tmp / name
        subprocess.run(['c++', '-std=c++20', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=undefined', '-I' + str(tmp / 'stubs'), '-I' + str(src),
                        '-I' + str(root / 'headless'), str(tmp / f'{name}.cpp'), '-o', str(exe)], check=True)
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        assert run.returncode == 0, f'{name} failed'
        outputs[name] = run.stdout + run.stderr

log = outputs['patched']
# What a tester sees on the console: the first large buffer, then the refused one asked again.
assert ('graphics memory: staging buffer of 144 MiB made for a transfer of 130 MiB, not the 256 MiB '
        'of the next power of two (1 so far)') in log
assert ('graphics memory: a staging buffer of 218103808 bytes was refused; released 2 idle staging buffers '
        '(240 MiB) and asked again') in log
assert 'graphics memory: the staging buffer was made when asked again' in log
assert ('graphics memory: a staging buffer of 117440512 bytes was refused; released 0 idle staging buffers '
        '(0 MiB) and asked again') in log
assert 'released 3 idle staging buffers (320 MiB)' in log
assert 'staging buffer' not in outputs['control']
print('Staging buffers: PASS Eden\'s power of two up to 16 MiB and for GPU-only buffers, 16 MiB steps above '
      '(130 MiB: 144, Eden 256 with #195\'s usage 0x8f3), kept buffers only when they hold the request, '
      'idle too-small ones let go, a refused buffer asked for again once after the idle ones go')
