// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_file.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

namespace Eden::Remote {

namespace {
// Transfers in a row that a read waits for and that bring no whole block before the file counts
// as gone.
constexpr unsigned kMostFailures = 5;
using Clock = std::chrono::steady_clock;
} // namespace

// The running transfer's receiver: fills the blocks from where it started, and says when to stop.
class StreamFile::Filler final : public Receiver {
  public:
    Filler(StreamFile& file, std::uint64_t start) : file_(file), first_(start), next_(start) {}

    // What comes must be the file the source listed, from where it was asked for.
    bool begin(bool from_start, std::uint64_t length) override {
        const std::uint64_t whole = file_.file_.size;
        const std::uint64_t at = next_ * kBlock;
        if (from_start && length != 0 && length != whole) {
            // Also what a server that has a shorter file than it listed comes to (416, the RomM
            // backend then asks for it from its start).
            broken = "The game's file on the source is " + std::to_string(length) + " bytes, not the " +
                     std::to_string(whole) + " it was listed with: the source's game list needs to be read again";
            return false;
        }
        if (from_start && at != 0) {
            broken = "This source sends a file only from its beginning, so the game cannot be read where it lies";
            return false;
        }
        if (!from_start && length != 0 && length != whole - at) {
            broken = "The source sent another part of the game's file than was asked for (" + std::to_string(length) +
                     " bytes from " + std::to_string(at) + ", not " + std::to_string(whole - at) + ")";
            return false;
        }
        return true;
    }

    bool take(const void* data, std::size_t size) override {
        // Nothing thrown here may cross the HTTP client's callbacks: it ends the transfer instead.
        try {
            auto* bytes = static_cast<const std::uint8_t*>(data);
            while (size > 0) {
                const std::size_t want = Want();
                if (want == 0) return false; // past the end
                // Each block takes its own size, no more: grown piece by piece it took half as much again.
                if (pending_.empty()) pending_.reserve(want);
                const std::size_t part = std::min(size, want - pending_.size());
                pending_.insert(pending_.end(), bytes, bytes + part);
                bytes += part;
                size -= part;
                if (pending_.size() == want && !Finish()) return false;
            }
            return true;
        } catch (const std::exception& problem) {
            error = std::string("The transfer stopped: ") + problem.what();
            return false;
        }
    }

    bool stopped() override {
        const std::lock_guard lock(file_.lock_);
        // In the middle of a block it turns only when it merely reads ahead: a block a read waits
        // for is not dropped half-way.
        if (file_.quit_ || (Turn() && !serving_)) redirected = true;
        // No whole block for so long: given up, which counts as a transfer that brought nothing.
        if (!redirected && Clock::now() - last_ >= kStall) {
            error = "The source sent no whole block for " + std::to_string(kStall.count()) + " s";
            return true;
        }
        return redirected;
    }

    // The whole blocks it brought. A part of one is dropped when the transfer breaks off (the next
    // starts at that block again): a source that keeps breaking off before a block is whole brings
    // nothing, however many bytes it sent.
    std::uint64_t kept = 0;
    std::string broken;      // what it sent cannot be this file from where it was asked: fails for good
    std::string error;       // why it stopped, when not for what the source said
    bool redirected = false; // stopped here, for another place or to close: no failure

  private:
    // The size of the block being filled.
    std::size_t Want() const {
        const std::uint64_t at = next_ * kBlock;
        if (at >= file_.file_.size) return 0;
        return static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, file_.file_.size - at));
    }
    // Under lock_: a read waits for a block nobody has, outside what this transfer still brings.
    bool Elsewhere() const {
        for (const auto& [index, readers] : file_.wanted_)
            if ((index < next_ || index >= next_ + kAhead) && !file_.blocks_.contains(index)) return true;
        return false;
    }
    // Under lock_: a read waits for a block this transfer still brings.
    bool WaitedHere() const {
        for (auto found = file_.wanted_.lower_bound(next_); found != file_.wanted_.end() && found->first < next_ + kAhead;
             ++found)
            if (!file_.blocks_.contains(found->first)) return true;
        return false;
    }
    // Under lock_: whether to turn to a read waiting elsewhere now. The block it was started for
    // always comes first, whatever else is wanted meanwhile: with reads waiting in several places,
    // transfers that turned away at once would bring nothing. One that a read waited for brings
    // kTurn blocks before it turns, so that two places read at once share it in stretches rather
    // than block by block; one that only reads ahead turns at once.
    bool Turn() {
        if (WaitedHere()) serving_ = true;
        if (next_ == first_ || !Elsewhere()) return false;
        return !serving_ || next_ - first_ >= kTurn;
    }
    // A block is whole: kept, and whether to go on with the next.
    bool Finish() {
        const std::lock_guard lock(file_.lock_);
        file_.Keep(next_, std::move(pending_));
        pending_ = {};
        ++kept;
        ++next_;
        last_ = Clock::now();
        file_.cursor_ = next_;
        file_.changed_.notify_all();
        // Done, wanted elsewhere, far enough ahead of the reads, or at a block that is kept
        // already: stop here.
        redirected = file_.quit_ || next_ >= file_.Blocks() || Turn() || next_ >= file_.furthest_ + kAhead ||
                     file_.blocks_.contains(next_);
        return !redirected;
    }

    StreamFile& file_;
    const std::uint64_t first_;
    std::uint64_t next_;
    std::vector<std::uint8_t> pending_;
    Clock::time_point last_ = Clock::now(); // when it began, or brought its last block
    bool serving_ = false;                  // a read waited for a block it brings
};

