#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of the community's invite (headless/prosperoeden/pe/ui/community.hpp): the Discord
server About names, and the QR code About and the card at the first start draw.

Builds a small program with the launcher's own QR encoder (pe/ui/qr_code.cpp, Project Nayuki's
qrcodegen) under the address and undefined-behaviour sanitizers, warnings as errors, and runs it:
it prints the server's name, its address and the modules of community_qr(), the code the launcher
draws. That code is then read back here as a phone reads it, without the encoder: its format
(error correction level and mask, both copies), its codewords with their Reed-Solomon error
correction, which must agree, and the text they hold, which must be exactly the invite's address.

Then About itself (pe/ui/browse.cpp): each of its two layouts (the section tabs at the top, and
the rail) and the card (pe/ui/community.cpp) draw the server's name, its address and that code.

usage: tools/check-community.py
CXX names another compiler.
"""
from pathlib import Path
import os, re, subprocess, sys, tempfile

URL = 'https://discord.gg/pSqgRU3YP9'
LINK = 'discord.gg/pSqgRU3YP9'
NAME = 'nightly'

root = Path(__file__).resolve().parents[1]
launcher = root / 'headless/prosperoeden'
flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Wall', '-Wextra', '-Werror',
         '-UNDEBUG']

PROGRAM = r'''#include "pe/ui/community.hpp"
#include <cassert>
#include <cstdio>
#include <string>
using namespace pe::ui;
int main() {
    const QrCode &code = community_qr();
    // Made once: About and the card draw the same code.
    assert(&code == &community_qr());
    assert(code.size >= 21 && (code.size - 17) % 4 == 0);
    assert(code.dark.size() == static_cast<std::size_t>(code.size * code.size));
    // The address shown is the one the code holds, without its scheme.
    assert(std::string(kCommunityUrl) == "https://" + std::string(kCommunityLink));
    std::printf("name %s\nlink %s\nurl %s\n", kCommunityName, kCommunityLink, kCommunityUrl);
    for (int y = 0; y < code.size; ++y) {
        std::string row;
        for (int x = 0; x < code.size; ++x)
            row += code.at(x, y) ? '1' : '0';
        std::printf("row %s\n", row.c_str());
    }
}
'''

# ---- reading a QR code (ISO/IEC 18004), versions 1 to 6: no version information, one alignment
# pattern at most. Coordinates are (x, y): column, row.

# Error correction codewords per block, and blocks, by level and version (1..6).
ECC_PER_BLOCK = {'L': [7, 10, 15, 20, 26, 18], 'M': [10, 16, 26, 18, 24, 16],
                 'Q': [13, 22, 18, 26, 18, 24], 'H': [17, 28, 22, 16, 22, 28]}
BLOCKS = {'L': [1, 1, 1, 1, 1, 2], 'M': [1, 1, 1, 2, 2, 4], 'Q': [1, 1, 2, 2, 4, 4], 'H': [1, 1, 2, 4, 4, 4]}
CODEWORDS = [26, 44, 70, 100, 134, 172]  # all of a version's codewords
LEVELS = {1: 'L', 0: 'M', 3: 'Q', 2: 'H'}  # the format's two level bits
MASKS = [lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
         lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
         lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
         lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0]
ALPHANUMERIC = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:'


def gf_multiply(x, y):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z & 0xFF


def ecc_of(data, degree):
    """The Reed-Solomon error correction codewords of `data` (generator with roots 2^0..2^(degree-1))."""
    divisor = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(degree):
            divisor[j] = gf_multiply(divisor[j], root)
            if j + 1 < degree:
                divisor[j] ^= divisor[j + 1]
        root = gf_multiply(root, 0x02)
    remainder = [0] * degree
    for byte in data:
        factor = byte ^ remainder.pop(0)
        remainder.append(0)
        for i, coefficient in enumerate(divisor):
            remainder[i] ^= gf_multiply(coefficient, factor)
    return remainder


def format_word(bits15):
    """The format's 5 data bits when its 15 bits are a valid BCH code word, else None."""
    word = bits15 ^ 0x5412
    data = word >> 10
    remainder = data
    for _ in range(10):
        remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537)
    return data if (data << 10 | remainder) == word else None


