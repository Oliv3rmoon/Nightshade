// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// The bridge's screen on a PC (view.hpp): every step it can show, in every catalog, as PNG files, and
// whether its text fits above the buttons' hints. tools/check-bridge.py builds and runs it.
//
//   bridge-preview <ui folder> [<folder for the pictures>]
//
// Without a folder for the pictures it only checks. A catalog whose text the launcher's font can't
// draw (Japanese, Arabic...) shows English, as on the console.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "pe/core/strings.hpp"
#include "view.hpp"

namespace fs = std::filesystem;
using namespace Eden::Bridge;

namespace {

std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

// Every step, with the longest of what it can say.
std::vector<std::pair<std::string, View>> Views() {
    const std::string name = "Nightshade 1.010.004";
    std::vector<std::pair<std::string, View>> views;
    views.push_back({"opening", {Step::opening, Say("Opening {0}...", {"Nightshade"}), {}, -1.0}});
    views.push_back({"finding", {Step::finding, Say("Preparing"), name, -1.0}});
    views.push_back({"downloading", {Step::installing, Say("Downloading {0}", {name}), Say("About {0} min left", {"12"}), 0.42}});
    views.push_back({"unpacking", {Step::installing, Say("Unpacking"), {}, 0.87}});
    views.push_back({"waiting",
                     {Step::waiting, Say("Waiting for the console to list {0}", {"Nightshade"}),
                      Say("{0} is installed. The homebrew loader lists it after its next scan or a restart.", {"Nightshade"}),
                      -1.0}});
    views.push_back({"closing", {Step::closing, Say("Opening {0}. This app closes now.", {"Nightshade"}), {}, -1.0}});
    views.push_back({"no-loader",
                     {Step::failed,
                      Say("No payload loader is listening on port {0}, so {1} can't be opened from here. Open it from "
                          "the PS5's home screen.",
                          {"9021", "Nightshade"}),
                      {},
                      -1.0}});
    views.push_back({"not-listed",
                     {Step::failed,
                      Say("The PS5 doesn't list {0} yet. The homebrew loader lists it after its next scan or a restart.",
                          {"Nightshade"}),
                      {},
                      -1.0}});
    views.push_back({"install-failed",
                     {Step::failed,
                      Say("{0} could not be installed: {1}",
                          {"Nightshade", Problem(GITHUB_RELEASE_HASHES_DIFFER, "Nightshade-v1.010.004.zip")}),
                      {},
                      -1.0}});
    views.push_back({"helper-refused",
                     {Step::failed,
                      Say("{0} could not be installed: {1}",
                          {"Nightshade", "A folder that isn't the app is in the way: /data/homebrew/PPSA64448"}),
                      {},
                      -1.0}});
    return views;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: bridge-preview <ui folder> [<folder for the pictures>]\n");
        return 2;
    }
    const fs::path ui = argv[1];
    const fs::path out = argc == 3 ? fs::path{argv[2]} : fs::path{};
    pe::gfx::Font font;
    if (!font.load(Read(ui / "fonts/montserrat-medium.pefont"))) {
        std::fprintf(stderr, "font: %s\n", font.error().c_str());
        return 2;
    }
    UseFont(&font);
    std::vector<std::string> languages{"en-US"};
    for (const auto& entry : fs::directory_iterator(ui / "lang"))
        if (entry.path().extension() == ".po") languages.push_back(entry.path().stem().string());
    std::sort(languages.begin() + 1, languages.end());
    if (!out.empty()) fs::create_directories(out);
    int failures = 0, pictures = 0, english = 0;
    Picture picture;
    for (const std::string& language : languages) {
        if (language == "en-US")
            pe::catalog().clear();
        else
            pe::catalog().load(Read(ui / "lang" / (language + ".po")));
        if (language != "en-US" && Say("Nightshade has its own app now") == "Nightshade has its own app now") ++english;
        for (const auto& [name, view] : Views()) {
            const float bottom = Draw(picture, font, view, 0.3);
            ++pictures;
            if (bottom > kTextBottom) {
                std::printf("FAIL bridge screen: %s, %s: its text reaches %.0f, under the hints (%.0f)\n", language.c_str(),
                            name.c_str(), bottom, kTextBottom);
                ++failures;
            }
            if (!out.empty() && (name == "downloading" || name == "install-failed" || language == "en-US")) {
                std::vector<unsigned char> rgb(static_cast<std::size_t>(Picture::kWidth) * Picture::kHeight * 3);
                for (std::size_t i = 0; i < picture.pixels().size(); ++i) {
                    const std::uint32_t pixel = picture.pixels()[i];
                    rgb[i * 3] = static_cast<unsigned char>(pixel);
                    rgb[i * 3 + 1] = static_cast<unsigned char>(pixel >> 8);
                    rgb[i * 3 + 2] = static_cast<unsigned char>(pixel >> 16);
                }
                const fs::path file = out / (language + "-" + name + ".png");
                stbi_write_png(file.string().c_str(), Picture::kWidth, Picture::kHeight, 3, rgb.data(), Picture::kWidth * 3);
            }
        }
    }
    std::printf("%s bridge screen: %d pictures in %zu languages (%d of them shown in English: the font has none of "
                "their letters), %d with text under the hints\n",
                failures ? "FAIL" : "PASS", pictures, languages.size(), english, failures);
    return failures ? 1 : 0;
}
