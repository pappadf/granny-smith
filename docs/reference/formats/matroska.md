# Matroska (the subset for a screen recording)

Matroska is a media container built on EBML, a binary encoding of a tree of typed, sized elements [1][2]. Its
specification is RFC 9559 [1]; EBML's is RFC 8794 [2]. This page covers what a writer and a reader of a simple
recording need: one video and at most one audio track, each frame one block, blocks in time order, a constant frame
rate — the EBML encoding, the top-level layout, the elements that file uses and their values, and how such a file
is written so that it stays readable if writing stops. Chapters, tags, attachments, lacing, BlockGroups,
encryption and the other track types are left to [1].

## 1. Identification (magic, sizes)

### 1.1 What Matroska is

A Matroska file is an **EBML header** element followed by one **Segment** element holding everything else [1].
WebM is the same structure with the DocType `webm` and a restricted set of codecs. A `.mkv` file is Matroska
holding any tracks; the extension is a convention only.

All EBML integers are **big-endian**; floats are IEEE 754 binary32 or binary64, big-endian [2].

### 1.2 Magic

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `1A 45 DF A3` — the EBML element's ID |

Inside that header, the DocType element (`42 82`) holds the ASCII string `matroska` (or `webm`). A file is
Matroska when it begins with `1A 45 DF A3` and its EBML header's DocType is one of those [2].

### 1.3 Sizes

- An element ID is 1 to 4 bytes; an element data size 1 to 8 bytes, so an element's body is at most 2^56 − 2 bytes
  [2]. The EBML header states the largest of each a file uses (EBMLMaxIDLength 4, EBMLMaxSizeLength 8).
- A block's timestamp is a signed 16-bit offset from its cluster's, in TimestampScale units (§2.5), which bounds
  how much time one cluster can cover: about 32.7 s at 1 ms units.

## 2. Layout tables

### 2.1 EBML encoding

Every element is **ID**, **data size**, **data** [2]. Both the ID and the size are variable-length integers
(VINTs): the number of leading zero bits of the first byte, plus one, is the length in bytes, and that first one bit
is the length marker.

| First byte | Length | Value bits |
|---|---|---|
| `1xxxxxxx` | 1 | 7 |
| `01xxxxxx` | 2 | 14 |
| `001xxxxx` | 3 | 21 |
| `0001xxxx` | 4 | 28 |
| … | … | … |
| `00000001` | 8 | 56 |

- An **ID** is read with its marker bit kept: `1A 45 DF A3`, `A3`, `42 82` are IDs as written.
- A **data size** is read with the marker removed. All value bits one means **unknown size** (`FF`, `7F FF`, …,
  `01 FF FF FF FF FF FF FF`): the element runs to the end of its parent, or until an element that cannot be its child
  begins [2]. A size may be written longer than it needs (any length up to EBMLMaxSizeLength), which is how a writer
  reserves room for a size it patches later.

| Type | Body |
|---|---|
| master | child elements, one after another |
| unsigned / signed integer | 0 to 8 bytes, big-endian (empty is 0) |
| float | 0, 4 or 8 bytes |
| string, UTF-8 | the bytes, no terminator required |
| binary | the bytes |

A **Void** element (ID `EC`) is padding anywhere: its body is ignored, and a writer can overwrite one in place with
an element of the same total length [1].

### 2.2 Top level

| Element | ID | Contents |
|---|---|---|
| EBML | `1A 45 DF A3` | EBMLVersion `42 86` = 1, EBMLReadVersion `42 F7` = 1, EBMLMaxIDLength `42 F2` = 4, EBMLMaxSizeLength `42 F3` = 8, DocType `42 82` = `matroska`, DocTypeVersion `42 87`, DocTypeReadVersion `42 85` |
| Segment | `18 53 80 67` | everything below |
| SeekHead | `11 4D 9B 74` | Seek `4D BB` entries: SeekID `53 AB` (an element's ID as bytes), SeekPosition `53 AC` (its offset from the Segment's data) |
| Info | `15 49 A9 66` | TimestampScale `2A D7 B1` (ns per tick; 1,000,000 = 1 ms), MuxingApp `4D 80`, WritingApp `57 41`, Duration `44 89` (a float, in ticks) |
| Tracks | `16 54 AE 6B` | one TrackEntry `AE` per track (§2.3) |
| Cluster | `1F 43 B6 75` | Timestamp `E7` (in ticks), then SimpleBlock `A3` elements (§2.5) |
| Cues | `1C 53 BB 6B` | CuePoint `BB` entries (§2.6) |