StreamFile::StreamFile(std::unique_ptr<Source> source, SourceGame game, SourceFile file, std::size_t cache_blocks,
                       Log log)
    : source_(std::move(source)), game_(std::move(game)), file_(std::move(file)),
      cache_blocks_(std::max<std::size_t>(cache_blocks, kAhead + 2)), log_(std::move(log)) {
    if (file_.size == 0) failed_ = "The source does not give the file's size";
    worker_ = std::thread([this] { Run(); });
}

StreamFile::~StreamFile() {
    Close();
    worker_.join();
}

void StreamFile::Close() {
    {
        const std::lock_guard lock(lock_);
        quit_ = true;
    }
    changed_.notify_all();
}

void StreamFile::KeepLessWhen(std::function<bool()> short_of_memory) {
    const std::lock_guard lock(lock_);
    short_of_memory_ = std::move(short_of_memory);
}

const StreamFile::Block* StreamFile::Kept(std::uint64_t index) {
    const auto found = blocks_.find(index);
    if (found == blocks_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, found->second.age);
    return &found->second;
}

void StreamFile::Keep(std::uint64_t index, std::vector<std::uint8_t> bytes) {
    stats_.received += bytes.size();
    if (const auto found = blocks_.find(index); found != blocks_.end()) {
        found->second.bytes = std::move(bytes);
        lru_.splice(lru_.begin(), lru_, found->second.age);
        return;
    }
    lru_.push_front(index);
    blocks_.emplace(index, Block{std::move(bytes), lru_.begin()});
    // The least recently used blocks go, but not one a read waits for: it has just come, and the
    // read has not taken its bytes yet. While memory is short, only those of a reading ahead stay.
    const std::size_t room = short_of_memory_ && short_of_memory_() ? kAhead + 2 : cache_blocks_;
    for (auto it = lru_.end(); blocks_.size() > room && it != lru_.begin();) {
        --it;
        if (wanted_.contains(*it)) continue;
        blocks_.erase(*it);
        it = lru_.erase(it);
        ++stats_.evicted;
    }
}

bool StreamFile::Waiting() const {
    for (const auto& [index, readers] : wanted_)
        if (!blocks_.contains(index)) return true;
    return false;
}

std::size_t StreamFile::Read(std::uint8_t* data, std::size_t size, std::uint64_t offset) {
    if (offset >= file_.size || size == 0) return 0;
    const std::uint64_t end = std::min<std::uint64_t>(offset + size, file_.size);
    const std::uint64_t first = offset / kBlock;
    const std::uint64_t last = (end - 1) / kBlock;
    std::size_t copied = 0;
    std::unique_lock lock(lock_);
    // Reads ahead go on from the newest read that needs the network, or that goes on in order
    // through what was read ahead (Run): not from a read of what is kept behind (another thread
    // reading a header again) or far ahead, which would hold back or scatter the reading ahead.
    bool missing = false;
    for (std::uint64_t index = first; index <= last && !missing; ++index) missing = !blocks_.contains(index);
    if (missing || (last + 1 > furthest_ && first < furthest_ + kAhead)) {
        furthest_ = last + 1;
        changed_.notify_all();
    }
    for (std::uint64_t index = first; index <= last; ++index) {
        const Block* block = Kept(index);
        if (block != nullptr) {
            ++stats_.hits;
        } else {
            ++stats_.waits;
            ++wanted_[index];
            changed_.notify_all();
            changed_.wait(lock, [&] { return quit_ || !failed_.empty() || blocks_.contains(index); });
            block = Kept(index);
            if (--wanted_[index] == 0) wanted_.erase(index);
            if (block == nullptr) return copied;
        }
        const std::uint64_t from = std::max(offset, index * kBlock);
        const std::uint64_t to = std::min(end, (index + 1) * kBlock);
        std::memcpy(data + copied, block->bytes.data() + (from - index * kBlock), static_cast<std::size_t>(to - from));
        copied += static_cast<std::size_t>(to - from);
    }
    return copied;
}

