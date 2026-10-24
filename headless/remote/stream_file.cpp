// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_file.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace Eden::Remote {

namespace {
// Transfers in a row that bring nothing before the file counts as gone.
constexpr unsigned kMostFailures = 5;
} // namespace

// The running transfer's receiver: fills the blocks from where it started, and says when to stop.
class StreamFile::Filler final : public Receiver {
  public:
    Filler(StreamFile& file, std::uint64_t start) : file_(file), first_(start), next_(start) {}

    bool begin(bool from_start) override {
        if (from_start && next_ != 0) {
            cannot_range = true;
            return false;
        }
        return true;
    }

    bool take(const void* data, std::size_t size) override {
        auto* bytes = static_cast<const std::uint8_t*>(data);
        while (size > 0) {
            const std::size_t want = Want();
            if (want == 0) return false; // past the end
            const std::size_t part = std::min(size, want - pending_.size());
            pending_.insert(pending_.end(), bytes, bytes + part);
            bytes += part;
            size -= part;
            brought += part;
            if (pending_.size() == want && !Finish()) return false;
        }
        return true;
    }

    // The block it was started for always comes first, whatever else is wanted meanwhile: with
    // reads waiting in several places, transfers that turned away at once would bring nothing.
    bool stopped() override {
        const std::lock_guard lock(file_.lock_);
        if (file_.quit_ || (next_ > first_ && Elsewhere())) redirected = true;
        return redirected;
    }

    std::uint64_t brought = 0;
    bool cannot_range = false;
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
    // A block is whole: kept, and whether to go on with the next.
    bool Finish() {
        const std::lock_guard lock(file_.lock_);
        file_.Keep(next_, std::move(pending_));
        pending_ = {};
        ++next_;
        file_.cursor_ = next_;
        file_.changed_.notify_all();
        // Done, wanted elsewhere, far enough ahead of the reads, or at a block that is kept
        // already: stop here.
        redirected = file_.quit_ || next_ >= file_.Blocks() || Elsewhere() || next_ >= file_.furthest_ + kAhead ||
                     file_.blocks_.contains(next_);
        return !redirected;
    }

    StreamFile& file_;
    const std::uint64_t first_;
    std::uint64_t next_;
    std::vector<std::uint8_t> pending_;
};

StreamFile::StreamFile(std::unique_ptr<Source> source, SourceGame game, SourceFile file, std::size_t cache_blocks)
    : source_(std::move(source)), game_(std::move(game)), file_(std::move(file)),
      cache_blocks_(std::max<std::size_t>(cache_blocks, kAhead + 2)) {
    if (file_.size == 0) failed_ = "The source does not give the file's size";
    worker_ = std::thread([this] { Run(); });
}

StreamFile::~StreamFile() {
    {
        const std::lock_guard lock(lock_);
        quit_ = true;
    }
    changed_.notify_all();
    worker_.join();
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
    // read has not taken its bytes yet.
    for (auto it = lru_.end(); blocks_.size() > cache_blocks_ && it != lru_.begin();) {
        --it;
        if (wanted_.contains(*it)) continue;
        blocks_.erase(*it);
        it = lru_.erase(it);
        ++stats_.evicted;
    }
}

std::size_t StreamFile::Read(std::uint8_t* data, std::size_t size, std::uint64_t offset) {
    if (offset >= file_.size || size == 0) return 0;
    const std::uint64_t end = std::min<std::uint64_t>(offset + size, file_.size);
    std::size_t copied = 0;
    std::unique_lock lock(lock_);
    // Reads ahead go on from the newest read (Run).
    furthest_ = (end - 1) / kBlock + 1;
    changed_.notify_all();
    for (std::uint64_t index = offset / kBlock; index * kBlock < end; ++index) {
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
    return stats_;
}

void StreamFile::Run() {
    std::unique_lock lock(lock_);
    while (!quit_) {
        // A block a read waits for that nobody brings: the first one, in order. Else the first
        // block missing within half the reading-ahead stretch after the newest read, so that reads
        // in order find their blocks there.
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
        running_ = true;
        cursor_ = start;
        ++stats_.transfers;
        lock.unlock();
        Filler filler(*this, start);
        std::string error;
        (void)source_->fetch(game_, file_, start * kBlock, filler, &error);
        lock.lock();
        running_ = false;
        if (filler.cannot_range) {
            failed_ = "This source sends a file only from its beginning, so the game cannot be read where it lies";
        } else if (filler.brought == 0 && !filler.redirected && !quit_) {
            if (++failures_ >= kMostFailures) {
                failed_ = error.empty() ? "The source stopped sending the file" : error;
            } else {
                // Wait a little longer each time before asking again.
                const auto pause = std::chrono::milliseconds(250) * (1u << failures_);
                changed_.wait_for(lock, pause, [&] { return quit_; });
            }
        } else {
            failures_ = 0;
        }
        changed_.notify_all();
    }
}

} // namespace Eden::Remote
