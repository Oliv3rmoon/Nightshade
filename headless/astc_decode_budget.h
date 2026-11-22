// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include "memory_budget.h"

namespace Eden::AstcPolicy {
// Decode-owned payload, including the input retained by the queued closure and the largest
// layer's RGBA scratch. Graphics staging and shared read scratch have separate budgets.
inline std::optional<std::uint64_t> DecodePayload(std::uint64_t input, std::uint64_t output,
                                                std::uint64_t width, std::uint64_t height) {
    constexpr auto max = (std::numeric_limits<std::uint64_t>::max)();
    if (!input || !output || !width || !height || width > max / height) return std::nullopt;
    const auto pixels = width * height;
    if (pixels > max / 4) return std::nullopt;
    const auto scratch = pixels * 4;
    if (input > max - output || input + output > max - scratch) return std::nullopt;
    return input + output + scratch;
}

// GPU thread only. Each context owns its charge until its buffers have been freed. The cache
// owns this budget before its contexts, and joins the decoder worker before destroying either.
class DecodeBudget {
public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : owner{std::exchange(other.owner, nullptr)}, bytes{other.bytes} {}
        Reservation& operator=(Reservation&& other) noexcept {
            if (this != &other) {
                Release();
                owner = std::exchange(other.owner, nullptr);
                bytes = other.bytes;
            }
            return *this;
        }
        ~Reservation() { Release(); }
        std::uint64_t Bytes() const { return owner ? bytes : 0; }
    private:
        friend class DecodeBudget;
        Reservation(DecodeBudget& budget, std::uint64_t size) : owner{&budget}, bytes{size} {}
        void Release() {
            if (owner) {
                owner->held -= bytes;
                owner = nullptr;
            }
        }
        DecodeBudget* owner{};
        std::uint64_t bytes{};
    };
    DecodeBudget() = default;
    DecodeBudget(const DecodeBudget&) = delete;
    DecodeBudget& operator=(const DecodeBudget&) = delete;
    std::uint64_t Held() const { return held; }
    std::optional<Reservation> Reserve(std::uint64_t bytes, bool busy) {
        if (!bytes || bytes > (std::numeric_limits<std::uint64_t>::max)() - held ||
            (!busy && !Performance::LightBackgroundRoom(held, bytes))) return std::nullopt;
        held += bytes;
        return Reservation{*this, bytes};
    }
private:
    std::uint64_t held{};
};
} // namespace Eden::AstcPolicy
