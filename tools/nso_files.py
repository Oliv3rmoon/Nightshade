# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Switch program files as the checks need them, made from scratch in their real formats.

NSO (text, rodata and data, each stored raw, LZ4 or ZBIC; segment hashes), NRO assets (ASET:
icon, NACP), NPDM, KIP. The compressors are the ones Eden's build fetches: lz4 and kinnay/zbic,
whose zstd.c is Atmosphere's ZBIC variant of zstd 1.5.7 (ZSTD_ZBIC_SUPPORT: the frame magic is
"ZBIC" and the FSE tables are stored with binary interpolative coding). They are compiled here
from the Eden source tree's package cache (.cache/cpm).

Used by tools/check-zbic.py, tools/check-direct-formats.py and tools/check-nso-memory.py.
"""
import hashlib
import os
import struct
import subprocess
from pathlib import Path

ZBIC_MAGIC = b'ZBIC'                    # 0x4349425A, little-endian
ZSTD_MAGIC = bytes.fromhex('28b52ffd')  # 0xFD2FB528
NSO_ZBIC = 0x80                         # NSO flags bit 7: compressed segments are ZBIC, not LZ4

ENCODER = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef USE_LZ4
#include "lz4.h"
#else
#include "zstd.h"
#endif
/* encode <in> <out> [level]: the whole file as one frame (zstd, ZBIC) or one block (LZ4). */
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* in = fopen(argv[1], "rb");
    if (!in) return 2;
    fseek(in, 0, SEEK_END);
    long size = ftell(in);
    fseek(in, 0, SEEK_SET);
    char* data = malloc(size ? size : 1);
    if (fread(data, 1, size, in) != (size_t)size) return 2;
    fclose(in);
#ifdef USE_LZ4
    int bound = LZ4_compressBound((int)size);
    char* out = malloc(bound);
    int written = LZ4_compress_default(data, out, (int)size, bound);
    if (written <= 0) return 1;
#else
    size_t bound = ZSTD_compressBound(size);
    char* out = malloc(bound);
    size_t written = ZSTD_compress(out, bound, data, size, argc > 3 ? atoi(argv[3]) : 3);
    if (ZSTD_isError(written)) {
        fprintf(stderr, "%s\n", ZSTD_getErrorName(written));
        return 1;
    }
#endif
    FILE* file = fopen(argv[2], "wb");
    if (!file || fwrite(out, 1, written, file) != (size_t)written || fclose(file)) return 2;
    return 0;
}
'''


def package(eden, name, inside):
    """The one folder of a package in the Eden source tree's cache that has `inside`."""
    found = sorted(p for p in (Path(eden) / '.cache/cpm' / name).glob('*') if (p / inside).exists())
    if len(found) != 1:
        raise SystemExit(f'{name}: expected one package with {inside} in {Path(eden) / ".cache/cpm" / name}, found {found}')
    return found[0]


def zbic_source(eden):
    return package(eden, 'zbic', 'src/zstd.c') / 'src'


def lz4_source(eden):
    """lz4's lib/ in the package cache, or None where the build took the system's liblz4 (the Linux
    host build: "Using system package lz4")."""
    found = sorted(p for p in (Path(eden) / '.cache/cpm/lz4').glob('*') if (p / 'lib/lz4.c').exists())
    return found[0] / 'lib' if len(found) == 1 else None


