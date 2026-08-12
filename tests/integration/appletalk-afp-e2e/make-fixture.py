#!/usr/bin/env python3
"""Build the AFP end-to-end suite's test application.

Stage 4 of proposal-afp-server-completeness.md §7.3 needs a resource-bearing
binary the guest can *launch over AFP*: the Segment Loader has to open the
file's resource fork through FPOpenFork, FPRead its CODE resources, and jump
into them.  Nothing on the stock System 6 boot volume is both a standalone
application and visually distinctive, so the suite ships its own — a minimal
68K app that paints a known pattern over the screen and then spins.

The app is emitted as an AppleDouble pair, which is exactly the on-disk shape
the AFP server serves and `cp` produces:

    <share>/TestApp        empty data fork (applications have none)
    <share>/._TestApp      AppleDouble header: Finder Info + resource fork

Generating it here rather than committing a binary keeps the 68K and the
resource-map layout reviewable.  Usage: make-fixture.py <share-directory>
"""

import struct
import sys
import os

# --- the application's code -------------------------------------------------
#
# The Segment Loader hands control to CODE 1 with the A5 world set up.  The app
# fills the Macintosh Plus frame buffer (512x342 mono = 21,888 bytes, its base
# address in the ScrnBase low-memory global at $0824) with a vertical-stripe
# pattern, then loops forever so the frame stays put for the screenshot.
CODE_1_BODY = bytes.fromhex(
    "2070 0824"          # movea.l  ($0824).w,a0     ; ScrnBase
    "303C 155F"          # move.w   #5471,d0         ; 21888/4 - 1 longs
    "20FC F0F0F0F0"      # move.l   #$F0F0F0F0,(a0)+ ; vertical stripes
    "51C8 FFF8"          # dbra     d0,-8            ; back to the move.l
    "60FE"               # bra.s    *                ; park here
    .replace(" ", "")
)

# CODE 1 is prefixed by the segment header the Segment Loader reads: the offset
# of this segment's first jump-table entry (from the start of the table) and
# how many entries it owns.
CODE_1 = struct.pack(">HH", 0, 1) + CODE_1_BODY

# CODE 0 describes the A5 world and carries the jump table itself:
#   above-A5 size, below-A5 size, jump-table size, jump-table offset from A5.
# Above A5 the 32-byte application-parameters block comes first, then the
# table; below A5 is the application's globals.
JT_ENTRY = struct.pack(">HHHH", 0x0000, 0x3F3C, 0x0001, 0xA9F0)  # unloaded: _LoadSeg 1
CODE_0 = struct.pack(">IIII", 32 + len(JT_ENTRY), 0x0100, len(JT_ENTRY), 32) + JT_ENTRY

RESOURCES = [(b"CODE", 0, CODE_0), (b"CODE", 1, CODE_1)]


def build_resource_fork(resources):
    """Assemble a classic resource fork (Inside Macintosh: More Macintosh Toolbox)."""
    data = b""
    offsets = []
    for _, _, payload in resources:
        offsets.append(len(data))
        data += struct.pack(">I", len(payload)) + payload

    # Group by type, preserving declaration order.
    types = []
    for rtype, rid, _ in resources:
        if not types or types[-1][0] != rtype:
            types.append((rtype, []))
        types[-1][1].append(rid)

    # The map is: 16-byte header copy, 4-byte next-map handle, 2-byte file
    # refnum, 2-byte attributes, then the offsets of the type and name lists.
    map_header_len = 16 + 4 + 2 + 2 + 2 + 2
    type_list_len = 2 + 8 * len(types)
    ref_list_len = 12 * len(resources)

    type_list_off = map_header_len
    name_list_off = type_list_off + type_list_len + ref_list_len

    type_list = struct.pack(">H", len(types) - 1)
    ref_lists = b""
    ref_cursor = type_list_len  # ref lists are addressed from the type list start
    index = 0
    for rtype, ids in types:
        type_list += rtype + struct.pack(">HH", len(ids) - 1, ref_cursor)
        for rid in ids:
            off = offsets[index]
            ref_lists += struct.pack(">hhB", rid, -1, 0)  # no name, no attributes
            ref_lists += struct.pack(">I", off)[1:]  # 3-byte data offset
            ref_lists += struct.pack(">I", 0)  # handle placeholder
            index += 1
        ref_cursor += 12 * len(ids)

    data_off = 256
    map_body = struct.pack(">HH", 0, 0)  # file refnum, attributes
    map_body += struct.pack(">HH", type_list_off, name_list_off)
    map_body += type_list + ref_lists

    map_len = map_header_len + type_list_len + ref_list_len
    header = struct.pack(">IIII", data_off, data_off + len(data), len(data), map_len)
    resource_map = header + struct.pack(">I", 0) + map_body
    return header + b"\x00" * (data_off - 16) + data + resource_map


def build_appledouble(finder_info, rsrc):
    """Wrap Finder Info (entry 9) and the resource fork (entry 2) in a sidecar."""
    entries = [(9, finder_info), (2, rsrc)]
    header_len = 26 + 12 * len(entries)
    out = struct.pack(">II", 0x00051607, 0x00020000) + b"\x00" * 16
    out += struct.pack(">H", len(entries))
    offset = header_len
    for entry_id, payload in entries:
        out += struct.pack(">III", entry_id, offset, len(payload))
        offset += len(payload)
    for _, payload in entries:
        out += payload
    return out


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make-fixture.py <share-directory>")
    share = sys.argv[1]

    # FInfo: type 'APPL', creator 'GSte', no flags, Finder-chosen position.
    finder_info = b"APPLGSte" + struct.pack(">HhhH", 0, 0, 0, 0) + b"\x00" * 16

    rsrc = build_resource_fork(RESOURCES)
    with open(os.path.join(share, "TestApp"), "wb") as f:
        pass  # applications carry no data fork
    with open(os.path.join(share, "._TestApp"), "wb") as f:
        f.write(build_appledouble(finder_info, rsrc))
    print("TestApp: %d bytes of resource fork" % len(rsrc))


if __name__ == "__main__":
    main()
