// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Switch 2 titles (experimental, eden-nx2, docs/NX2_PORT.md) and the app's sessions.
//
// eden-nx2 keeps what it learns of a Switch 2 title's GPU driver in process-wide state: that a
// Switch 2 NvRm talks to the GPU (g_nx2_driver, g_nx2_ampere: the puller then runs Ampere
// classes and Turing macros), the GPU mappings the title made (Nx2GpuMemoryRegistry, applied to
// every new GPU address space), its semaphore regions and waiters. On a PC one process plays one
// game. Here the app returns to the launcher and starts the next game in the same process, so
// that state is put back before every session: a Switch 1 game after a Switch 2 one must not run
// as Ampere, nor a second Switch 2 game get the first one's mappings.
#pragma once
#include <new>

#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "video_core/nx2.h"

namespace Eden::Nx2 {
// Called between sessions only: no guest, GPU or service thread of an earlier session is left.
inline void ResetForSession() {
    Service::Nvidia::Devices::g_nx2_driver.store(false);
    Tegra::g_nx2_ampere.store(false);
    for (auto& region : Tegra::g_nx2_semaphore_regions) region.store(0);
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    registry.~Nx2GpuMemoryRegistry();
    new (&registry) Service::Nvidia::Devices::Nx2GpuMemoryRegistry();
    auto& waiters = Tegra::Nx2SemaphoreWaiters::Get();
    waiters.~Nx2SemaphoreWaiters();
    new (&waiters) Tegra::Nx2SemaphoreWaiters();
}
} // namespace Eden::Nx2