std::string StreamFile::Failed() const {
    const std::lock_guard lock(lock_);
    return failed_;
}

StreamFile::Stats StreamFile::GetStats() const {
    const std::lock_guard lock(lock_);
    Stats stats = stats_;
    stats.kept = blocks_.size();
    for (const auto& [index, block] : blocks_) stats.memory += block.bytes.capacity();
    return stats;
}

void StreamFile::Run() {
    std::unique_lock lock(lock_);
    // Nothing thrown here may end the program: the reads then fail, saying why.
    try {
        while (!quit_) {
            // A block a read waits for that nobody brings: the first one, in order. Else the first
            // block missing within half the reading-ahead stretch after the newest read, so that
            // reads in order find their blocks there.
            std::uint64_t start = 0;
            bool found = false;
            for (const auto& [index, readers] : wanted_)
                if (!blocks_.contains(index)) {
                    start = index;
                    found = true;
                    break;
                }
            // (Nothing is read ahead before the first read: furthest_ is 0 until then.)
            for (std::uint64_t index = furthest_; !found && furthest_ > 0 &&
                                                  index < std::min<std::uint64_t>(furthest_ + kAhead / 2, Blocks());
                 ++index)
                if (!blocks_.contains(index)) {
                    start = index;
                    found = true;
                }
            if (!found || !failed_.empty()) {
                changed_.wait(lock);
                continue;
            }
            Transfer(lock, start);
        }
    } catch (const std::exception& problem) {
        if (!lock.owns_lock()) lock.lock();
        if (failed_.empty()) failed_ = std::string("Reading the file stopped: ") + problem.what();
        running_ = false;
        changed_.notify_all();
    }
}

void StreamFile::Transfer(std::unique_lock<std::mutex>& lock, std::uint64_t start) {
    running_ = true;
    cursor_ = start;
    ++stats_.transfers;
    lock.unlock();
    Filler filler(*this, start);
    std::string error;
    try {
        (void)source_->fetch(game_, file_, start * kBlock, filler, &error);
    } catch (const std::exception& problem) {
        error = std::string("The transfer stopped: ") + problem.what();
    }
    lock.lock();
    running_ = false;
    if (!filler.error.empty()) error = filler.error;
    // Said without the lock: the log may take a moment.
    const auto say = [&](const std::string& line) {
        if (!log_) return;
        lock.unlock();
        log_(line);
        lock.lock();
    };
    if (!filler.broken.empty()) {
        failed_ = filler.broken;
        changed_.notify_all();
        say(failed_);
    } else if (filler.kept == 0 && !filler.redirected && !quit_) {
        const std::string why = error.empty() ? "The source stopped sending the file" : error;
        if (Waiting()) {
            // A read waits: it counts, and the reads fail once the source failed so many times.
            if (++failures_ >= kMostFailures) {
                failed_ = why;
                changed_.notify_all();
                say("Gave up after " + std::to_string(kMostFailures) + " transfers in a row brought nothing: " + why);
            } else {
                // Wait a little longer each time before asking again.
                const auto pause = std::chrono::milliseconds(250) * (1u << failures_);
                say("A transfer from " + std::to_string(start) + " MiB in brought nothing (" + why + "); asking again in " +
                    std::to_string(pause.count()) + " ms (" + std::to_string(failures_) + " of " +
                    std::to_string(kMostFailures) + ")");
                changed_.wait_for(lock, pause, [&] { return quit_; });
            }
        } else {
            // Only reading ahead: nothing the game waits for failed. Asked again later, longer each
            // time up to kAheadPause, or as soon as a read waits.
            ahead_failures_ = std::min(ahead_failures_ + 1, 6u);
            const auto pause = std::min<std::chrono::milliseconds>(std::chrono::milliseconds(250) * (1u << ahead_failures_),
                                                                   kAheadPause);
            say("A transfer reading ahead from " + std::to_string(start) + " MiB in brought nothing (" + why +
                "); reading ahead again in " + std::to_string(pause.count()) + " ms");
            changed_.wait_for(lock, pause, [&] { return quit_ || Waiting(); });
        }
    } else {
        failures_ = 0;
        ahead_failures_ = 0;
    }
    changed_.notify_all();
}

} // namespace Eden::Remote