def build_codecs(eden, work, names=('zbic', 'zstd', 'lz4')):
    """Compiles the encoders named: 'zbic', 'zstd' (the same zstd.c without ZBIC), 'lz4' -> executable."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'encode.c'
    source.write_text(ENCODER)
    cc = os.environ.get('CC', 'cc')
    tools = {}
    for name in names:
        if name in ('zbic', 'zstd'):
            zbic = zbic_source(eden)
            flags = ['-DZSTD_ZBIC_SUPPORT=%d' % (name == 'zbic'), '-I' + str(zbic)]
            extra = [str(zbic / 'zstd.c'), '-lpthread']
        else:
            lz4 = lz4_source(eden)
            flags = ['-DUSE_LZ4'] + (['-I' + str(lz4)] if lz4 else [])
            extra = [str(lz4 / 'lz4.c')] if lz4 else ['-llz4']
        tool = work / f'{name}-encode'
        subprocess.run([cc, '-O1', '-w', *flags, str(source), *extra, '-o', str(tool)], check=True)
        tools[name] = tool
    return tools


def compress(tool, data, work, level=None):
    work = Path(work)
    raw, packed = work / 'in.bin', work / 'out.bin'
    raw.write_bytes(data)
    subprocess.run([str(tool), str(raw), str(packed)] + ([str(level)] if level is not None else []), check=True)
    return packed.read_bytes()


def page(size):
    return (size + 0xFFF) & ~0xFFF


def nro_segments(nro):
    """The text, rodata and data of an NRO (where they are in memory) and its BSS size."""
    assert nro[0x10:0x14] == b'NRO0', 'not an NRO'
    text, text_size, ro, ro_size, rw, rw_size, bss = struct.unpack_from('<7I', nro, 0x20)
    return [(nro[text:text + text_size], text), (nro[ro:ro + ro_size], ro), (nro[rw:rw + rw_size], rw)], bss


def make_nso(segments, bss, codec=None, tools=None, work=None, build_id=None, level=3):
    """An NSO: segments [(bytes, memory offset)] for text, rodata, data, stored raw or compressed
    with codec 'lz4' or 'zbic', with their SHA-256 hashes (checked by the console's loader)."""
    stored, flags = [], 0x38  # check the hash of each segment
    for index, (data, _) in enumerate(segments):
        if codec:
            stored.append(compress(tools[codec], data, work, level if codec == 'zbic' else None))
            flags |= 1 << index
        else:
            stored.append(data)
    if codec == 'zbic':
        flags |= NSO_ZBIC
    header = bytearray(0x100)
    struct.pack_into('<4sIII', header, 0, b'NSO0', 0, 0, flags)
    offset = 0x100
    for index, ((data, location), body) in enumerate(zip(segments, stored)):
        # The text's fourth word is its module name's offset, the rodata's that name's size, the
        # data's the BSS size.
        struct.pack_into('<4I', header, 0x10 + 0x10 * index, offset, location, len(data), bss if index == 2 else 0)
        struct.pack_into('<I', header, 0x60 + 4 * index, len(body))
        header[0xA0 + 0x20 * index:0xC0 + 0x20 * index] = hashlib.sha256(data).digest()
        offset += len(body)
    header[0x40:0x60] = build_id or hashlib.sha256(b''.join(d for d, _ in segments)).digest()
    return bytes(header) + b''.join(stored)


def read_nso(nso):
    """The header fields of an NSO: flags, [(file offset, memory offset, size, stored size, hash)], BSS."""
    magic, _, _, flags = struct.unpack_from('<4sIII', nso, 0)
    assert magic == b'NSO0', 'not an NSO'
    segments = []
    for index in range(3):
        offset, location, size, extra = struct.unpack_from('<4I', nso, 0x10 + 0x10 * index)
        stored = struct.unpack_from('<I', nso, 0x60 + 4 * index)[0]
        segments.append((offset, location, size, stored, nso[0xA0 + 0x20 * index:0xC0 + 0x20 * index]))
    return flags, segments, struct.unpack_from('<I', nso, 0x1C + 0x20)[0]


def make_nacp(names, title_id, languages=0):
    """A NACP: names for the first languages (AmericanEnglish first), the title ID Eden reads from it
    (SaveDataOwnerId), the supported-language flags."""
    nacp = bytearray(0x4000)
    for index, name in enumerate(names):
        encoded, developer = name.encode(), b'Oliv3rmoon'
        nacp[index * 0x300:index * 0x300 + len(encoded)] = encoded
        nacp[index * 0x300 + 0x200:index * 0x300 + 0x200 + len(developer)] = developer
    struct.pack_into('<I', nacp, 0x302C, languages)
    struct.pack_into('<Q', nacp, 0x3078, title_id)
    return bytes(nacp)


def make_aset(icon, nacp, romfs=b''):
    """NRO assets (ASET), appended to an NRO at its file size: icon (JPEG), NACP, RomFS."""
    header_size = 0x38
    parts, offset, sections = [icon, nacp, romfs], header_size, []
    for part in parts:
        sections.append((offset if part else 0, len(part)))
        offset += len(part)
    header = struct.pack('<4sI', b'ASET', 0) + b''.join(struct.pack('<QQ', *s) for s in sections)
    return header + b''.join(parts)


def make_npdm(title_id, name=b'Fixture'):
    """A main.npdm for a 64-bit application in the 39-bit address space, with the default
    metadata's thread capability (priorities 16 to 63, cores 0 to 3) as Eden's GetDefault has."""
    npdm = bytearray(0x380)
    thread_info = 0x30043F7
    struct.pack_into('<4s', npdm, 0, b'META')
    npdm[0xC] = 1 | (3 << 1)  # 64-bit instructions, 39-bit address space
    npdm[0xE], npdm[0xF] = 0x2C, 0  # main thread priority, core
    struct.pack_into('<I', npdm, 0x1C, 0x100000)  # main thread stack
    npdm[0x20:0x20 + len(name)] = name
    # ACI0 at 0x80 (its offsets are relative to it), ACID at 0x100.
    struct.pack_into('<IIII', npdm, 0x70, 0x80, 0x80, 0x100, 0x280)
    struct.pack_into('<4s', npdm, 0x80, b'ACI0')
    struct.pack_into('<Q', npdm, 0x90, title_id)
    struct.pack_into('<6I', npdm, 0xA0, 0x40, 0x1C, 0x5C, 0, 0x60, 4)
    npdm[0xC0] = 1  # FileAccessHeader: version 1, every permission
    struct.pack_into('<QIIII', npdm, 0xC4, 0xFFFFFFFFFFFFFFFF, 0x1C, 0, 0x1C, 0)
    struct.pack_into('<I', npdm, 0xE0, thread_info)
    struct.pack_into('<4sI', npdm, 0x300, b'ACID', 0x280)
    struct.pack_into('<IQQ', npdm, 0x30C, 0, title_id, title_id)  # flags (application pool), title IDs
    struct.pack_into('<6I', npdm, 0x320, 0x240, 0x2C, 0x26C, 0, 0x270, 4)
    npdm[0x340] = 1  # FileAccessControl: version 1, every permission
    struct.pack_into('<Q', npdm, 0x344, 0xFFFFFFFFFFFFFFFF)
    struct.pack_into('<I', npdm, 0x370, thread_info)
    return bytes(npdm)


def make_kip(segments, bss, title_id, name=b'Fixture'):
    """A KIP1 (uncompressed sections): text, rodata, data at their memory offsets, then the BSS."""
    header = bytearray(0x100)
    struct.pack_into('<4s12sQI', header, 0, b'KIP1', name, title_id, 0)
    header[0x1C], header[0x1D], header[0x1F] = 0x2C, 0, 0x08 | 0x10  # priority, core, 64-bit, 39-bit
    end = 0
    for index, (data, location) in enumerate(segments):
        attribute = 0x100000 if index == 1 else 0  # the rodata's attribute is the main thread's stack
        struct.pack_into('<4I', header, 0x20 + 0x10 * index, location, len(data), len(data), attribute)
        end = max(end, location + len(data))
    struct.pack_into('<4I', header, 0x20 + 0x10 * 3, page(end), bss, 0, 0)
    capabilities = [0x30043F7] + [0xFFFFFFFF] * 31
    struct.pack_into('<32I', header, 0x80, *capabilities)
    return bytes(header) + b''.join(data for data, _ in segments)
