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
failing, or sends files only from their start, fails the reads instead of hanging them.
"""
from pathlib import Path
import os, subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
remote = root / 'headless/remote'

code = r'''
#include "stream_file.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
using namespace Eden::Remote;
static std::uint8_t Byte(std::uint64_t at) { return static_cast<std::uint8_t>((at * 2654435761u) >> 13); }

struct FakeSource final : Source {
    std::uint64_t size;
    std::atomic<int> fetches{0};
    std::atomic<int> fail_next{0};      // fetches that bring nothing
    bool from_start_only = false;
    explicit FakeSource(std::uint64_t size_) : size(size_) {}
    std::string address() const override { return "fake"; }
    bool list(std::vector<SourceGame>*, std::string*, const Stopped&) override { return false; }
    bool cover(const SourceGame&, std::string*, const Stopped&) override { return false; }
    bool fetch(const SourceGame&, const SourceFile&, std::uint64_t offset, Receiver& receiver, std::string* error) override {
        ++fetches;
        if (fail_next > 0) { --fail_next; *error = "connection reset"; return false; }
        if (from_start_only) offset = 0;
        if (!receiver.begin(from_start_only)) { *error = "stopped"; return false; }
        std::uint8_t chunk[64 * 1024];
        for (std::uint64_t at = offset; at < size;) {
            if (receiver.stopped()) { *error = "stopped"; return false; }
            const std::size_t part = static_cast<std::size_t>(std::min<std::uint64_t>(sizeof(chunk), size - at));
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
        // One that keeps failing: the read ends short, with the reason, instead of waiting for ever.
        auto owned = std::make_unique<FakeSource>(kSize);
        owned->fail_next = 1000;
        StreamFile file(std::move(owned), SourceGame{}, name, 40);
        std::vector<std::uint8_t> got(1000);
        assert(file.Read(got.data(), got.size(), 0) == 0 && file.Failed() == "connection reset");
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
        // Closed while a transfer runs and a read waits: both end.
        auto owned = std::make_unique<FakeSource>(kSize);
        auto file = std::make_unique<StreamFile>(std::move(owned), SourceGame{}, name, 40);
        std::thread reader([&] { std::vector<std::uint8_t> got(1 << 20); (void)file->Read(got.data(), got.size(), 9 * StreamFile::kBlock); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        reader.join();
        file.reset();
    }
    std::puts("ok");
}
'''

with tempfile.TemporaryDirectory() as tmp:
    for flags, label in ((['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'], 'checked'),
                         (['-O1', '-g', '-fsanitize=thread'], 'thread sanitizer')):
        exe = Path(tmp) / ('stream-' + label.split()[0])
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror', *flags,
                        '-I' + str(remote), '-x', 'c++', '-', str(remote / 'stream_file.cpp'), '-o', str(exe)],
                       input=code, text=True, check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600)
        sys.stdout.write(f'[{label}] ' + result.stdout.splitlines()[0] + '\n' if result.stdout else '')
        if result.returncode != 0:
            sys.stderr.write(result.stderr[-3000:])
            sys.exit(f'stream file check failed ({label}): exit {result.returncode}')
print('Stream file: PASS every byte right in order, at random and from 8 threads, blocks read ahead by one transfer, '
      'a jump one more, the cache bounded, a failing source retried, a broken or start-only one failing the read')
