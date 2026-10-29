// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The bridge's picture (bridge.h): drawn by the CPU, with the launcher's own font
// (pe/gfx/font.hpp: Montserrat, a distance field per glyph) for its text, and shown by screen.hpp.
// Nothing of the launcher's OpenGL renderer: the bridge has no GPU work, and draws a few lines when
// something changes. The same picture is drawn on a PC by preview.cpp.
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

#include "pe/gfx/font.hpp"

namespace Eden::Bridge {

struct Rgb {
    std::uint8_t r, g, b;
};

class Picture {
public:
    static constexpr int kWidth = 1920;
    static constexpr int kHeight = 1080;

    void clear(Rgb colour);
    void fill(float x, float y, float w, float h, Rgb colour, float alpha = 1.0f);
    // An anti-aliased disc, and a ring of the given width (the controller's Circle).
    void disc(float cx, float cy, float radius, Rgb colour, float alpha = 1.0f);
    void ring(float cx, float cy, float radius, float width, Rgb colour, float alpha = 1.0f);
    // Two strokes across, the controller's Cross.
    void cross(float cx, float cy, float half, float width, Rgb colour);
    // One line of text with its baseline at y (x is its left edge, centre or right edge); returns its width.
    float text(const pe::gfx::Font& font, std::string_view value, float x, float y, float size, Rgb colour,
               pe::gfx::Align align = pe::gfx::Align::left, float alpha = 1.0f);

    // Row-major pixels: R, G, B and A from the lowest byte up, as VideoOut takes them.
    const std::vector<std::uint32_t>& pixels() const { return pixels_; }

private:
    void blend(int x, int y, Rgb colour, float alpha);

    std::vector<std::uint32_t> pixels_ = std::vector<std::uint32_t>(static_cast<std::size_t>(kWidth) * kHeight);
};

} // namespace Eden::Bridge
