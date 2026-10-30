// SPDX-License-Identifier: GPL-3.0-or-later
// Whether a range of a guest's memory is one piece of host memory (Eden's Memory::GetSpan,
// core/memory.cpp, as headless/CMakeLists.txt patches it).
//
// Eden reads a service's input buffers, a voice's samples and a video decoder's commands where they
// lie when the range is one piece of host memory, and copies it page by page when not. It decided
// by a number each mapping of guest memory is given: the same number at the range's first page and
// at the page after it meant one piece. The number is 16 bits and counts up with every mapping, so
// it goes round after 65,536 of them (43 minutes into a Tears of the Kingdom session on a tester's
// console: "assert current_block != 65535" in its eden_log.txt), and from then on two mappings can
// carry the same one; a mapping whose middle was mapped again keeps its number at both ends; and
// the page after a range is not in it. In each case a service could be handed bytes from memory the
// range does not cover, read as if they were the game's.
//
// A page table entry holds the host address of its page less the page's guest address. That value
// is the same for every page of one piece of host memory and differs between pieces, so a range is
// one piece when every page it covers holds the same value, and none is unmapped (0).
#pragma once
#include <cstdint>

namespace Eden::GuestSpan {
// `entries[page].Pointer(true)`: a page's value, the marked bit ignored (a page the GPU caches is
// still read where it lies, as Eden does).
template <typename Entries>
bool OnePiece(const Entries& entries, std::uint64_t first_page, std::uint64_t last_page) {
    const auto pointer = entries[first_page].Pointer(true);
    if (pointer == 0) return false;
    for (std::uint64_t page = first_page + 1; page <= last_page; ++page)
        if (entries[page].Pointer(true) != pointer) return false;
    return true;
}
} // namespace Eden::GuestSpan
