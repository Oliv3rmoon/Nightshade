// SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
// SPDX-License-Identifier: GPL-3.0-or-later
// A game file read where it lies on a download source, instead of downloaded whole first
// (Swordpdf/ProsperoEden#111): the emulator asks for pieces of a game file as the game needs them,
// and these come from the source as they are asked for.
//
// The file is read in blocks (kBlock) kept in memory, the most recently used ones (up to the
// cache's size). One transfer at a time runs from the source, forward from where a read wanted a
// block nobody has yet: it keeps filling the blocks after it, ahead of the reads (up to kAhead
// past the furthest block a read wanted), and starts again elsewhere when a read wants a block
// outside that stretch. A game that loads a level reads on in order, so one transfer serves it;
// one that jumps about costs a new transfer per jump. A source that cannot start a file other
// than at its beginning (Receiver::begin with from_start) cannot be read this way: such a read
// fails, and the game is downloaded as before.
#pragma once

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
    static constexpr std::size_t kDefaultCache = 256;              // blocks kept: 256 MiB

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

    struct Stats {
        std::uint64_t transfers = 0;    // fetches started
        std::uint64_t received = 0;     // bytes that came
        std::uint64_t hits = 0;         // blocks a read found kept
        std::uint64_t waits = 0;        // blocks a read waited for
        std::uint64_t evicted = 0;      // blocks dropped to make room
    };
    Stats GetStats() const;

  private:
    struct Block {
        std::vector<std::uint8_t> bytes;
        std::list<std::uint64_t>::iterator age; // in lru_, the most recent first
    };
    class Filler;
    void Run();
    // Under lock_: the block, or null when not kept; marks it as just used.
    const Block* Kept(std::uint64_t index);
    void Keep(std::uint64_t index, std::vector<std::uint8_t> bytes);
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
    std::uint64_t furthest_ = 0;               // the block after the newest read's last one
    std::uint64_t cursor_ = 0;                 // the block the running transfer fills next
    bool running_ = false;                     // a transfer is running
    bool quit_ = false;
    unsigned failures_ = 0;                    // transfers in a row that brought no whole block
    std::string failed_;
    Stats stats_;
    std::thread worker_;
};

} // namespace Eden::Remote