def read(modules):
    size = len(modules)
    assert all(len(row) == size for row in modules), 'the code is not square'
    version = (size - 17) // 4
    assert size == 17 + 4 * version and 1 <= version <= 6, f'size {size}: not a version 1 to 6 code'
    dark = lambda x, y: modules[y][x]

    # The format, both copies (around the top-left finder; beside the other two).
    first = sum(dark(8, i) << i for i in range(6)) | dark(8, 7) << 6 | dark(8, 8) << 7 | dark(7, 8) << 8
    first |= sum(dark(14 - i, 8) << i for i in range(9, 15))
    second = sum(dark(size - 1 - i, 8) << i for i in range(8))
    second |= sum(dark(8, size - 15 + i) << i for i in range(8, 15))
    assert first == second, 'the two copies of the format differ'
    data_bits = format_word(first)
    assert data_bits is not None, 'the format is not a valid BCH code word'
    level, mask = LEVELS[data_bits >> 3], data_bits & 7
    assert dark(8, size - 8), 'the dark module is light'

    # Where the function patterns are: finders, separators and formats, timing, alignment.
    function = [[False] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            function[y][x] = ((x <= 8 and y <= 8) or (x >= size - 8 and y <= 8) or (x <= 8 and y >= size - 8)
                              or x == 6 or y == 6)
    if version >= 2:
        at = size - 7  # the one alignment pattern of versions 2 to 6
        for y in range(at - 2, at + 3):
            for x in range(at - 2, at + 3):
                function[y][x] = True
    # The finders as they must be (a damaged reading would not pass the rest either).
    for cx, cy in ((3, 3), (size - 4, 3), (3, size - 4)):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    ring = max(abs(dx), abs(dy))
                    assert dark(x, y) == (ring not in (2, 4)), f'finder at ({cx}, {cy}) is not one'
    for i in range(8, size - 8):
        assert dark(i, 6) == (i % 2 == 0) and dark(6, i) == (i % 2 == 0), 'the timing patterns are wrong'

    # The codewords, read in the two-column zigzag from the bottom right, unmasked.
    bits = []
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vertical in range(size):
            for j in range(2):
                x = right - j
                upward = ((right + 1) & 2) == 0
                y = size - 1 - vertical if upward else vertical
                if not function[y][x]:
                    bits.append(dark(x, y) ^ MASKS[mask](x, y))
        right -= 2
    total = CODEWORDS[version - 1]
    assert len(bits) >= total * 8, 'fewer data modules than the version has codewords'
    codewords = [int(''.join('1' if b else '0' for b in bits[i * 8:i * 8 + 8]), 2) for i in range(total)]

    # Blocks: the short ones first; data interleaved, then error correction interleaved.
    blocks = BLOCKS[level][version - 1]
    ecc_length = ECC_PER_BLOCK[level][version - 1]
    short = blocks - total % blocks
    short_data = total // blocks - ecc_length
    lengths = [short_data + (0 if i < short else 1) for i in range(blocks)]
    data = [[] for _ in range(blocks)]
    ecc = [[] for _ in range(blocks)]
    stream = iter(codewords)
    for i in range(max(lengths)):
        for j in range(blocks):
            if i < lengths[j]:
                data[j].append(next(stream))
    for _ in range(ecc_length):
        for j in range(blocks):
            ecc[j].append(next(stream))
    for j in range(blocks):
        assert ecc_of(data[j], ecc_length) == ecc[j], f'block {j}: its error correction does not match its data'

    # The segments: numeric, alphanumeric or bytes (ISO-8859-1 or UTF-8), to the terminator.
    stream = ''.join(f'{byte:08b}' for block in data for byte in block)
    position = 0

    def take(count):
        nonlocal position
        value = int(stream[position:position + count], 2) if count else 0
        position += count
        return value

    text = b''
    while position + 4 <= len(stream):
        mode = take(4)
        if mode == 0:
            break
        if mode == 4:
            text += bytes(take(8) for _ in range(take(8)))
        elif mode == 2:
            count = take(9)
            for _ in range(count // 2):
                pair = take(11)
                text += (ALPHANUMERIC[pair // 45] + ALPHANUMERIC[pair % 45]).encode()
            if count % 2:
                text += ALPHANUMERIC[take(6)].encode()
        elif mode == 1:
            count = take(10)
            for _ in range(count // 3):
                text += f'{take(10):03d}'.encode()
            if count % 3:
                text += f'{take(4 if count % 3 == 1 else 7):0{count % 3}d}'.encode()
        else:
            raise AssertionError(f'segment mode {mode} is not one a URL is written in')
    return text.decode('utf-8'), version, level, mask


def check_sources():
    """About's two layouts and the card draw the server's name, its address and community_qr()."""
    problems = []
    browse = (launcher / 'pe/ui/browse.cpp').read_text(encoding='utf-8')
    about = browse[browse.index('void Launcher::draw_about(Canvas &c)'):]
    about = about[:about.index('\n}\n')]
    tabs, rail = about.split('    if (th().top_nav)\n', 1)[1].split('\n        return;\n    }\n', 1)
    card = (launcher / 'pe/ui/community.cpp').read_text(encoding='utf-8')
    card = card[card.index('void Launcher::draw_community(Canvas &c, float open)'):]
    for name, body in (('About with the section tabs', tabs), ('About with the rail', rail), ('the card', card)):
        for needed in ('kCommunityName', 'kCommunityLink', 'qr_code(c, community_qr(),'):
            if needed not in body:
                problems.append(f'{name} does not draw {needed}')
    if 'tr("JOIN THE COMMUNITY")' not in tabs or 'tr("JOIN THE COMMUNITY")' not in rail:
        problems.append('About does not head the community with JOIN THE COMMUNITY')
    header = (launcher / 'pe/ui/community.hpp').read_text(encoding='utf-8')
    for constant, value in (('kCommunityName', NAME), ('kCommunityLink', LINK), ('kCommunityUrl', URL)):
        if not re.search(rf'{constant} = "{re.escape(value)}";', header):
            problems.append(f'community.hpp: {constant} is not "{value}"')
    if '"${launcher_dir}/pe/ui/community.cpp"' not in (root / 'headless/CMakeLists.txt').read_text(encoding='utf-8'):
        problems.append("headless/CMakeLists.txt does not build pe/ui/community.cpp into the app's launcher")
    return problems


with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'community.cpp'
    source.write_text(PROGRAM)
    exe = Path(tmp) / 'eden-community-check'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', *flags, '-I' + str(launcher),
                    '-I' + str(root / 'third_party'), str(source), str(launcher / 'pe/ui/qr_code.cpp'), '-o', str(exe)],
                   check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
    if result.returncode != 0:
        sys.stderr.write(result.stdout[-2000:] + result.stderr[-3000:])
        sys.exit(f'community check failed: exit {result.returncode}')

lines = result.stdout.splitlines()
said = dict(line.split(' ', 1) for line in lines if not line.startswith('row '))
modules = [[c == '1' for c in line[4:]] for line in lines if line.startswith('row ')]
problems = []
for key, value in (('name', NAME), ('link', LINK), ('url', URL)):
    if said.get(key) != value:
        problems.append(f'the {key} is {said.get(key)!r}, not {value!r}')
try:
    text, version, level, mask = read(modules)
    if text != URL:
        problems.append(f'the QR code holds {text!r}, not {URL!r}')
    else:
        print(f'PASS QR code: version {version} ({len(modules)}x{len(modules)}), level {level}, mask {mask}, '
              f'error correction matches, holds exactly {text}')
except AssertionError as error:
    problems.append(f'the QR code cannot be read: {error}')
problems += check_sources()
if problems:
    sys.exit('community check failed:\n  ' + '\n  '.join(problems))
print(f'PASS About (both layouts) and the card draw {NAME}, {LINK} and that QR code')
