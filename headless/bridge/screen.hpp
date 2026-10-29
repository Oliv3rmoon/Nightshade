// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The bridge's screen (bridge.h): its picture (picture.hpp) shown through VideoOut as the native app
// boilerplate's demo shows one (its src/demo_renderer.cpp): two buffers in direct memory in the
// display's tiled layout, one shown while the other is filled.
#pragma once
#include <cstdint>

#include "picture.hpp"

namespace Eden::Bridge {

class Screen {
public:
    // VideoOut and its two buffers. False: there is no screen to show anything on (the bridge goes on
    // without one).
    bool open();
    // Copies the picture into the buffer not shown and shows it at the next refresh.
    bool show(const Picture& picture);

private:
    std::int32_t video_ = -1;
    std::uint8_t* buffers_[2] = {nullptr, nullptr};
    int shown_ = -1;
};

} // namespace Eden::Bridge
