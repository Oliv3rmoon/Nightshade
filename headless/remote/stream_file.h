// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A game file read where it lies on a download source, instead of downloaded whole first
// (Swordpdf/ProsperoEden#111): the emulator asks for pieces of a game file as the game needs them,
// and these come from the source as they are asked for.
//
// The file is read in blocks (kBlock) kept in memory, the most recently used ones (up to the
// cache's size, fewer while memory is short). One transfer at a time runs from the source, forward
// from where a read wanted a block nobody has yet: it keeps filling the blocks after it, ahead of
// the reads (up to kAhead past the furthest block a read in order wanted), and starts again
// elsewhere when a read wants a block outside that stretch. A game that loads a level reads on in
// order, so one transfer serves it; one that jumps about costs a new transfer per jump; two places
// read at once share the transfer in stretches of kTurn blocks. A read of a block that is kept
// behind (another thread reading a header again) or far ahead does not move the reading ahead.
//
// A source that cannot start a file other than at its beginning (Receiver::begin with from_start),
// one that sends another part of the file than was asked for, or a file of another size than the
// source listed, cannot be read this way: such a read fails, and the game is downloaded as before.
// Only transfers that a read waits for count towards the source failing for good: one reading
// ahead that fails is tried again later, waiting longer each time (up to kAheadPause).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source.h"

namespace Eden::Remote {

class StreamFile {
  public:
    static constexpr std::size_t kBlock = std::size_t{1} << 20;    // 1 MiB
    static constexpr std::size_t kAhead = 32;                      // blocks read ahead: 32 MiB
    static constexpr std::size_t kDefaultCache = 128;              // blocks kept: 128 MiB
    // Blocks a transfer that reads waits for brings before it turns to a read waiting elsewhere.
    static constexpr std::size_t kTurn = 4;
    // A transfer that brings no whole block for this long is given up: a network that stalled, or
    // one that trickles (the HTTP client gives up only below a byte a second).
    static constexpr std::chrono::seconds kStall{20};
    // The longest a transfer reading ahead waits before it is tried again after failing.
    static constexpr std::chrono::seconds kAheadPause{8};

    // What becomes of the transfers that fail, one line each in English (one that brought nothing
    // and when the source is asked again, the source failing for good); said on the file's own
    // thread. Nothing in it comes from the source's entry but what its backend says went wrong.
    using Log = std::function<void(const std::string& line)>;

    // source: the file's source, used by this file alone (one transfer at a time). size: the
    // file's size as the source gave it (it must know it). cache_blocks: how many blocks stay.
    StreamFile(std::unique_ptr<Source> source, SourceGame game, SourceFile file, std::size_t cache_blocks = kDefaultCache,
               Log log = {});
    ~StreamFile();
    StreamFile(const StreamFile&) = delete;
    StreamFile& operator=(const StreamFile&) = delete;

    std::uint64_t Size() const { return file_.size; }
    const std::string& Name() const { return file_.name; }
    // Up to size bytes at offset into data, from any thread: all of them, or fewer at the end of
    // the file, or when the source failed for good (Failed).
    std::size_t Read(std::uint8_t* data, std::size_t size, std::uint64_t offset);
    // Why reads fail, once the source failed for good (English); empty while it works.
    std::string Failed() const;
    // Ends the transfer and every read that waits for the source, at once (the game is being
    // stopped); later reads get only what is kept. Not a failure: Failed stays as it was. The
    // destructor does the same.
    void Close();
    // While this says memory is short (asked as each block comes), only the blocks being read
    // ahead stay kept, the least recently used going first. Asked under the file's lock: it must
    // be quick and take no lock of its own.
    void KeepLessWhen(std::function<bool()> short_of_memory);

    struct Stats {
        std::uint64_t transfers = 0;    // fetches started
        std::uint64_t received = 0;     // bytes that came
        std::uint64_t hits = 0;         // blocks a read found kept
        std::uint64_t waits = 0;        // blocks a read waited for
        std::uint64_t evicted = 0;      // blocks dropped to make room
        std::uint64_t kept = 0;         // blocks kept now
        std::uint64_t memory = 0;       // the bytes they take
    };
    Stats GetStats() const;

  private:
    struct Block {
        std::vector<std::uint8_t> bytes;
        std::list<std::uint64_t>::iterator age; // in lru_, the most recent first
    };
    class Filler;
    void Run();
    void Transfer(std::unique_lock<std::mutex>& lock, std::uint64_t start);
    // Under lock_: the block, or null when not kept; marks it as just used.
    const Block* Kept(std::uint64_t index);
    void Keep(std::uint64_t index, std::vector<std::uint8_t> bytes);
    // Under lock_: a read waits for a block nobody has.
    bool Waiting() const;
    std::uint64_t Blocks() const { return (file_.size + kBlock - 1) / kBlock; }

    std::unique_ptr<Source> source_;
    const SourceGame game_;
    const SourceFile file_;
    const std::size_t cache_blocks_;
    const Log log_;

    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::map<std::uint64_t, Block> blocks_;
    std::list<std::uint64_t> lru_;
    std::map<std::uint64_t, unsigned> wanted_; // blocks reads wait for, and how many reads
    // The block after the last one of the newest read that needed the network or went on in
    // order: where the reading ahead goes on from.
    std::uint64_t furthest_ = 0;
    std::uint64_t cursor_ = 0;                 // the block the running transfer fills next
    bool running_ = false;                     // a transfer is running
    bool quit_ = false;
    unsigned failures_ = 0;                    // transfers in a row a read waited for that brought no whole block
    unsigned ahead_failures_ = 0;              // the same for transfers no read waited for
    std::string failed_;
    std::function<bool()> short_of_memory_;
    Stats stats_;
    std::thread worker_;
};

} // namespace Eden::Remote