Offsets in SeekHead and Cues count from the first byte of the Segment's **data**, not from the start of the file.
A DocTypeReadVersion of 2 or more says that SimpleBlocks may be present [1].

### 2.3 TrackEntry

| Element | ID | Value |
|---|---|---|
| TrackNumber | `D7` | 1, 2, … — what blocks name |
| TrackUID | `73 C5` | non-zero, unique in the file |
| TrackType | `83` | 1 video, 2 audio |
| FlagLacing | `9C` | 0: no block holds more than one frame |
| CodecID | `86` | a string naming the codec mapping (§2.4) |
| CodecPrivate | `63 A2` | the mapping's setup data |
| DefaultDuration | `23 E3 83` | ns per frame: a constant frame rate is stated here |
| Video | `E0` | PixelWidth `B0`, PixelHeight `BA` (the stored frame), DisplayWidth `54 B0`, DisplayHeight `54 BA` (the shape to show it at) |
| Audio | `E1` | SamplingFrequency `B5` (float, Hz), Channels `9F`, BitDepth `62 64` |

Players read the frame rate from DefaultDuration; block timestamps, rounded to the tick, only place each frame.

### 2.4 Codec mappings

| CodecID | CodecPrivate | Each block |
|---|---|---|
| `V_MS/VFW/FOURCC` | a 40-byte `BITMAPINFOHEADER` [4], little-endian: biSize 40, biWidth, biHeight, biPlanes 1, biBitCount, biCompression the FourCC (`ZMBV` for [zmbv.md](zmbv.md)), biSizeImage, then zeros | one frame of that codec |
| `A_FLAC` | `fLaC` and the FLAC metadata blocks, STREAMINFO first ([flac.md](flac.md) §2.1–2.2) | one FLAC frame |

[3] lists the mappings. `V_MS/VFW/FOURCC` is the mapping for any codec defined for Video for Windows: a reader
takes the codec from the FourCC.

### 2.5 SimpleBlock

| Field | Size | Meaning |
|---|---|---|
| track | VINT, marker removed | the TrackNumber |
| timecode | 2 | signed, ticks from the cluster's Timestamp |
| flags | 1 | bit 7 keyframe; bit 3 invisible; bits 2–1 lacing (00 none); bit 0 discardable |
| frame | rest | the codec's frame |

A block's time is `(cluster Timestamp + timecode) × TimestampScale` ns.

### 2.6 CuePoint

| Element | ID | Value |
|---|---|---|
| CueTime | `B3` | ticks |
| CueTrackPositions | `B7` | CueTrack `F7` (a TrackNumber), CueClusterPosition `F1` (the cluster's offset from the Segment's data) |

## 3. Algorithms (checksums, compression)

### 3.1 Reading a VINT

```
len = 1; while first_byte & (0x80 >> (len - 1)) == 0: len += 1     // first byte 0: invalid
v = first_byte & (0xFF >> len)                                      // keep the marker for an ID
for i in 1..len-1: v = (v << 8) | byte[i]
unknown = (v == 2^(7*len) - 1)                                      // sizes only
```

### 3.2 Writing a file that stays readable

A recording's end is not known while it is written, and the writing may stop at any point. A file written in this
order is readable at every moment, up to its last complete cluster:

1. The EBML header; the Segment's ID and an 8-byte size of all ones (unknown).
2. A Void large enough for the SeekHead (the positions of Info, Tracks and Cues are not all known yet).
3. Info with a Void where the Duration will go; Tracks.
4. Clusters, each assembled in memory and written whole, its size known. A new cluster starts at each video
   keyframe (so a cue can point at it), when a block's timecode would not fit in 16 bits, and at a bound on size or
   time.
5. At the end: Cues; then the Segment's size, the SeekHead and the Duration patched over their placeholders.

A reader of a file cut during step 4 sees an unknown-size Segment and complete clusters, possibly followed by the
start of one: it stops at the last element that fits in the file. Nothing in step 5 is needed to play the file
front to back; the SeekHead and Cues only make seeking cheap.

### 3.3 Checksums

None in this subset. (EBML defines an optional CRC-32 element, `BF`, as the first child of a master [2]; it is not
used here.)

