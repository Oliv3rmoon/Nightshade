#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check ZBIC: the NSO segment compression of Switch 22.0.0 and later, as Eden decodes it.

ZBIC is zstd 1.5.7 with two changes (Atmosphere's loader, kinnay/zbic): the frame magic is "ZBIC"
(0x4349425A) instead of 0xFD2FB528, and the FSE tables a block stores are written with binary
interpolative coding instead of zstd's own normalized counts. An NSO marks it with flag bit 7: its
compressed segments are ZBIC frames instead of LZ4 blocks.

This compiles Eden's own decoder (src/common/zbic_compression.cpp with the zbic package's zstd.c,
as the build does) and the ZBIC and plain zstd encoders of that zstd.c, makes code-like, string
and data segments, and checks that:
  - each ZBIC frame starts with "ZBIC", and the same data compressed by plain zstd differs from it
    after the magic (the BIC tables are really in the samples, not only the magic);
  - Eden decodes every ZBIC frame back to the exact bytes, at four compression levels and over many
    blocks;
  - it refuses a plain zstd frame, a plain frame given the ZBIC magic, a truncated or damaged frame,
    and a buffer too small;
  - given a buffer larger than the segment it reports the frame's real size, which is why the NSO
    loader compares the result with the segment's size (headless/CMakeLists.txt);
  - a whole NSO made the way the console's are (flags 0x80 | compressed | hashed, SHA-256 of each
    segment in its header) decodes segment by segment to the hashes in its header.

