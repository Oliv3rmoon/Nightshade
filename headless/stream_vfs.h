// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A game file read where it lies on its download source (remote/stream_file.h) as Eden's file
// layer sees a file: read-only, named as the game's file is on the source, of the size the source
// gave, in a folder of its own that holds nothing else (Eden's loaders look at a file's folder).
// Eden reads the game through it as through a file on the console (streamed_game.h).
//
// A read the source can no longer answer, once it failed for good: the first one says why in the
// log, and Failed tells the session. Before the game runs it comes back short, as a read past a
// file's end does, and Eden's loader stops with an error. Once the game runs (Hold) it does not
// come back: Eden's storage layers above decrypt a read's whole buffer whatever came, and the
// game would get what was in it. It waits instead, the session is told at once (lost) and ends
// the game with that reason (headless/main.cpp); Stop, as the game is being stopped, lets it and
// every later read come back with what is kept. A read that waits long for the network is said in
// the log too (at most one line every ten seconds), and when the file is let go, what its
// transfers came to.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "remote/stream_file.h"

namespace Eden {

class StreamVfsFile final : public FileSys::VfsFile {
  public:
    using Clock = std::chrono::steady_clock;
    // A read that waits this long for the source is said in the log.
    static constexpr auto kSlow = std::chrono::milliseconds(500);
    static constexpr auto kSlowEvery = std::chrono::seconds(10);
    // One English line each; nothing from the source's entry is in them.
    using Log = std::function<void(const std::string& line)>;

    explicit StreamVfsFile(std::shared_ptr<Remote::StreamFile> file, Log log = {})
        : file_(std::move(file)), log_(std::move(log)) {}
    ~StreamVfsFile() override {
        if (!log_) return;
        const auto stats = file_->GetStats();
        const double minutes = std::chrono::duration<double>(Clock::now() - opened_).count() / 60.0;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "Closed after %.1f min: %llu transfers, %s received, %llu blocks found kept, %llu waited for, "
                      "%llu dropped from the cache",
                      minutes, static_cast<unsigned long long>(stats.transfers), Size(stats.received).c_str(),
                      static_cast<unsigned long long>(stats.hits), static_cast<unsigned long long>(stats.waits),
                      static_cast<unsigned long long>(stats.evicted));
        log_(line);
    }

    std::string GetName() const override { return file_->Name(); }
    std::size_t GetSize() const override { return static_cast<std::size_t>(file_->Size()); }
    bool Resize(std::size_t) override { return false; }
    // Empty: Eden's loaders ask it whether the file is part of an unpacked game (an ExeFS folder),
    // without looking whether there is one.
    FileSys::VirtualDir GetContainingDirectory() const override { return folder_; }
    bool IsWritable() const override { return false; }
    bool IsReadable() const override { return true; }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset = 0) const override {
        const Clock::time_point start = Clock::now();
        const std::size_t read = file_->Read(data, length, offset);
        const Clock::duration waited = Clock::now() - start;
        if (waited >= kSlow) Slow(length, offset, waited);
        // Short before the file's end: the source failed for good, or the file was stopped (the
        // game is being stopped, which is no failure).
        if (read < length && offset + read < GetSize() && Failing()) Held();
        return read;
    }
    std::size_t Write(const u8*, std::size_t, std::size_t = 0) override { return 0; }
    bool Rename(std::string_view) override { return false; }

    // Why the game's reads fail, once one did because the source failed for good (English);
    // empty until then.
    std::string Failed() const { return failed_.load() ? file_->Failed() : std::string{}; }

    // The game runs: from now on a read the source can no longer answer waits for Stop, and lost
    // is told at once (once, from that read's thread).
    void Hold(std::function<void()> lost) {
        const std::lock_guard lock(hold_lock_);
        holding_ = true;
        lost_ = std::move(lost);
    }
    // The game is being stopped: every read, waiting or to come, comes back with what is kept, and
    // none of them counts as a failure.
    void Stop() {
        {
            const std::lock_guard lock(hold_lock_);
            stopped_ = true;
        }
        file_->Close();
        held_.notify_all();
    }

    // "12.3 MB", "1.2 GB": a size for the log.
    static std::string Size(std::uint64_t bytes) {
        char text[32];
        if (bytes >= (std::uint64_t{1} << 30))
            std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1073741824.0);
        else
            std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1048576.0);
        return text;
    }

  private:
    // A read came short: true when the source failed for good, not because the file was stopped.
    bool Failing() const {
        if (stopped_.load()) return false;
        const std::string reason = file_->Failed();
        if (reason.empty()) return false; // closed
        if (!failed_.exchange(true) && log_) log_("The game's reads of what is not kept fail from now on: " + reason);
        return true;
    }
    // Once the game runs: the read waits for Stop, and the session is told.
    void Held() const {
        std::function<void()> lost;
        {
            const std::lock_guard lock(hold_lock_);
            if (!holding_ || stopped_) return;
            if (!lost_said_) lost = lost_;
            lost_said_ = true;
        }
        if (lost) lost();
        std::unique_lock lock(hold_lock_);
        held_.wait(lock, [&] { return stopped_.load(); });
    }
    void Slow(std::size_t length, std::size_t offset, Clock::duration waited) const {
        if (!log_) return;
        const std::lock_guard lock(slow_lock_);
        const Clock::time_point now = Clock::now();
        if (slow_said_ != Clock::time_point{} && now - slow_said_ < kSlowEvery) {
            ++slow_since_;
            return;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "A read of %zu KiB at %llu MiB waited %.1f s for the source", (length + 1023) >> 10,
                      static_cast<unsigned long long>(offset >> 20), std::chrono::duration<double>(waited).count());
        std::string said = line;
        if (slow_since_ > 0) said += " (and " + std::to_string(slow_since_) + " more since the last such line)";
        slow_said_ = now;
        slow_since_ = 0;
        log_(said);
    }

    const std::shared_ptr<Remote::StreamFile> file_;
    const Log log_;
    const FileSys::VirtualDir folder_ = std::make_shared<FileSys::VectorVfsDirectory>();
    const Clock::time_point opened_ = Clock::now();
    mutable std::atomic<bool> failed_{false};
    mutable std::mutex hold_lock_;
    mutable std::condition_variable held_;
    bool holding_ = false;           // the game runs (Hold)
    std::atomic<bool> stopped_{false};
    std::function<void()> lost_;
    mutable bool lost_said_ = false;
    mutable std::mutex slow_lock_;
    mutable Clock::time_point slow_said_{}; // when a slow read was said last
    mutable unsigned slow_since_ = 0;       // slow reads not said since
};

} // namespace Eden
