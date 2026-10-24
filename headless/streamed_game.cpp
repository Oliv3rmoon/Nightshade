// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// Opening a game played where it lies on its download source; see streamed_game.h.
#include "streamed_game.h"

#include "diagnostics.h"
#include "remote/remote.h"
#include "remote/stream_file.h"
#include "stream_vfs.h"

#include <cstdlib>
#include <utility>

namespace Eden::StreamedGame {

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
    auto readable = std::make_shared<StreamVfsFile>(file, log);
    auto streamed = std::make_shared<Game>();
    streamed->path = path;
    streamed->source = source_name;
    streamed->name = game.name;
    streamed->file_name = file->Name();
    // 16 hex digits when the source knows it (remote.h), else nothing.
    streamed->title_id = game.title_id.empty() ? 0 : std::strtoull(game.title_id.c_str(), nullptr, 16);
    streamed->cover = game.cover;
    streamed->file = readable;
    streamed->failed = [readable] { return readable->Failed(); };
    streamed->stop = [file] { file->Close(); };
    Set(std::move(streamed));
    return true;
}

} // namespace Eden::StreamedGame