Usage: check-zbic.py <Eden source tree> (the build cache's source/, with its .cache/cpm).
"""
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import hashlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nso_files  # noqa: E402

DECODER = r'''
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>
#include "common/zbic_compression.h"
// decode <in> <capacity> <out>: Eden's DecompressDataZBIC into a buffer of <capacity> bytes. Prints
// its result and IsZBIC, and writes what it decoded.
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<u8> source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<u8> output(std::strtoull(argv[2], nullptr, 0));
    const int result = Common::Compression::DecompressDataZBIC(output, source);
    std::printf("%d %d\n", result, Common::Compression::IsZBIC(source) ? 1 : 0);
    if (result > 0) std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char*>(output.data()), result);
    return 0;
}
'''


def compiler():
    for name in (os.environ.get('CXX'), 'clang++-18', 'clang++', 'g++'):
        if name and shutil.which(name):
            return name
    raise SystemExit('no C++ compiler')


def code_like(size, seed):
    """AArch64-looking words: functions of moves, loads, stores, adds, compares, branches and calls."""
    rnd = random.Random(seed)
    words = []
    while len(words) * 4 < size:
        words += [0xA9BF7BFD, 0x910003FD]  # stp x29, x30, [sp, #-16]!; mov x29, sp
        for _ in range(rnd.randrange(4, 40)):
            kind = rnd.randrange(8)
            reg = rnd.randrange(31)
            if kind == 0:
                words.append(0xD2800000 | rnd.randrange(1 << 12) << 5 | reg)       # mov xN, #imm
            elif kind == 1:
                words.append(0xF9400000 | rnd.randrange(64) << 10 | rnd.randrange(31) << 5 | reg)  # ldr
            elif kind == 2:
                words.append(0xF9000000 | rnd.randrange(64) << 10 | rnd.randrange(31) << 5 | reg)  # str
            elif kind == 3:
                words.append(0x91000000 | rnd.randrange(256) << 10 | rnd.randrange(31) << 5 | reg)  # add
            elif kind == 4:
                words.append(0xEB00001F | rnd.randrange(31) << 16 | reg << 5)       # cmp
            elif kind == 5:
                words.append(0x54000000 | rnd.randrange(64) << 5 | rnd.randrange(14))  # b.cond
            elif kind == 6:
                words.append(0x94000000 | rnd.randrange(1 << 20))                   # bl
            else:
                words.append(0xAA0003E0 | rnd.randrange(31) << 16 | reg)            # mov xN, xM
        words += [0xA8C17BFD, 0xD65F03C0]  # ldp x29, x30, [sp], #16; ret
    return struct.pack(f'<{len(words)}I', *words)[:size]


def strings_like(size, seed):
    rnd = random.Random(seed)
    vocabulary = [b'nn::fs::', b'MountRom', b'Result', b'0x%08x', b'Effect/', b'.bfres', b'Layout/', b'player',
                  b'enemy_', b'Shader', b'Texture', b'%s/%s', b'Failed to open %s', b'Actor', b'Model/', b'.sarc']
    out = bytearray()
    while len(out) < size:
        if rnd.random() < 0.8:
            out += b''.join(rnd.choice(vocabulary) for _ in range(rnd.randrange(1, 5))) + b'\0'
        else:
            out += struct.pack('<f', rnd.uniform(-1000, 1000))
    return bytes(out[:size])


def data_like(size, seed):
    rnd = random.Random(seed)
    out = bytearray(size)
    for offset in range(0, size - 8, 8):
        if rnd.random() < 0.3:
            struct.pack_into('<Q', out, offset, 0x8000000 + rnd.randrange(1 << 20) * 8)
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    eden = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix='zbic-') as tmp:
        work = Path(tmp)
        tools = nso_files.build_codecs(eden, work / 'tools')
        (work / 'decode.cpp').write_text(DECODER)
        decoder = work / 'zbic-decode'
        # Eden's own decoder, built as the build builds it: the zbic package's zstd.c included in it.
        subprocess.run([compiler(), '-std=c++20', '-O1', '-w', '-I' + str(eden / 'src'),
                        '-I' + str(nso_files.zbic_source(eden)), str(eden / 'src/common/zbic_compression.cpp'),
                        str(work / 'decode.cpp'), '-o', str(decoder), '-lpthread'], check=True)

        def decode(frame, capacity):
            (work / 'frame.bin').write_bytes(frame)
            out = work / 'decoded.bin'
            out.unlink(missing_ok=True)
            result, is_zbic = map(int, subprocess.run([str(decoder), str(work / 'frame.bin'), str(capacity), str(out)],
                                                      check=True, capture_output=True, text=True).stdout.split())
            return result, is_zbic, out.read_bytes() if result > 0 else b''

        samples = {'text': code_like(1 << 20, 1), 'rodata': strings_like(192 << 10, 2), 'data': data_like(48 << 10, 3)}
        bic_tables = 0
        checks = 0
        for name, data in samples.items():
            for level in (1, 3, 9, 19):
                zbic = nso_files.compress(tools['zbic'], data, work, level)
                plain = nso_files.compress(tools['zstd'], data, work, level)
                label = f'{name} ({len(data)} bytes, level {level})'
                assert zbic[:4] == nso_files.ZBIC_MAGIC and plain[:4] == nso_files.ZSTD_MAGIC, label
                assert len(zbic) < len(data), f'{label}: not compressed'
                # The same compressor but for the tables: what differs after the magic is their coding.
                differs = zbic[4:] != plain[4:]
                bic_tables += differs
                assert decode(zbic, len(data)) == (len(data), 1, data), f'{label}: not decoded to its bytes'
                assert decode(plain, len(data))[0] == -1, f'{label}: a plain zstd frame was taken for ZBIC'
                if differs:
                    result, _, out = decode(nso_files.ZBIC_MAGIC + plain[4:], len(data))
                    assert not (result == len(data) and out == data), f'{label}: plain zstd tables read as BIC'
                assert decode(zbic[:-8], len(data))[0] == -1, f'{label}: a truncated frame decoded'
                assert decode(zbic, len(data) - 1)[0] == -1, f'{label}: decoded into too small a buffer'
                # A frame decodes to its own size, whatever room it has: the loader must check it.
                assert decode(zbic, len(data) + 4096)[0] == len(data), label
                damaged = bytearray(zbic)
                damaged[len(damaged) // 2] ^= 0x5A
                result, _, out = decode(bytes(damaged), len(data))
                assert not (result == len(data) and out == data), f'{label}: a damaged frame decoded to the bytes'
                checks += 1
        assert bic_tables >= 6 and samples, f'only {bic_tables} of {checks} frames had tables the BIC coding changes'

        # An NSO as the console's are: ZBIC segments, each hashed in the header.
        segments, location = [], 0
        for data in samples.values():
            segments.append((data, location))
            location = nso_files.page(location + len(data))
        nso = nso_files.make_nso(segments, 0x10000, 'zbic', tools, work)
        flags, entries, bss = nso_files.read_nso(nso)
        assert flags == 0x80 | 0x38 | 0x7 and bss == 0x10000, hex(flags)
        for index, (offset, memory, size, stored, digest) in enumerate(entries):
            frame = nso[offset:offset + stored]
            assert frame[:4] == nso_files.ZBIC_MAGIC and memory == segments[index][1]
            result, _, out = decode(frame, size)
            assert result == size and hashlib.sha256(out).digest() == digest, f'NSO segment {index}'
    print(f'ZBIC: {checks} frames (code, strings, data; levels 1, 3, 9, 19; up to 1 MiB) decoded by Eden to their '
          f'bytes, {bic_tables} with BIC-coded tables; plain, mislabelled, truncated, damaged and short-buffer '
          'frames refused; a ZBIC NSO decoded to the hashes in its header PASS')


if __name__ == '__main__':
    main()
