# Screen recorder

> 🚧 **Placeholder** — this page is planned but not yet written.
<!-- gs-doc-status: stub -->

The recorder, [`src/core/record/`](../../../../src/core/record/), writes the
emulated screen and sound to a lossless Matroska file on the guest's clock.
So far it holds the container: the Matroska muxer behind a small muxer
interface ([`mkv_mux.c`](../../../../src/core/record/mkv_mux.c),
[`mkv_mux.h`](../../../../src/core/record/mkv_mux.h)) and a reader for
the subset it writes ([`mkv_read.c`](../../../../src/core/record/mkv_read.c),
[`mkv_read.h`](../../../../src/core/record/mkv_read.h)). The codecs it
carries, ZMBV and FLAC, are in the codec library
([../../codec/codec.md](../../codec/codec.md) §1.4).

## 1. Responsibilities & design

> **TODO** — the capture on the emulator thread, the recorder thread and
> the `recorder` object are not written yet.

The muxer takes encoded packets with a presentation time in nanoseconds
and writes them; which container is a table of operations (`muxer_ops_t`:
`packet`, `close`, `bytes`), so another container is an addition beside
`mkv_mux.c`. The Matroska file is written front to back and is a valid
stream at every cluster boundary, so a recording whose writer never closed
it plays to its last complete cluster.

## 2. Key types & files

| | |
|---|---|
| `muxer_t`, `muxer_ops_t` | a container writer: queue a packet of a track, close (finalise and free), bytes so far |
| `mux_video_t`, `mux_audio_t` | the tracks: ZMBV frame size, display size, frame period and bit count; FLAC rate, channels, depth and STREAMINFO |
| `mkv_mux_open` | the Matroska muxer |
| `mkv_reader_t`, `mkv_track_t`, `mkv_packet_t` | the reader: tracks, then packets (track, timestamp, keyframe, bytes) in file order |

## 3. Behaviour/algorithms

- **Layout.** The EBML header (DocType `matroska`); a Segment whose size
  is written as unknown and patched at close; a 64-byte Void that close
  overwrites with a SeekHead (Info, Tracks, Cues); Info (TimestampScale
  1 ms, the muxing and writing app, an 11-byte Void that close turns into
  the Duration); Tracks — video `V_MS/VFW/FOURCC` with a
  `BITMAPINFOHEADER` CodecPrivate (FourCC `ZMBV`), `DefaultDuration` the
  frame period, pixel and display size; audio `A_FLAC` with CodecPrivate
  `fLaC` plus the STREAMINFO block, sampling frequency, channels, bit depth
  16 — then Clusters of SimpleBlocks, and the Cues at close.
- **Time.** A block's timestamp is its pts rounded to the millisecond, so
  frame *k* of a track with period *P* ns is at round(*kP* / 10⁶) ms; the
  track's constant rate is in `DefaultDuration`, which is how players read
  it (1203/20 fps for the 16,625,103 ns VBL period).
- **Interleaving.** Packets wait per track until every track has one; the
  earliest goes out first, video before audio at the same millisecond. A
  queue that grows past 16 MB or 1024 packets is written out regardless.
- **Clusters.** One is built in memory (a 1 MB buffer, grown for a larger
  frame) and written whole, its size known, when the next block is a video
  keyframe, is a second or more after the cluster's start, or follows 1 MB
  of cluster, or would need a relative timestamp the block's 16 bits
  cannot hold. Each cluster that opens on a video keyframe gets a cue. The
  file is flushed after every cluster.
- **The reader** walks front to back and needs neither SeekHead nor Cues;
  a cluster or other element that runs past the end of the file ends the
  walk, and the file counts as cut. It handles the writer's subset, plus
  BlockGroups; not lacing or unknown-size clusters.

## 4. Object-model / shell surface

> **TODO**

## 5. Checkpointing

None: a recording is process state and is not checkpointed.

## 6. Testing

- `tests/unit/suites/record_mkv`: known packets on a video and an audio
  track round-trip exactly, in time order, at round(*kP* / 10⁶) ms, in
  clusters that open on keyframes and span under a second (one carrying a
  1.5 MB frame), with a cue per keyframe cluster, the segment size and the
  duration; a video-only file; a file cut halfway into a cluster returns
  exactly the earlier clusters' packets; a file whose muxer was never
  closed reads back up to its last complete cluster.

## 7. Known debts

> **TODO**

## 8. See also

- [../../codec/codec.md](../../codec/codec.md) — the codec library, with
  ZMBV and FLAC.
