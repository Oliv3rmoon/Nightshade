// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The console's fonts, for the bridge (bridge.h): none. They are in /preinst/common/font, which an
// app sees only with the filesystem access Nightshade asks for at its start, and the bridge asks for
// none: it has nothing of its own to read or write outside its sandbox. So it is built with this
// in place of pe/gfx/system_fonts.cpp (and HarfBuzz): its text is the launcher's baked font alone
// (Latin, Cyrillic, Vietnamese), and a translation that font can't draw is shown in English
// (main.cpp, Say).
#include "pe/gfx/system_fonts.hpp"

namespace pe::gfx {

struct SystemFonts::Face {};

std::vector<std::string> system_font_folders() { return {}; }
std::vector<std::string> system_font_files(std::string_view, std::string_view) { return {}; }

SystemFonts::SystemFonts() = default;
SystemFonts::~SystemFonts() = default;
void SystemFonts::set(std::vector<std::string>, std::string_view) {}
int SystemFonts::face_for(char32_t) { return -1; }
bool SystemFonts::face_has(int, char32_t) { return false; }
void SystemFonts::shape(int, const char32_t*, int, bool, std::vector<RunGlyph>* out) { out->clear(); }
bool SystemFonts::field(int, std::uint32_t, float, float, GlyphField*) { return false; }
std::string SystemFonts::loaded() const { return "none"; }

} // namespace pe::gfx
