// Nightshade - Its community: the Discord server About names, with a QR code a phone opens it by,
// and the card that names it once, at the app's first start (community.cpp).
// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "pe/ui/qr_code.hpp"

namespace pe::ui
{

// The server's name, its invite as a player would type it, and the address its QR code holds.
// Plain text (not tr()): a name and an address, the same in every language.
inline constexpr const char *kCommunityName = "nightly";
inline constexpr const char *kCommunityLink = "discord.gg/pSqgRU3YP9";
inline constexpr const char *kCommunityUrl = "https://discord.gg/pSqgRU3YP9";

// The QR code About and the card draw: kCommunityUrl's, made once.
inline const QrCode &community_qr()
{
    static const QrCode code = make_qr_code(kCommunityUrl);
    return code;
}

} // namespace pe::ui
