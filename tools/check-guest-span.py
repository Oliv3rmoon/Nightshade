#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host check of when Eden reads a range of guest memory where it lies (Memory::GetSpan, patched by
headless/CMakeLists.txt to headless/guest_span.h).

Usage: check-guest-span.py [PINNED_CORE_MEMORY_CPP]

Eden took a range as one piece of host memory when its first page and the page after it carried the
same mapping number. Mappings are numbered by a 16-bit count that goes round after 65,536 of them;
from then on, and wherever the middle of a mapping was mapped again, a service could be handed bytes
of memory the range does not cover. This builds a page table the way Eden's MapPages fills it, runs
both rules over the cases, and checks that the build replaces the pinned function (when the pinned
core/memory.cpp is given) with one that asks guest_span.h.
"""
from pathlib import Path
import subprocess, sys, tempfile

root = Path(__file__).resolve().parents[1]
code = r"""
#include "guest_span.h"
#include <cassert>
#include <cstdint>
#include <vector>
// A page table entry as Eden's: the host address of the page less its guest address, a mapping
// number, and a marked bit (a page the GPU caches).
struct Entry {
    std::uintptr_t pointer = 0;
    std::uint16_t block = 0;
    bool marked = false;
    std::uintptr_t Pointer(bool ignore_marked = false) const { return marked && !ignore_marked ? 0 : pointer; }
    std::uint16_t Block() const { return block; }
};
constexpr std::uint64_t Page = 4096;
struct Table {
    std::vector<Entry> entries = std::vector<Entry>(64);
    std::uint16_t count = 0;
    // MapPages: `pages` guest pages from `page` show host memory from `host` on, one number for them.
    void Map(std::uint64_t page, std::uint64_t pages, std::uintptr_t host) {
        const std::uint16_t block = count++;
        for (std::uint64_t i = 0; i < pages; ++i) entries[page + i] = {host - page * Page, block, false};
    }
    void Unmap(std::uint64_t page, std::uint64_t pages) {
        for (std::uint64_t i = 0; i < pages; ++i) entries[page + i] = {};
    }
    // The rules for [addr, addr + size).
    bool Old(std::uint64_t addr, std::uint64_t size) const {
        return entries[addr / Page].Block() == entries[(addr + size) / Page].Block() && entries[addr / Page].Pointer(true) != 0;
    }
    bool New(std::uint64_t addr, std::uint64_t size) const {
        return size != 0 && Eden::GuestSpan::OnePiece(entries, addr / Page, (addr + size - 1) / Page);
    }
};
int main() {
    constexpr std::uintptr_t Host = 0x8000000000;
    {
        // One mapping: one piece, at any offset and size inside it.
        Table table;
        table.Map(4, 8, Host + 100 * Page);
        assert(table.New(4 * Page, 8 * Page) && table.New(5 * Page + 17, 3 * Page) && table.New(11 * Page + 4000, 96));
        // Past its end or before its start: not.
        assert(!table.New(10 * Page, 3 * Page) && !table.New(3 * Page + 10, Page));
        // An unmapped page in it: not.
        table.Unmap(7, 1);
        assert(!table.New(4 * Page, 8 * Page) && table.New(4 * Page, 3 * Page));
    }
    {
        // Two mappings that happen to be one piece of host memory: one piece (the old rule copied).
        Table table;
        table.Map(4, 4, Host + 100 * Page);
        table.Map(8, 4, Host + 104 * Page);
        assert(table.New(6 * Page, 4 * Page) && !table.Old(6 * Page, 4 * Page));
        // Two that are not: not, whatever their numbers.
        Table apart;
        apart.Map(4, 4, Host + 100 * Page);
        apart.Map(8, 4, Host + 300 * Page);
        assert(!apart.New(6 * Page, 4 * Page));
    }
    {
        // The count goes round: after 65,536 mappings a new one carries an old one's number. A
        // range over the two was read where it lies by the old rule, from memory it does not cover.
        Table table;
        table.Map(4, 4, Host + 100 * Page);          // number 0
        table.count = 0;                              // 65,536 mappings later
        table.Map(8, 4, Host + 900 * Page);          // number 0 again, elsewhere in host memory
        assert(table.entries[4].Block() == table.entries[8].Block());
        assert(table.Old(6 * Page, 4 * Page) && !table.New(6 * Page, 4 * Page));
    }
    {
        // The middle of a mapping mapped again: both ends keep their number.
        Table table;
        table.Map(4, 8, Host + 100 * Page);
        table.Unmap(6, 2);
        table.Map(6, 2, Host + 500 * Page);
        assert(table.Old(4 * Page, 7 * Page) && !table.New(4 * Page, 7 * Page));
        assert(table.New(4 * Page, 2 * Page) && table.New(6 * Page, 2 * Page) && table.New(8 * Page, 4 * Page));
    }
    {
        // A range that ends where a page ends is not judged by the page after it.
        Table table;
        table.Map(4, 4, Host + 100 * Page);
        table.Map(8, 4, Host + 700 * Page);
        assert(table.New(4 * Page, 4 * Page) && !table.Old(4 * Page, 4 * Page));
    }
    {
        // A page the GPU caches (marked) is still read where it lies, as in Eden.
        Table table;
        table.Map(4, 4, Host + 100 * Page);
        table.entries[5].marked = true;
        assert(table.New(4 * Page, 4 * Page));
    }
    return 0;
}
"""
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'span'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'headless'), '-x', 'c++', '-', '-o', str(exe)], input=code, text=True, check=True)
    subprocess.run([str(exe)], check=True)

# The build replaces both GetSpan functions with ones that ask guest_span.h, and only them.
cmake = (root / 'headless/CMakeLists.txt').read_text()
old = cmake.split('set(span_old [=[', 1)[1].split(']=])', 1)[0]
new = cmake.split('set(span_new [=[', 1)[1].split(']=])', 1)[0]
assert old.count('GetSpan(') == 2 and '.Block()' in old
assert new.count('GetSpan(') == 2 and '.Block()' not in new and new.count('OnePiece(addr, size)') == 2
assert '::Eden::GuestSpan::OnePiece(current_page_table->entries, masked >> YUZU_PAGEBITS,' in new
assert 'string(REPLACE "${span_old}" "${span_new}" arena_source "${arena_source}")' in cmake
assert 'string(PREPEND arena_source "#include \\"${EDEN_PORT_DIR}/guest_span.h\\"\\n")' in cmake
if len(sys.argv) > 1:
    pinned = Path(sys.argv[1]).read_text()
    assert pinned.count(old) == 1, 'the pinned GetSpan is not the one the build replaces'
    assert pinned.count('GetSpan(const VAddr addr') == 2
print('Guest span: PASS a range is read where it lies only when every page of it is one piece of host '
      'memory: not across mappings that share a number after the count goes round, a middle mapped '
      'again, an unmapped page or the end of a mapping')
