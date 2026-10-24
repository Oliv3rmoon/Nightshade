// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// What the launcher says when Eden cannot load a game. Eden's status is ErrorLoader (7) plus the
// loader's own status, so "Loader status 28" was status 21, a missing key area key: the keys file
// has no key for the game's key generation (Swordpdf/ProsperoEden#206, #279, #295). The player
// now reads Eden's sentence for the status and, for a key problem, the newest master key the
// keys file holds.
#pragma once
#include <cstdio>
#include <string>

namespace Eden::LoadFailure {

// status: Eden's Core::SystemResultStatus; loader_base: its ErrorLoader. loader_message: Eden's
// text for the loader status (Loader::GetResultStatusString), empty below loader_base.
// key_problem: the loader status is about the keys file. newest_master_key: the highest
// revision of master_key_XX loaded, or -1 for none.
inline std::string Text(unsigned status, unsigned loader_base, const std::string& loader_message, bool key_problem,
                        int newest_master_key) {
    std::string text = "Loader status " + std::to_string(status);
    if (status >= loader_base && !loader_message.empty()) {
        text += " (" + std::to_string(status - loader_base) + "): " + loader_message;
        if (text.back() != '.') text += '.';
    } else {
        text += '.';
    }
    if (key_problem) {
        text += " The keys file (prod.keys) lacks a key this game needs.";
        if (newest_master_key >= 0) {
            char name[32];
            std::snprintf(name, sizeof(name), "master_key_%02x", newest_master_key);
            text += std::string(" The newest master key in it is ") + name +
                    "; a game made for a newer system version needs keys from that version.";
        } else {
            text += " It holds no master key.";
        }
        text += " See stderr.log.";
    } else {
        text += " Check this ROM, its keys and firmware; see stderr.log.";
    }
    return text;
}

} // namespace Eden::LoadFailure
