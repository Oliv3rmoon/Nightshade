// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>
#include <exception>
#include <mutex>
#include <utility>

namespace Eden {
inline std::mutex gpu_failure_mutex;
inline std::exception_ptr gpu_failure;
inline void RecordGpuFailure(std::exception_ptr error) {
    std::lock_guard lock(gpu_failure_mutex);
    if (!gpu_failure) gpu_failure = std::move(error);
}
inline std::exception_ptr TakeGpuFailure() {
    std::lock_guard lock(gpu_failure_mutex);
    return std::exchange(gpu_failure, {});
}

// What the GPU worker does when it fails on a GPU that stopped (VK_ERROR_DEVICE_LOST, or the
// driver's stall dump ran). A normal stop then waited on the stuck GPU until the stop limit
// started the app again, and the console was left at its home screen with no crash report
// (Oliv3rmoon/Nightshade#1). main.cpp sets it to the crash report's Fail: the report is written
// and the app closes, as for a device loss on any other thread. Unset (the checks), the worker
// stops the session as for any other failure.
inline void (*gpu_lost)(const char* reason) noexcept = nullptr;
// Whether the driver's stall dump ran (Eden::Crash::GpuStalled), set beside gpu_lost.
inline bool (*gpu_stalled)() noexcept = nullptr;
inline bool IsGpuLost(const char* what) noexcept {
    return (what && std::strcmp(what, "VK_ERROR_DEVICE_LOST") == 0) || (gpu_stalled && gpu_stalled());
}
}
