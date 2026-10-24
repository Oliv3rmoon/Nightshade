// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Opening a game played where it lies on its download source; see streamed_game.h.
#include "streamed_game.h"

#include "assets_dir.h"
#include "diagnostics.h"
#include "metadata_bridge.h"
#include "performance.h"
#include "remote/remote.h"
#include "remote/stream_file.h"
#include "stream_vfs.h"

#include "core/file_sys/registered_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <utility>

namespace Eden::StreamedGame {

namespace {
// The source has this long to send the start of the game's file: one that is off, down or stalled
// is told before anything else waits for it.
constexpr std::chrono::seconds kStart{15};
// And this long, from the beginning, for the rest of what the session reads of the game before it
// loads it: nothing reads the controller yet then, so nothing could stop those reads.
constexpr std::chrono::seconds kBeforeLoad{45};
} // namespace

bool Open(const std::string& path, std::string* error) {
    Close();
    // The lines of the file and its transfers: "[ProsperoEden] stream: ..." (docs/DOWNLOADS.md).
    // Only the source's name is said of it, never its address or sign-in.
    const auto log = [](const std::string& line) { Report("stream", line.c_str()); };
    std::string source, id;
    Remote::Game game;
    if (!Remote::ParseStreamPath(path, &source, &id) || !Remote::Find(source, id, &game)) {
        *error = "The game is no longer on its download source";
        log(*error);
        return false;
    }
    std::string source_name = source;
    for (const Remote::SourceStatus& status : Remote::Current().sources)
        if (status.key == source) source_name = status.name;
    std::string why;
    std::shared_ptr<Remote::StreamFile> file = Remote::OpenStream(source, id, &why, log);
    if (file == nullptr) {
        *error = "The game could not be read from " + source_name + ": " + why;
        log(game.name + ": " + why);
        return false;
    }
    using Remote::StreamFile;
    log(game.name + " (" + file->Name() + ", " + StreamVfsFile::Size(file->Size()) + ") is played where it lies on " +
        source_name + ": read in " + std::to_string(StreamFile::kBlock >> 20) + " MiB blocks, up to " +
        std::to_string((StreamFile::kAhead * StreamFile::kBlock) >> 20) + " MiB ahead, " +
        std::to_string((StreamFile::kDefaultCache * StreamFile::kBlock) >> 20) + " MiB kept");
    // Graphics memory short (the renderer's collector says so): it keeps no more than it reads ahead.
    file->KeepLessWhen([] { return Performance::graphics_memory_short.load(std::memory_order_relaxed); });
    auto readable = std::make_shared<StreamVfsFile>(file, log);
    // 16 hex digits when the source knows it (remote.h), else nothing.
    const std::uint64_t listed = game.title_id.empty() ? 0 : std::strtoull(game.title_id.c_str(), nullptr, 16);
    const auto made = [&](std::uint64_t title_id) {
        auto streamed = std::make_shared<Game>();
        streamed->path = path;
        streamed->source = source_name;
        streamed->name = game.name;
        streamed->file_name = file->Name();
        streamed->title_id = title_id;
        streamed->cover = game.cover;
        streamed->file = readable;
        streamed->failed = [readable] { return readable->Failed(); };
        streamed->hold = [readable](std::function<void()> lost) { readable->Hold(std::move(lost)); };
        streamed->stop = [readable] { readable->Stop(); };
        return streamed;
    };
    Set(made(listed));

    // What the session reads of the game before it loads it, read here first, within a time limit:
    // its blocks are then kept for the session's own reads. Eden's metadata reader, which these
    // use, has no other caller while the session starts.
    const auto start = [&] {
        std::uint8_t bytes[4096];
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(bytes), file->Size()));
        return readable->Read(bytes, want, 0) == want;
    };
    const auto rest = [&] {
        std::uint64_t title_id = listed;
        try {
            if (title_id == 0) title_id = eden_game_title_id(path.c_str());
            (void)eden_game_supported_languages(path.c_str(), AssetsPath("keys").c_str());
            FileSys::ManualContentProvider contents;
            (void)contents.AddEntriesFromContainer(readable);
        } catch (const std::exception&) {
            // The session reads the same again, and says what it makes of it.
        }
        return title_id;
    };
    std::uint64_t title_id = listed;
    const Before before = ReadBeforeLoad(start, rest, [&] { readable->Stop(); }, kStart, kBeforeLoad, &title_id);
    if (before != Before::read) {
        const std::string lost =
            before == Before::no_start ?
                "it sent nothing of the game's file within " + std::to_string(kStart.count()) + " s" :
            before == Before::start_failed ?
                (file->Failed().empty() ? std::string{"it did not send the game's file"} : file->Failed()) :
                "it did not send what the game needs to start within " + std::to_string(kBeforeLoad.count()) + " s";
        Close();
        *error = "The game could not be read from " + source_name + ": " + lost;
        log(game.name + ": " + lost);
        return false;
    }
    // The title ID in its file, when the source did not know it: what the session goes by.
    if (title_id != listed) Set(made(title_id));
    return true;
}

} // namespace Eden::StreamedGame
