// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of the wallpaper folders (wallpapers.h): the game files folder's wallpapers/ and
// /data/prosperoeden/wallpapers, made in the temporary folder. Which folders are read and in which
// order, an image in either or both listed once, a folder that isn't there, and the file a chosen
// name is read from.
//
// usage: eden-wallpapers-check
#include "wallpapers.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace
{
namespace fs = std::filesystem;
int failures = 0;
int passed = 0;

void expect(bool ok, const std::string &what)
{
    if (ok)
        ++passed;
    else
    {
        std::printf("FAIL wallpapers: %s\n", what.c_str());
        ++failures;
    }
}

void write(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// A folder's regular files, as the console's ListEntries gives them (unsorted here: List sorts).
bool files(const std::string &folder, std::vector<std::string> *names)
{
    names->clear();
    std::error_code error;
    fs::directory_iterator it{folder, error};
    if (error)
        return false;
    for (fs::directory_iterator end; it != end; it.increment(error))
    {
        if (error)
            return false;
        std::error_code type_error;
        if (it->is_regular_file(type_error) && !type_error)
            names->push_back(it->path().filename().string());
    }
    return true;
}

bool exists(const std::string &path)
{
    std::error_code error;
    return fs::is_regular_file(path, error) && !error;
}

std::string names(const std::vector<Eden::Wallpapers::Image> &images)
{
    std::string text;
    for (const auto &image : images)
        text += (text.empty() ? "" : ",") + image.name;
    return text;
}

void folders()
{
    using Eden::Wallpapers::Folders;
    const std::string data = "/data/prosperoeden/wallpapers";
    // The default: the game files folder is the data folder, so one folder.
    expect(Folders("/data/prosperoeden", data) == std::vector<std::string>{data},
           "the game files folder at /data/prosperoeden reads one folder");
    expect(Folders("/data/prosperoeden/", data + "/") == std::vector<std::string>{data},
           "a trailing slash is the same folder");
    // A game files folder on a USB drive: its wallpapers/ first, then the data folder's.
    expect(Folders("/mnt/usb0/assets", data) == std::vector<std::string>{"/mnt/usb0/assets/wallpapers", data},
           "a game files folder on a USB drive is read first, then the data folder");
    expect(Folders("/mnt/usb0/homebrew/PPSA99008/assets", data) ==
               std::vector<std::string>{"/mnt/usb0/homebrew/PPSA99008/assets/wallpapers", data},
           "the app folder's assets/, when it is the game files folder, is read first");
    expect(Folders("/", data) == std::vector<std::string>{"/wallpapers", data}, "a game files folder at / is /wallpapers");
    expect(Folders("", data) == std::vector<std::string>{data}, "without a game files folder, the data folder alone");
}

void listing(const fs::path &base)
{
    const fs::path assets = base / "usb0/assets", data = base / "data/prosperoeden";
    const std::string usb = Eden::Wallpapers::In(assets.string());
    const std::string own = (data / "wallpapers").string();
    const std::vector<std::string> both = Eden::Wallpapers::Folders(assets.string(), own);
    expect(both == std::vector<std::string>{usb, own}, "the temporary game files folder and data folder are two");

    // Neither folder there yet: nothing, and nothing fails.
    expect(Eden::Wallpapers::List(both, files).empty(), "no folder: no images");

    write(data / "wallpapers/beach.png", "data beach");
    write(data / "wallpapers/Forest.jpg", "data forest");
    expect(names(Eden::Wallpapers::List(both, files)) == "beach.png,Forest.jpg",
           "the data folder's images while the game files folder has no wallpapers/ (it works as before)");

    write(assets / "wallpapers/city.png", "usb city");
    write(assets / "wallpapers/beach.png", "usb beach"); // the same name as the data folder's
    write(assets / "wallpapers/Aurora.PNG", "usb aurora");
    fs::create_directories(assets / "wallpapers/old"); // a folder is not an image
    write(assets / "keys/prod.keys", "keys");           // the rest of the folder is not read
    const auto images = Eden::Wallpapers::List(both, files);
    expect(names(images) == "Aurora.PNG,beach.png,city.png,Forest.jpg",
           "both folders' images, sorted without regard to case, a name in both listed once: " + names(images));
    bool from_usb = false, from_data = false;
    for (const auto &image : images)
    {
        if (image.name == "beach.png")
            from_usb = image.folder == usb;
        if (image.name == "Forest.jpg")
            from_data = image.folder == own;
    }
    expect(from_usb, "a name in both folders is the game files folder's");
    expect(from_data, "an image only in the data folder is listed from there");

    using Eden::Wallpapers::Find;
    expect(Find(both, "beach.png", exists) == usb + "/beach.png", "a name in both folders is read from the game files folder");
    expect(Find(both, "Forest.jpg", exists) == own + "/Forest.jpg", "a name only in the data folder is read from there");
    expect(Find(both, "city.png", exists) == usb + "/city.png", "a name only in the game files folder is read from there");
    expect(Find(both, "gone.png", exists).empty(), "a name no folder has is no file");
    expect(Find(both, "", exists).empty(), "no wallpaper is no file");
    expect(Find(both, "old", exists).empty(), "a folder is no wallpaper file");

    // The same folder twice (the game files folder is the data folder): each image once.
    const std::vector<std::string> one = Eden::Wallpapers::Folders(data.string(), own);
    expect(one == std::vector<std::string>{own}, "the data folder as the game files folder is one folder");
    expect(names(Eden::Wallpapers::List({own, own}, files)) == "beach.png,Forest.jpg",
           "a folder named twice lists each image once");

    // The USB drive pulled out: the data folder's images stay, beach.png now from there.
    fs::remove_all(assets);
    const auto left = Eden::Wallpapers::List(both, files);
    expect(names(left) == "beach.png,Forest.jpg", "without the game files folder the data folder's images stay");
    expect(Find(both, "beach.png", exists) == own + "/beach.png", "and a name in both is read from the data folder");
    expect(Find(both, "city.png", exists).empty(), "and an image only on the drive is no file");
}
} // namespace

int main()
{
    std::error_code error;
    const fs::path base = fs::temp_directory_path(error) / ("eden-wallpapers-check-" + std::to_string(::getpid()));
    if (error)
    {
        std::fprintf(stderr, "no temporary folder: %s\n", error.message().c_str());
        return 2;
    }
    fs::remove_all(base, error);
    folders();
    listing(base);
    fs::remove_all(base, error);
    std::printf("%s wallpapers: %d checks of the folders read, the images listed and the file a name is read from, %d failed\n",
                failures ? "FAIL" : "PASS", passed + failures, failures);
    return failures ? 1 : 0;
}
