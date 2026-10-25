// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Host check of a game played where it lies on its download source (streamed_game.h) as Eden's own
// loaders and its patched core.cpp see it (the derived core.cpp, headless/CMakeLists.txt): a game
// whose source failed, and one whose bytes are no game, shown to Eden's IdentifyFile and to the
// contents registration the session starts with (main.cpp), end without a crash. Eden's loader of
// unpacked games asks the folder of any file it is shown without looking whether there is one; a
// streamed file once had none. GetGameFileFromPath gives the streamed file for its path, and nothing
// for another streamed path. tools/build-headless-host.sh runs it; tools/check-streamed-game.py has
// the rest, without Eden's loaders.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/loader/loader.h"
#include "stream_vfs.h"
#include "streamed_game.h"

namespace {
using namespace Eden;
using Remote::StreamFile;

// A source that is gone: every transfer fails at once, as when the server is off.
struct GoneSource final : Remote::Source {
    std::string address() const override { return "gone"; }
    bool list(std::vector<Remote::SourceGame>*, std::string*, const Remote::Stopped&) override { return false; }
    bool cover(const Remote::SourceGame&, std::string*, const Remote::Stopped&) override { return false; }
    bool fetch(const Remote::SourceGame&, const Remote::SourceFile&, std::uint64_t, Remote::Receiver&,
               std::string* error) override {
        *error = "The server did not answer";
        return false;
    }
};

// A source whose file is bytes that are no game.
struct NoGameSource final : Remote::Source {
    std::uint64_t size;
    explicit NoGameSource(std::uint64_t size_) : size(size_) {}
    std::string address() const override { return "no game"; }
    bool list(std::vector<Remote::SourceGame>*, std::string*, const Remote::Stopped&) override { return false; }
    bool cover(const Remote::SourceGame&, std::string*, const Remote::Stopped&) override { return false; }
    bool fetch(const Remote::SourceGame&, const Remote::SourceFile&, std::uint64_t offset, Remote::Receiver& receiver,
               std::string* error) override {
        if (!receiver.begin(false, size - offset)) return false;
        std::vector<std::uint8_t> chunk(64 * 1024);
        for (std::uint64_t at = offset; at < size; at += chunk.size()) {
            if (receiver.stopped()) {
                *error = "stopped";
                return false;
            }
            for (std::size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<std::uint8_t>((at + i) * 7);
            if (!receiver.take(chunk.data(), static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), size - at))))
                return false;
        }
        return true;
    }
};

std::shared_ptr<StreamVfsFile> Streamed(std::unique_ptr<Remote::Source> source) {
    constexpr std::uint64_t kSize = 48 * StreamFile::kBlock;
    return std::make_shared<StreamVfsFile>(std::make_shared<StreamFile>(
        std::move(source), Remote::SourceGame{}, Remote::SourceFile{"7", "Game.nsp", kSize, Remote::FileKind::game}, 40));
}
} // namespace

int main() {
    for (const bool gone : {true, false}) {
        auto file = gone ? Streamed(std::make_unique<GoneSource>()) : Streamed(std::make_unique<NoGameSource>(48 * StreamFile::kBlock));
        // As the session starts a game: what kind of file it is, and its contents.
        const Loader::FileType type = Loader::IdentifyFile(file);
        assert(type == Loader::FileType::Unknown || type == Loader::FileType::Error);
        FileSys::ManualContentProvider contents;
        assert(!contents.AddEntriesFromContainer(file));
        assert(gone == !file->Failed().empty());
        std::printf("%s: IdentifyFile %d, contents not registered\n", gone ? "a source that is gone" : "bytes that are no game",
                    static_cast<int>(type));
    }
    // Eden's own GetGameFileFromPath, patched (the derived core.cpp in the core library).
    const auto file = Streamed(std::make_unique<NoGameSource>(48 * StreamFile::kBlock));
    auto game = std::make_shared<StreamedGame::Game>();
    game->path = "stream:home/10";
    game->file = file;
    StreamedGame::Set(game);
    const FileSys::VirtualFilesystem host = std::make_shared<FileSys::RealVfsFilesystem>();
    assert(Core::GetGameFileFromPath(host, "stream:home/10") == file);
    assert(Core::GetGameFileFromPath(host, "stream:home/11") == nullptr);
    StreamedGame::Close();
    assert(Core::GetGameFileFromPath(host, "stream:home/10") == nullptr);
    std::puts("streamed game check: PASS");
    return 0;
}
