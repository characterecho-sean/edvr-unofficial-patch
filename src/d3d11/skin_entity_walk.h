#pragma once

// F2: the hook's reading of the game's entry list, as a pure function over a reader (skin_entity_hook.cpp supplies a reader
// that wraps every read in structured exception handling; the rig supplies a fake heap with faults in it).
//
// What it reads, from the decompile of FUN_144c540e0 (build 332841; the offsets are EVIDENCE from the decompile, checked at run
// time by checkSnapshot and by the GPU against the dispatch's own job table, not assumed):
//   node + 0xA8   the first entry of the list the game walks (a singly linked list, in registration order)
//   node + 0xC4   the running base after the function has assigned every entry its rows
//   entry + 0x00  the vtable
//   entry + 0x08  the next entry
//   entry + 0x38  the mesh data; its first ushort is the bone count of the entry's primary job
//   entry + 0xA8  the base the function just assigned (the first row of the entry's primary job)
// Nothing is written. A read that fails stops the walk with kSnapFault; a pointer that cannot be a user-mode object stops it with
// kSnapImplausible; more entries than a snapshot holds stops it with kSnapOverflow (a list that points back into itself ends there too).

#include <cstdint>

#include "skin_join.h"

namespace edvr {
namespace skinjoin {

constexpr uint32_t kOffNodeList = 0xA8, kOffNodeEnd = 0xC4;
constexpr uint32_t kOffEntryNext = 0x08, kOffEntryMesh = 0x38, kOffEntryDst = 0xA8;

inline bool userPointer(uint64_t p) { return p >= 0x10000u && p < 0x00007FFFFFFE0000ull && (p & 7u) == 0; }

// `read(address, out, bytes)` -> true when the bytes were read.
template <class Reader>
inline void walkNode(Reader&& read, uint64_t node, Snapshot& s) {
    s.n = 0;
    s.end = 0;
    s.flags = 0;
    s.node = node;
    uint64_t entry = 0;
    uint32_t end = 0;
    if (!userPointer(node) || !read(node + kOffNodeList, &entry, 8)) {
        s.flags |= kSnapFault;
        return;
    }
    while (entry) {
        if (s.n >= kMaxEntries) { s.flags |= kSnapOverflow; break; }
        if (!userPointer(entry)) { s.flags |= kSnapImplausible; break; }
        uint64_t vtable = 0, next = 0, mesh = 0;
        uint32_t dst = 0;
        uint16_t count = 0;
        if (!read(entry, &vtable, 8) || !read(entry + kOffEntryNext, &next, 8) || !read(entry + kOffEntryMesh, &mesh, 8) ||
            !read(entry + kOffEntryDst, &dst, 4)) { s.flags |= kSnapFault; break; }
        if (!userPointer(mesh)) { s.flags |= kSnapImplausible; break; }
        if (!read(mesh, &count, 2)) { s.flags |= kSnapFault; break; }
        Entry& e = s.e[s.n++];
        e.key = entry;
        e.vtable = vtable;
        e.mesh = mesh;
        e.dst = dst;
        e.count = count;
        entry = next;
    }
    if (!read(node + kOffNodeEnd, &end, 4)) s.flags |= kSnapFault;
    s.end = end;
}

}  // namespace skinjoin
}  // namespace edvr
