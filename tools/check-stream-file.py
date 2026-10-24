#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of a game file read where it lies on a source (headless/remote/stream_file.h).

Builds stream_file.cpp with a stand-in source that serves a 100 MiB file of known bytes from any
offset, slowly, as a network does, and checks what a game's reads get and what they cost:
every byte right, read in order, at random and from eight threads at once (under the thread
sanitizer too); a read in order after the first block finds its blocks brought ahead, by one
transfer; a jump starts one new transfer; a read past the end gets the bytes up to it; the kept
blocks stay within the cache; a source that fails now and then is asked again, one that keeps
failing, keeps breaking off before a whole block came, or sends files only from their start, fails
the reads instead of hanging them, and says so in its log; a file closed while a read waits for a
source that stalls ends that read at once, without counting as a failure.

And: a source that goes away while the reads find what they want kept fails nothing (reading
ahead is only asked again later), and serves the next read once it is back; another thread reading
a kept header again does not hold back the reading ahead of one that reads in order, and two places
read at once share the transfers in stretches, not block by block; each kept block takes its own
size and no more, and while memory is short only the reading ahead stays; a source that answers
with another part of the file, or a file of another size, fails the read saying so; a transfer that
trickles is given up after StreamFile::kStall and asked again; nothing thrown by the source or the
log ends the program.
"""
from pathlib import Path
import os, platform, shutil, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
remote = root / 'headless/remote'

code = r'''
#include "stream_file.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <thread>
using namespace Eden::Remote;
static std::uint8_t Byte(std::uint64_t at) { return static_cast<std::uint8_t>((at * 2654435761u) >> 13); }

struct FakeSource final : Source {
    std::uint64_t size;
    std::atomic<int> fetches{0};
    std::atomic<int> fail_next{0};      // fetches that bring nothing
    bool from_start_only = false;
    std::uint64_t cut_after = 0;        // each fetch breaks off after this many bytes (0: never)
    std::uint64_t stall_from = 0;       // the bytes from here on never come (0: they all do)
    std::size_t chunk_size = 64 * 1024; // the pieces it sends in (at most 64 KiB)
    std::uint64_t told_length = 0;      // the length it says follows (0: the right one)
    std::atomic<bool> down{false};      // the network is away: every fetch fails at once
    std::uint64_t down_after = 0;       // ... once a fetch sent this many bytes (0: never)
    bool throws = false;                // a fetch throws, as an allocation that failed
    std::atomic<bool> trickle{false};   // the next fetch sends a few bytes now and then, never a block
    explicit FakeSource(std::uint64_t size_) : size(size_) {}
    std::string address() const override { return "fake"; }
    bool list(std::vector<SourceGame>*, std::string*, const Stopped&) override { return false; }
    bool cover(const SourceGame&, std::string*, const Stopped&) override { return false; }
    bool fetch(const SourceGame&, const SourceFile&, std::uint64_t offset, Receiver& receiver, std::string* error) override {
        ++fetches;
        if (throws) throw std::bad_alloc();
        if (down) { *error = "The server did not answer"; return false; }
        if (fail_next > 0) { --fail_next; *error = "connection reset"; return false; }
        if (from_start_only) offset = 0;
        if (!receiver.begin(from_start_only, told_length != 0 ? told_length : size - offset)) { *error = "stopped"; return false; }
        std::uint8_t chunk[64 * 1024];
        if (trickle.exchange(false))
            for (std::uint64_t at = offset; !receiver.stopped(); at += 100) {
                for (std::size_t i = 0; i < 100; ++i) chunk[i] = Byte(at + i);
                if (!receiver.take(chunk, 100)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        for (std::uint64_t at = offset; at < size;) {
            if (receiver.stopped()) { *error = "stopped"; return false; }
            if (cut_after > 0 && at - offset >= cut_after) { *error = "connection reset"; return false; }
            if (down_after > 0 && at - offset >= down_after) { down = true; *error = "connection reset"; return false; }
            if (stall_from > 0 && at >= stall_from) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const std::size_t part = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_size, size - at));
            for (std::size_t i = 0; i < part; ++i) chunk[i] = Byte(at + i);
            std::this_thread::sleep_for(std::chrono::microseconds(20));  // a network
            if (!receiver.take(chunk, part)) { *error = "stopped"; return false; }
            at += part;
        }
        return true;
    }
};

static void Check(StreamFile& file, std::uint64_t offset, std::size_t size) {
    std::vector<std::uint8_t> got(size);
    const std::size_t read = file.Read(got.data(), size, offset);
    const std::uint64_t expect = offset >= file.Size() ? 0 : std::min<std::uint64_t>(size, file.Size() - offset);
    assert(read == expect);
    for (std::size_t i = 0; i < read; ++i) assert(got[i] == Byte(offset + i));
}

int main() {
    constexpr std::uint64_t kSize = 100 * StreamFile::kBlock + 12345;
    const SourceFile name{"7", "Game.nsp", kSize, FileKind::game};
    // A transfer that trickles, a few bytes now and then and never a whole block: given up after
    // kStall and asked again, which serves the read (beside the rest: it takes that long).
    std::thread trickled([&] {
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->trickle = true;
        std::vector<std::string> said;
        std::mutex said_lock;
        StreamFile file(std::move(owned), SourceGame{}, name, 40, [&](const std::string& line) {
            const std::lock_guard lock(said_lock);
            said.push_back(line);
        });
        const auto begun = std::chrono::steady_clock::now();
        Check(file, 2 * StreamFile::kBlock, 1000);
        assert(std::chrono::steady_clock::now() - begun >= StreamFile::kStall && file.Failed().empty());
        const std::lock_guard lock(said_lock);
        assert(said.size() == 1 && said[0].find("sent no whole block for 20 s") != std::string::npos);
    });
    {
        auto owned = std::make_unique<FakeSource>(kSize);
        FakeSource& source = *owned;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        // The header, then on in order: one transfer brings the blocks ahead of the reads.
        Check(file, 0, 0x200);
        for (std::uint64_t at = 0; at < 20 * StreamFile::kBlock; at += 300000) Check(file, at, 300000);
        assert(source.fetches == 1);
        // A jump back to kept blocks costs nothing; a jump far ahead, one transfer.
        Check(file, 15 * StreamFile::kBlock + 1000, 5000);
        assert(source.fetches == 1);
        Check(file, 80 * StreamFile::kBlock + 7, 1 << 20);
        assert(source.fetches == 2);
        // Across the end of the file: the bytes up to it. Past it: none.
        Check(file, kSize - 100, 4096);
        Check(file, kSize + 5, 10);
        // At random, from eight threads at once.
        std::vector<std::thread> readers;
        for (unsigned t = 0; t < 8; ++t)
            readers.emplace_back([&file, t] {
                std::mt19937_64 random(t + 1);
                for (int i = 0; i < 60; ++i) Check(file, random() % kSize, 1 + random() % (3 << 20));
            });
        for (auto& reader : readers) reader.join();
        const auto stats = file.GetStats();
        assert(stats.evicted > 0 && file.Failed().empty());
        std::printf("in order, jumps, the end, 8 threads: %llu transfers, %llu MiB, %llu hits, %llu waits, %llu evicted\n",
                    (unsigned long long)stats.transfers, (unsigned long long)(stats.received >> 20),
                    (unsigned long long)stats.hits, (unsigned long long)stats.waits, (unsigned long long)stats.evicted);
    }
    {
        // A source that fails twice is asked again; the reads get their bytes.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->fail_next = 2;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        Check(file, 5 * StreamFile::kBlock, 1000);
        assert(file.Failed().empty());
    }
    {
        // One that keeps failing: the read ends short, with the reason, instead of waiting for ever,
        // and the log says what came of each transfer.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->fail_next = 1000;
        std::vector<std::string> said;
        std::mutex said_lock;
        StreamFile file(std::move(owned), SourceGame{}, name, 40, [&](const std::string& line) {
            const std::lock_guard lock(said_lock);
            said.push_back(line);
        });
        std::vector<std::uint8_t> got(1000);
        assert(file.Read(got.data(), got.size(), 0) == 0 && file.Failed() == "connection reset");
        const std::lock_guard lock(said_lock);
        assert(said.size() == 5 && said[0].find("asking again in 500 ms (1 of 5)") != std::string::npos &&
               said[4] == "Gave up after 5 transfers in a row brought nothing: connection reset");
    }
    {
        // One that breaks off every transfer before a block is whole: what it sent is dropped each
        // time, so it brings nothing, and the read fails rather than asking again for ever.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->cut_after = 300 * 1024;
        FakeSource& source = *owned;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        std::vector<std::uint8_t> got(1000);
        assert(file.Read(got.data(), got.size(), 7 * StreamFile::kBlock) == 0 && file.Failed() == "connection reset");
        assert(source.fetches == 5);
    }
    {
        // A source that sends a file only from its start cannot be read where it lies.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->from_start_only = true;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        std::vector<std::uint8_t> got(10);
        assert(file.Read(got.data(), got.size(), 3 * StreamFile::kBlock) == 0 && !file.Failed().empty());
    }
    {
        // Closed while a read waits for a source that has stalled: the read ends at once, short;
        // what is kept is still read; closing is no failure.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->stall_from = 2 * StreamFile::kBlock;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        Check(file, 0, 1000);
        std::atomic<bool> ended{false};
        std::size_t read = 1;
        std::thread reader([&] {
            std::vector<std::uint8_t> got(1000);
            read = file.Read(got.data(), got.size(), 5 * StreamFile::kBlock);
            ended = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(!ended);
        const auto closed = std::chrono::steady_clock::now();
        file.Close();
        reader.join();
        assert(read == 0 && std::chrono::steady_clock::now() - closed < std::chrono::seconds(2));
        assert(file.Failed().empty());
        Check(file, 100, 5000);
    }
    {
        // The network goes away after the first blocks, while the game reads only what is kept: no
        // read waited for anything, so nothing failed, however long it is away (each try reading
        // ahead is said in the log); once it is back the next read far ahead is served.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->down_after = 3 * StreamFile::kBlock;
        FakeSource& source = *owned;
        std::vector<std::string> said;
        std::mutex said_lock;
        StreamFile file(std::move(owned), SourceGame{}, name, 40, [&](const std::string& line) {
            const std::lock_guard lock(said_lock);
            said.push_back(line);
        });
        Check(file, 0, 4096);
        for (int i = 0; i < 90; ++i) {
            Check(file, 100, 4096);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        assert(source.down && file.Failed().empty());
        {
            const std::lock_guard lock(said_lock);
            assert(!said.empty() && said[0].find("reading ahead again in") != std::string::npos);
            for (const std::string& line : said) assert(line.find("Gave up") == std::string::npos);
        }
        source.down = false;
        Check(file, 60 * StreamFile::kBlock + 3, 100000);
        assert(file.Failed().empty());
    }
    {
        // Another thread reads a kept header again and again while one reads 100 MiB in order: the
        // reading ahead follows the one in order, one transfer.
        constexpr std::uint64_t kLong = 150 * StreamFile::kBlock;
        auto owned = std::make_unique<FakeSource>(kLong);
        FakeSource& source = *owned;
        StreamFile file(std::move(owned), SourceGame{}, SourceFile{"7", "Game.nsp", kLong, FileKind::game}, 200);
        Check(file, 0, 512);
        std::atomic<bool> done{false};
        std::thread header([&] {
            while (!done) {
                Check(file, 0, 512);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        for (std::uint64_t at = 0; at < 100 * StreamFile::kBlock; at += 256 * 1024) Check(file, at, 256 * 1024);
        done = true;
        header.join();
        assert(source.fetches <= 2);
    }
    {
        // Two places read in order at once: they share the transfers in stretches, not one block a
        // transfer. Each block takes its own size, whatever pieces the source sends it in.
        constexpr std::uint64_t kLong = 200 * StreamFile::kBlock;
        auto owned = std::make_unique<FakeSource>(kLong);
        owned->chunk_size = 16 * 1024 + 5;
        FakeSource& source = *owned;
        StreamFile file(std::move(owned), SourceGame{}, SourceFile{"7", "Game.nsp", kLong, FileKind::game}, 200);
        std::thread second([&] {
            for (std::uint64_t at = 120 * StreamFile::kBlock; at < 150 * StreamFile::kBlock; at += 128 * 1024)
                Check(file, at, 128 * 1024);
        });
        for (std::uint64_t at = 0; at < 60 * StreamFile::kBlock; at += 128 * 1024) Check(file, at, 128 * 1024);
        second.join();
        const auto stats = file.GetStats();
        std::printf("two places in order (60 + 30 MiB): %d transfers\n", source.fetches.load());
        assert(source.fetches <= 45);
        assert(stats.kept > 0 && stats.memory == stats.kept * StreamFile::kBlock);
    }
    {
        // While memory is short, only the reading ahead stays kept.
        auto owned = std::make_unique<FakeSource>(kSize);
        StreamFile file(std::move(owned), SourceGame{}, name, 90);
        std::atomic<bool> short_of_memory{false};
        file.KeepLessWhen([&] { return short_of_memory.load(); });
        for (std::uint64_t at = 0; at < 50 * StreamFile::kBlock; at += 512 * 1024) Check(file, at, 512 * 1024);
        assert(file.GetStats().kept > StreamFile::kAhead + 2);
        short_of_memory = true;
        for (std::uint64_t at = 50 * StreamFile::kBlock; at < 60 * StreamFile::kBlock; at += 512 * 1024) Check(file, at, 512 * 1024);
        assert(file.GetStats().kept <= StreamFile::kAhead + 2);
        Check(file, 55 * StreamFile::kBlock, 1000);
    }
    {
        // A source that answers with another part of the file than was asked for, or with a file of
        // another size than it listed (what a server whose file shrank comes to): the read fails,
        // saying so, at once.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->told_length = 1000;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        std::vector<std::uint8_t> got(10);
        assert(file.Read(got.data(), got.size(), 4 * StreamFile::kBlock) == 0 &&
               file.Failed().find("another part of the game's file") != std::string::npos);
        auto shorter = std::make_unique<FakeSource>(kSize);
        shorter->from_start_only = true;
        shorter->told_length = kSize - 5000;
        StreamFile shrunk(std::move(shorter), SourceGame{}, name, 40);
        assert(shrunk.Read(got.data(), got.size(), 4 * StreamFile::kBlock) == 0 &&
               shrunk.Failed().find("bytes, not the " + std::to_string(kSize) + " it was listed with") != std::string::npos);
    }
    {
        // Nothing thrown by the source, nor by the log, ends the program: the reads fail, saying so.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->throws = true;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        std::vector<std::uint8_t> got(10);
        assert(file.Read(got.data(), got.size(), 0) == 0 && file.Failed().starts_with("The transfer stopped: "));
        auto failing = std::make_unique<FakeSource>(kSize);
        failing->fail_next = 1000;
        StreamFile logged(std::move(failing), SourceGame{}, name, 40, [](const std::string&) { throw std::bad_alloc(); });
        assert(logged.Read(got.data(), got.size(), 0) == 0 && logged.Failed().starts_with("Reading the file stopped: "));
    }
    {
        // Closed while a transfer runs and a read waits: both end.
        auto owned = std::make_unique<FakeSource>(kSize);
        auto file = std::make_unique<StreamFile>(std::move(owned), SourceGame{}, name, 40);
        std::thread reader([&] { std::vector<std::uint8_t> got(1 << 20); (void)file->Read(got.data(), got.size(), 9 * StreamFile::kBlock); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        reader.join();
        file.reset();
    }
    trickled.join();
    std::puts("ok");
}
'''


def run(exe):
    """Runs a check's program. Under a kernel that places memory more randomly than an older
    ThreadSanitizer knows (Ubuntu 24.04's with GCC 13: "unexpected memory mapping"), the same
    program without address randomization."""
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
    if result.returncode != 0 and 'unexpected memory mapping' in result.stderr and shutil.which('setarch'):
        result = subprocess.run(['setarch', platform.machine(), '-R', str(exe)], capture_output=True, text=True,
                                timeout=600)
    return result


with tempfile.TemporaryDirectory() as tmp:
    for flags, label in ((['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'], 'checked'),
                         (['-O1', '-g', '-fsanitize=thread'], 'thread sanitizer')):
        exe = Path(tmp) / ('stream-' + label.split()[0])
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I' + str(remote), '-x', 'c++', '-', str(remote / 'stream_file.cpp'), '-o', str(exe)],
                       input=code, text=True, check=True)
        result = run(exe)
        sys.stdout.write(f'[{label}] ' + result.stdout.splitlines()[0] + '\n' if result.stdout else '')
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-3000:])
            sys.exit(f'stream file check failed ({label}): exit {result.returncode}')
print('Stream file: PASS every byte right in order, at random and from 8 threads, blocks read ahead by one transfer, '
      'a jump one more, the cache bounded, a failing source retried and logged, a broken, breaking-off or start-only '
      'one failing the read, a close ending a waiting read; a source away while only kept blocks are read failing '
      'nothing, reading ahead not held back by a reread header, two places sharing transfers in stretches, blocks of '
      'their own size, less kept while memory is short, a wrong part or size failing the read, a trickle given up, '
      'nothing thrown ending the program')
