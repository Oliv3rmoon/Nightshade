// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "picture.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Eden::Bridge {
namespace {

constexpr std::uint32_t Pack(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xff000000u | (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(g) << 8) | r;
}

float Clamp01(float value) { return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value; }

// The distance to the segment a-b.
float SegmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float t = Clamp01(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy));
    const float x = ax + t * dx - px, y = ay + t * dy - py;
    return std::sqrt(x * x + y * y);
}

} // namespace

void Picture::clear(Rgb colour) { std::fill(pixels_.begin(), pixels_.end(), Pack(colour.r, colour.g, colour.b)); }

void Picture::blend(int x, int y, Rgb colour, float alpha) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight || alpha <= 0.0f) return;
    std::uint32_t& pixel = pixels_[static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x)];
    if (alpha >= 1.0f) {
        pixel = Pack(colour.r, colour.g, colour.b);
        return;
    }
    const auto mix = [alpha](std::uint32_t under, std::uint8_t over) {
        return static_cast<std::uint8_t>(std::lround(static_cast<float>(over) * alpha +
                                                     static_cast<float>(under & 0xffu) * (1.0f - alpha)));
    };
    pixel = Pack(mix(pixel, colour.r), mix(pixel >> 8, colour.g), mix(pixel >> 16, colour.b));
}

void Picture::fill(float x, float y, float w, float h, Rgb colour, float alpha) {
    const int left = std::max(0, static_cast<int>(std::floor(x)));
    const int top = std::max(0, static_cast<int>(std::floor(y)));
    const int right = std::min(kWidth, static_cast<int>(std::ceil(x + w)));
    const int bottom = std::min(kHeight, static_cast<int>(std::ceil(y + h)));
    for (int row = top; row < bottom; ++row) {
        const float cover_y = Clamp01(std::min(y + h, row + 1.0f) - std::max(y, static_cast<float>(row)));
        for (int column = left; column < right; ++column) {
            const float cover_x = Clamp01(std::min(x + w, column + 1.0f) - std::max(x, static_cast<float>(column)));
            blend(column, row, colour, alpha * cover_x * cover_y);
        }
    }
}

void Picture::disc(float cx, float cy, float radius, Rgb colour, float alpha) {
    for (int row = static_cast<int>(cy - radius - 1); row <= static_cast<int>(cy + radius + 1); ++row)
        for (int column = static_cast<int>(cx - radius - 1); column <= static_cast<int>(cx + radius + 1); ++column) {
            const float distance = std::hypot(column + 0.5f - cx, row + 0.5f - cy);
            blend(column, row, colour, alpha * Clamp01(radius - distance + 0.5f));
        }
}

void Picture::ring(float cx, float cy, float radius, float width, Rgb colour, float alpha) {
    for (int row = static_cast<int>(cy - radius - 1); row <= static_cast<int>(cy + radius + 1); ++row)
        for (int column = static_cast<int>(cx - radius - 1); column <= static_cast<int>(cx + radius + 1); ++column) {
            const float distance = std::hypot(column + 0.5f - cx, row + 0.5f - cy);
            const float inside = Clamp01(radius - distance + 0.5f) * Clamp01(distance - (radius - width) + 0.5f);
            blend(column, row, colour, alpha * inside);
        }
}

void Picture::cross(float cx, float cy, float half, float width, Rgb colour) {
    for (int row = static_cast<int>(cy - half - width); row <= static_cast<int>(cy + half + width); ++row)
        for (int column = static_cast<int>(cx - half - width); column <= static_cast<int>(cx + half + width); ++column) {
            const float px = column + 0.5f, py = row + 0.5f;
            const float distance = std::min(SegmentDistance(px, py, cx - half, cy - half, cx + half, cy + half),
                                            SegmentDistance(px, py, cx - half, cy + half, cx + half, cy - half));
            blend(column, row, colour, Clamp01(width * 0.5f - distance + 0.5f));
        }
}

float Picture::text(const pe::gfx::Font& font, std::string_view value, float x, float y, float size, Rgb colour,
                   pe::gfx::Align align, float alpha) {
    std::vector<pe::gfx::GlyphQuad> quads;
    const float width = font.layout(value, x, y, size, align, quads);
    const std::vector<std::uint8_t>& atlas = font.atlas();
    const int atlas_w = font.atlas_width(), atlas_h = font.atlas_height();
    // A distance field: 128 on the outline, falling by 128 / range per atlas pixel outward; in
    // output pixels the range is sdf_range(size).
    const float spread = font.sdf_range(size);
    const auto sample = [&](float ax, float ay) {
        ax = std::clamp(ax, 0.0f, static_cast<float>(atlas_w - 1));
        ay = std::clamp(ay, 0.0f, static_cast<float>(atlas_h - 1));
        const int x0 = static_cast<int>(ax), y0 = static_cast<int>(ay);
        const int x1 = std::min(x0 + 1, atlas_w - 1), y1 = std::min(y0 + 1, atlas_h - 1);
        const float fx = ax - static_cast<float>(x0), fy = ay - static_cast<float>(y0);
        const auto at = [&](int ix, int iy) {
            return static_cast<float>(atlas[static_cast<std::size_t>(iy) * static_cast<std::size_t>(atlas_w) +
                                            static_cast<std::size_t>(ix)]);
        };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
        return top + (bottom - top) * fy;
    };
    for (const pe::gfx::GlyphQuad& quad : quads) {
        if (quad.x1 <= quad.x0 || quad.y1 <= quad.y0) continue;
        const int left = static_cast<int>(std::floor(quad.x0)), right = static_cast<int>(std::ceil(quad.x1));
        const int top = static_cast<int>(std::floor(quad.y0)), bottom = static_cast<int>(std::ceil(quad.y1));
        for (int row = top; row < bottom; ++row)
            for (int column = left; column < right; ++column) {
                const float u = quad.u0 + (column + 0.5f - quad.x0) / (quad.x1 - quad.x0) * (quad.u1 - quad.u0);
                const float v = quad.v0 + (row + 0.5f - quad.y0) / (quad.y1 - quad.y0) * (quad.v1 - quad.v0);
                const float field = sample(u * static_cast<float>(atlas_w) - 0.5f, v * static_cast<float>(atlas_h) - 0.5f);
                blend(column, row, colour, alpha * Clamp01((field - 128.0f) / 128.0f * spread + 0.5f));
            }
    }
    return width;
}

} // namespace Eden::Bridge