## 4. Worked decode of a real specimen

A 25.5 s recording of a Macintosh Plus booting [5]: 79,332 bytes, ZMBV 512 x 342 video and FLAC 22,255 Hz mono
audio. FFmpeg 6.1.1's `ffprobe` reports it as `matroska,webm`, 25.519533 s, video at 1203/20 fps (*observed*).

| Offset | Element | Value |
|---|---|---|
| `0x0000` | EBML, size `01 00 00 00 00 00 00 23` (35) | versions 1/1, max ID 4, max size 8, `matroska`, DocTypeVersion 4, ReadVersion 2 |
| `0x002F` | Segment, size 79,273 | (patched from unknown at the end); its data starts at `0x003B` |
| `0x003B` | SeekHead, 44 bytes | Info at `0x40`, Tracks at `0x7C`, Cues at `0x13581` |
| `0x006C` | Void, 13 bytes | the rest of the 64 bytes reserved for the SeekHead |
| `0x007B` | Info | TimestampScale `0F 42 40` (1 ms); MuxingApp, WritingApp; Duration `40 D8 EB E2 1E 64 6F 15` = 25,519.53 ms |
| `0x00B7` | Tracks | |
| `0x00C3` | TrackEntry 1 | video, no lacing, `V_MS/VFW/FOURCC`; CodecPrivate `28 00 00 00 00 02 00 00 56 01 00 00 01 00 08 00 5A 4D 42 56 00 AC 02 00 …` (40, 512, 342, 1 plane, 8 bits, `ZMBV`, 175,104); DefaultDuration `FD AD CF` = 16,625,103 ns; 512 x 342, displayed 512 x 342 |
| `0x0137` | TrackEntry 2 | audio, `A_FLAC`; CodecPrivate `fLaC` + STREAMINFO ([flac.md](flac.md) §4); SamplingFrequency 22,255.0, Channels 1, BitDepth 16 |
| `0x019C` | Cluster, 25,651 bytes | Timestamp 0; SimpleBlocks: track 1, `00 00`, `80` (keyframe, 204-byte ZMBV keyframe); track 2, `00 00`, `80` (a FLAC frame); track 1, `00 11` (17 ms), `00`; track 1, `00 21` (33 ms) … |
| `0x65DB` | Cluster | Timestamp `03 F6` = 1,014 ms: the second cluster opens after a second |
| `0x135BC` | Cues | one CuePoint: CueTime 0, track 1, CueClusterPosition `01 61` = `0x19C − 0x3B` |

Every master's size but the SeekHead's and its Seeks' is written 8 bytes long, the length a size patched in place
needs; the file has 26 clusters and one keyframe, so one cue.

## 5. Open questions

- Which players besides those built on FFmpeg play ZMBV in Matroska has not been checked; the `V_MS/VFW/FOURCC`
  mapping is generic and a player needs a ZMBV decoder of its own.
- Whether a player uses the SeekHead and Cues of a file whose only keyframe is its first is a property of the
  player, not of the format; the format requires neither.

## Appendix A. Granny Smith implementation notes

The screen recorder writes exactly the subset of §2 in the order of §3.2: a 64-byte SeekHead reservation, an 11-byte
Duration reservation, TimestampScale 1 ms, video as track 1 (`V_MS/VFW/FOURCC`, ZMBV) and FLAC audio as track 2,
DefaultDuration the frame period, every block a SimpleBlock with no lacing, a cue per cluster that opens on a video
keyframe, and the file flushed after every cluster. Clusters close at a video keyframe, after a second, past 1 MB, or
when a timecode would not fit. Its reader accepts that subset and BlockGroups, not lacing or unknown-size clusters.
See `docs/internals/core/record/record.md` §3.7.

## References

1. S. Lhomme, M. Bunkus, D. Rice, *Matroska Media Container Format Specification*, RFC 9559, IETF, October 2024.
2. S. Lhomme, D. Rice, M. Bunkus, *Extensible Binary Meta Language*, RFC 8794, IETF, July 2020.
3. *Matroska Media Container Codec Specifications*, IETF CELLAR working group — `V_MS/VFW/FOURCC`, `A_FLAC`.
4. Microsoft, *BITMAPINFOHEADER structure*, Windows GDI documentation.
5. A Macintosh Plus boot to the floppy prompt, recorded from power-on (specimen) — decoded as in §4.
