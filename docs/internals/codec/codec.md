# Codec library

The codec library, [`src/codec/`](../../../src/codec/), is the one place the
tree compresses, decompresses and checksums data: a C99 library with one
public header, [`codec.h`](../../../src/codec/include/codec.h), and no
dependency beyond libc. Every algorithm the tree meets — disk-image chunks,
archive methods, compressed resources, the checkpoint file — has its single
implementation here; the formats that carry them (peeler's archives, the
image layer, the resource-fork parser, the checkpoint file) only parse their
containers.

## 1. Responsibilities & design

| Caller | Uses |
|---|---|
| peeler: StuffIt, Compact Pro, BinHex, zip and gzip (`src/peeler/lib/formats/`, each fork a decoder over its range of the archive, `lib/fork_decoder.c`) | the StuffIt methods, Compact Pro's LZH and RLE, RLE90, DEFLATE, gzip, CRC-16 and CRC-32 |
| UDIF chunks (`src/core/storage/image_udif.c`) | ADC, zlib, bzip2, LZFSE, xz, by chunk type |
| NDIF chunks (`image_ndif.c`) | ADC |
| QCOW2 clusters (`image_qcow2.c`) | raw DEFLATE |
| compressed resources (`resource_fork.c`, `tools/dump`) | dcmp 0, 2 and 3 |
| the checkpoint file (`src/core/checkpoint.c`) | GSRLE, both ways |
| the UDIF writer (`udif_writer.c`) | the zlib encoder (or, opted into, the xz encoder), CRC-32 (and its zero-run form) |
| the PNG reader and writer (`src/core/debug/debug.c`), the ImageWriter's page store (`iw_page.c`) and the PDF writer (`pdf_writer.c`) | zlib, both ways |
| MOOF, AFP's append logs, BMAC, the Rage 128 CCE, the PCI option-ROM identity | CRC-32 |

The library is a sibling of peeler rather than part of it: peeler is an
archive reader, while the codecs are services its archives, the image layer
and the writers share. peeler depends on the library, never the other way
round, and nothing in the library includes anything from `src/core` or
`src/peeler`.

Every decoder and encoder is first-party, written from the formats'
published descriptions (and, for dcmp 0 and 2, Apple's released assembly);
no third-party implementation's code was translated into it. Independent
encoders (zlib, libbz2 and liblzma through Python, Apple's LZFSE encoder
through pyliblzfse) are used only to generate test vectors (§6).

The decoders take untrusted input — disk images and archives from anywhere
— so every length, distance, table index and state is bounds-checked
against the buffers it addresses, and a malformed stream is an error.

### 1.1 One decoder interface

Every decoder is a `codec_dec_t`, created for one stream by
`codec_dec_new(id, &opts, &err)` and run with `codec_dec_run(d, out, cap,
&n)` as many times as the caller likes: each run produces up to `cap` bytes
and stops wherever the buffer fills — mid-match, mid-block, mid-run — and
the next continues from exactly there. A reader therefore decodes as far as
it reads and no further, which is how peeler's decode-through sources serve
an archive member a read at a time. A run returns 1 once the stream has
ended and every byte of it has been produced, 0 while more remains, or a
negative `CODEC_ERR_*` code (`DATA`, `TRUNCATED`, `OVERFLOW`, `NOMEM`,
`UNSUPPORTED`, `CHECKSUM`, `ARG`, or a pull callback's own) after the bytes
before the fault; `codec_dec_message()` says why in a few words.

Input is `len` bytes in memory or pulled through a `codec_pull_fn` as the
decoder needs it; `opts.pull_close`, when set, is called on the pull
context as the decoder is freed, so a decoder can own what it reads (never
when `codec_dec_new` fails: the context is then still the caller's).
`codec_dec_pull` is such a callback over another decoder, and
`codec_dec_stack(inner, id, &opts, &err)` builds the chain with the outer
decoder owning the inner one: a Compact Pro fork is `CODEC_CPT_RLE` stacked
on `CODEC_CPT_LZH`. When an inner decoder fails, the outer one reports the
inner one's message. `codec_dec_consumed()` is exact once a stream has
ended.

`opts.out_len` is the decoded size where the container states it. A stream
with no end of its own — ADC, the StuffIt methods 2, 3, 13 and 15, Compact
Pro's RLE, GSRLE — ends there and requires it (its final match or run is cut
at the size, as the original decoders did; input that runs out first is
`TRUNCATED`); for every other codec it is a limit, and decoding past it is
`OVERFLOW`. `opts.exact` applies the first rule to any codec — the
container's size wins — which is how a StuffIt fork decodes whatever its
method (RLE90 and stored included). `opts.check` names a checksum the
container states over the decoded bytes (`CODEC_CHECK_CRC32`, zip's;
`CODEC_CHECK_CRC16_ARC`, StuffIt's), verified once every byte is out:
`CHECKSUM` if it differs.

The decoder is the one streaming type in the tree. A container's own
checks and chains are its options rather than a wrapper type: peeler hands
the decode-through sources a `codec_dec_t` for every streaming fork (its
`peel_fmt_t.decoder`), and drains the same decoder for the buffer API. The one-shot forms are built on the same decoders:
`codec_decode(id, src, len, dst, cap)` (the stream must end within `cap`
bytes, so a buffer one byte short is `OVERFLOW`, never a silent clamp) and
`codec_decode_alloc` for a stream of unknown size under a ceiling (a PNG, a
compressed resource).

`codec_decode` reaches every decoder by `codec_id_t`, which is how a
container maps its method field to a codec (`udif_chunk_codec()` for UDIF,
`sit_method_codec()` for StuffIt).

### 1.2 One encoder interface

Every encoder is a `codec_enc_t`: `codec_enc_new(id, max_len, level, &err)`
allocates its state once for inputs up to `max_len` at a level 1..9 (< 0 is
the codec's default), and `codec_enc_run` turns one whole input into one
complete stream, reusing that state — the UDIF writer compresses every
chunk of an image through one encoder. `codec_enc_bound(id, len)` is output
room that always suffices; a smaller buffer that turns out too small is
`OVERFLOW`, never a truncated stream. `codec_encode` is the one-shot form.
zlib, xz and GSRLE encode.

### 1.3 One layout

Inside the library each codec's source file follows the same layout: the
format in its header comment; constants; types; bit and table helpers; the
decoder (init, fill, free, and its `codec_dec_ops_t`); the encoder, if any.
The framework they are written on is `lib/codec_internal.h`:

- **The output ring** (`codec_ring_t`) is both a decoder's output buffer and
  its history — an LZ decoder's window. A decoder's `fill` decodes while the
  ring has room for its *burst* (the most one symbol can write: 258 for
  DEFLATE, a 32 KiB match for StuffIt 13, one byte for Arsenic), so it never
  has to suspend inside a symbol; `codec.c` drains the ring into the
  caller's buffers. Every decoder is resumable on the output side without
  code of its own for it. `ring_copy` is the LZ77 copy (bulk where source
  and destination do not overlap or wrap); `ring_copy_zero` serves the
  formats whose window starts zero-filled (StuffIt 13, Compact Pro).
- **The input** (`codec_in_t`) holds memory or a pull buffer and one 64-bit
  bit accumulator, read LSB-first (`lsb_*`: DEFLATE, StuffIt's LZW and
  method 13) or MSB-first (`msb_*`: bzip2, StuffIt's methods 3 and 15,
  Compact Pro's LZH), refilled eight bytes at a time. Bits past the end
  read as zeros and are counted, so a decoder checks `codec_in_overrun()`
  at symbol boundaries rather than after every bit.
- **Canonical Huffman trees** (`codec_htree_*`) in a bounded node pool, for
  the formats whose codes a lookup table would mishandle (StuffIt 13's
  absent symbols take code values, and its codes reach 31 bits) or are
  rare (Compact Pro). The fast formats build lookup tables of their own.
- Byte order (`codec_le32`, `codec_be16`, ...) and every checksum
  (`checksum.c`) come from one place, never a private copy.

## 2. Key types & files

| | |
|---|---|
| `codec_dec_t`, `codec_dec_opts_t` | a decoder for one stream, and the one streaming type (§1.1) |
| `codec_check_t` | a stated checksum over the decoded bytes, verified at the end |
| `codec_enc_t` | an encoder, its state reused across inputs (§1.2) |
| `codec_id_t` | `COPY`, `DEFLATE`, `ZLIB`, `GZIP`, `ADC`, `BZIP2`, `LZFSE`, `XZ`, `LZMA_ALONE`, `RLE90`, `SIT_LZW`, `SIT_HUFFMAN`, `SIT13`, `SIT_ARSENIC`, `CPT_RLE`, `CPT_LZH`, `DCMP`, `GSRLE` |
| `codec_crc32`, `codec_crc32_zeros`, `codec_crc32_msb`, `codec_crc64`, `codec_adler32`, `codec_crc16_xmodem`, `codec_crc16_arc` | CRC-32 (reflected; zero runs in O(log n)), CRC-32 MSB-first (bzip2), CRC-64/XZ, Adler-32, CRC-16/XMODEM (BinHex, MacBinary), CRC-16/ARC (StuffIt); constant tables, slice-by-8 for the CRC-32s, callable from any thread |

| File | |
|---|---|
| `lib/codec.c` | the registry, the input and ring helpers, the Huffman pool, `codec_dec_run` and the one-shot and encoder wrappers, `CODEC_COPY` |
| `lib/inflate.c`, `lib/deflate.c` | DEFLATE, zlib and gzip decoding; DEFLATE and zlib encoding |
| `lib/bzip2.c`, `lib/lzfse.c`, `lib/adc.c` | bzip2, LZFSE (with LZVN), ADC |
| `lib/lzma.c`, `lib/lzma_enc.c`, `lib/lzma_internal.h` | LZMA, LZMA2, xz and `.lzma`; the xz encoder; the model they share |
| `lib/rle90.c` | 0x90 run lengths, StuffIt's and BinHex's dialects (`CODEC_RLE90_BINHEX`) |
| `lib/sit_lzw.c`, `lib/sit_huffman.c`, `lib/sit13.c`, `lib/sit_arsenic.c` | StuffIt methods 2, 3, 13 and 15 |
| `lib/cpt.c` | Compact Pro's LZH and RLE |
| `lib/dcmp.c`, `lib/dcmp3_tables.h` | System 7 compressed resources |
| `lib/gsrle.c` | the checkpoint file's run lengths |
| `lib/checksum.c` | every checksum |

## 3. Behaviour/algorithms

- **DEFLATE.** Huffman codes decode through two-level tables: a 10-bit
  (literal/length) or 8-bit (distance) root whose entries carry the
  symbol's base value and extra-bit count, so a length or distance is one
  lookup and one bit read; longer codes go through subtables. The hot loop
  keeps the bit accumulator and the ring cursor in locals and copies
  matches in bulk. `CODEC_ZLIB` checks the RFC 1950 header and, when the
  stream carries one, the Adler-32 trailer (a stream that stops right after
  its final block is accepted, as some writers omit the trailer).
  `CODEC_GZIP` reads every member — header with its optional extra field,
  name, comment and header CRC, the DEFLATE stream, the CRC-32 and size
  checked — and ignores what follows the last one that is not another
  member, as `gzip -d` does. The encoder is greedy LZ77 over hash chains (depth by level), each block
  written as dynamic Huffman, fixed Huffman or stored, whichever is
  smallest; its dynamic codes are length-limited and complete.
- **bzip2.** Per block: the symbol map, the selectors (move-to-front coded),
  the delta-coded code lengths of up to six canonical Huffman tables, then
  the run-length and move-to-front coded symbols, each Huffman code found
  through a 10-bit lookup table per table (a canonical walk past it). The
  inverse Burrows-Wheeler transform runs as a linked walk over one 32-bit
  word per byte (symbol in the low byte, link above), and the initial
  run-length coding (four equal bytes then a count) is undone as the walk
  emits, a ring's room at a time. Each block's CRC and each stream's
  combined CRC are checked; concatenated streams decode as one, and bytes
  after the last that are not a stream are ignored (as `bzip2 -d` does).
  Randomised blocks (bzip2 before 0.9.5) are refused as unsupported.
- **LZFSE.** Stored (`bvx-`), LZVN (`bvxn`) and v2 (`bvx2`) blocks until
  `bvx$`; matches may reach into earlier blocks. A v2 block's header packs
  the counts, the initial states and the payloads' bit offsets into three
  64-bit words and codes the four frequency tables (literals, L, M, D) with
  a 2- to 14-bit prefix code. The finite-state (tANS) tables give each
  symbol its frequency's worth of consecutive states, each naming the bits
  to read for the next state. Literals decode first, by four interleaved
  states; then each (L, M, D) triple copies L literals and an M-byte match
  (D = 0 repeating the last distance), as many per fill as the ring holds.
  Both payloads are bit streams read backwards from their ends. The
  unpacked `bvx1` header is an encoder-internal form and is refused as
  unsupported.
- **LZMA / xz.** A binary range decoder with 11-bit adaptive probabilities
  drives the 12-state literal/match machine, the literal coders (selected by
  the previous byte and the position), the length coders and the distance
  coder (6-bit slot, then modelled, direct and aligned bits). The ring is
  the dictionary, sized to the stream's dictionary or the stated output,
  whichever is smaller. LZMA2 chunks reset the dictionary, the state or the
  properties as their control byte says, and each must decode exactly its
  stated sizes. The xz container is parsed fully — stream header, block
  headers (only an LZMA2 filter), their stated sizes, block padding,
  CRC-32 or CRC-64 checks (SHA-256 is skipped), the index (record count and
  CRC) and the stream footer — and concatenated streams with zero padding
  decode as one.
- **ADC.** Literal runs and short/long back-references, byte-aligned, in a
  64 KiB window; a reference before the start is an error.
- **RLE90.** A byte is itself; 0x90 escapes the next: 0 for a literal 0x90,
  N for the previous byte N − 1 more times. After an escaped 0x90 StuffIt
  repeats the byte before it and BinHex the 0x90 itself, and a count of 1
  is StuffIt's no-op but an error in BinHex.
- **StuffIt method 2 (LZW).** compress(1)-style, LSB-first, 9 to 14 bits; a
  clear code resets the dictionary and the rest of its group of eight codes
  is padding.
- **StuffIt method 3 (static Huffman).** A pre-order serialised tree, then
  the symbols walked a bit at a time; a single-leaf tree codes its symbol
  as no bits.
- **StuffIt method 13 (LZSS + Huffman).** Five predefined table sets or
  dynamic ones (code lengths through a fixed meta code; absent symbols
  still take code values), a 64 KiB window starting zero-filled, the two
  literal/length trees alternating after a match.
- **StuffIt method 15 (Arsenic).** An arithmetic decoder over adaptive
  frequency models decodes, per block, move-to-front indices with zero runs
  in bijective base 2; then the inverse Burrows-Wheeler transform, an
  optional de-randomisation and a final run-length stage. The block and
  stream framing and every length are bounded.
- **Compact Pro.** LZH: blocks of three canonical Huffman codes (lengths in
  nibbles) and literal/match tokens in an 8 KiB zero-filled window (offset
  0 is a whole window back); a block ends at a token cost of 0x1FFF0, and
  the input then skips to a byte boundary and 2 or 3 bytes more. RLE: 0x81
  escapes — 0x81 0x82 N repeats the previous byte N − 2 more times, 0x81
  0x81 is a literal 0x81 whose second byte escapes again.
- **dcmp.** The 0xA89F6572 header, then DonnBits (version 8, dcmp 0: byte
  opcodes, a var table of remembered strings, constant words), GreggyBits
  (version 9, dcmp 2: byte-to-word tables, static or saved, optionally
  bitmapped) or InstaCompOne (version 9, dcmp 3: LZ77 with static prefix
  codes, literals in runs, a distance code that widens as the output
  grows). Other decompressors are `UNSUPPORTED`, so callers keep the raw
  bytes. A resource is at most 16 MiB (`CODEC_DCMP_MAX_SIZE`) and its
  decoders address the whole output, so this decoder decodes the resource
  on its first fill and hands it out from there.
- **GSRLE.** Chunks of a marker, a 32-bit little-endian count and either
  that many literal bytes or one byte to repeat; the encoder makes a run of
  four or more equal bytes. A chunk past the stated size is an error.
- **xz encoding.** The decoder's model run backwards through a range
  encoder, over a heuristic parse: hash chains over 3-byte prefixes (as
  deep as the level asks), the four repeat distances tried at every
  position and preferred unless a new match is clearly longer, one byte of
  lazy matching, short repeats for a byte equal to the one at the last
  distance, and short matches far back dropped as not worth their distance
  bits. The output is one stream, one block, LZMA2 (lc 3, lp 0, pb 2):
  chunks close at 64 KiB coded or 2 MiB decoded, a chunk that does not
  shrink goes out stored with the model reset after it, and the CRC-64
  check, index and footer follow. xz-utils and Python's `lzma` read what it
  writes, and the UDIF interop workflow has hdiutil open its `.dmg`s.

### 3.1 Speed

Decoded MB/s on one x86-64 core (`-O2`), whole chunks as UDIF stores them:
System 7.5 64 KiB chunks / a Mac OS 9 image in 1 MiB chunks / 1 MiB of text,
with the library before this framework and Python's C libraries (zlib,
libbz2, liblzma) for reference:

| | before | now | reference |
|---|---|---|---|
| zlib | 152 / 152 / 636 | 257 / 318 / 2550 | 169 / 234 / 1705 |
| bzip2 | 41 / 30 / 34 | 68 / 45 / 37 | 33 / 26 / 25 |
| xz | 36 / 43 / 290 | 40 / 41 / 330 | 59 / 80 / 1514 |
| LZFSE | 233 / 263 / 2715 | 259 / 245 / 5491 | — |

## 4. Object-model / shell surface

None. The codecs are reached through their callers: `files.convert`,
`files.verify` and attaching a `.dmg` (UDIF), `files.archive.*` and the VFS
(peeler), checkpoints, and the resource views.

## 5. Checkpointing

None: the library holds no state between calls. (It encodes the checkpoint
file's blocks, but keeps nothing of its own in one.)

## 6. Testing

- `tests/unit/suites/codec` runs under ASan, UBSan and LeakSanitizer, and
  again on wasm32 (`make -C tests/unit run-wasm32`). Its vectors
  (`vectors.h`, from `make_vectors.py`) are encoded by independent encoders;
  the data they decode to is regenerated by `vec_gen.h`. Every vector must
  decode exactly, refuse a buffer one byte short, write nothing past its
  data, and decode the same streamed — output buffers of 1, 7 and 4093
  bytes, input from memory and pulled 1 and 977 bytes at a time; every
  proper prefix must fail (or, where the format lets a stream stop early,
  decode exactly); a flipped bit at every byte must not crash or overrun,
  and in bzip2 and xz must be caught by their checksums unless it lands in
  bits the decoder never reads. It also covers concatenated streams, the
  unsupported paths, exact consumption (a trailer after a DEFLATE stream),
  pull errors, chained decoders, RLE90's two dialects, Compact Pro's RLE,
  StuffIt's LZW, GSRLE, the checksums' published check values, and the
  encoders: every data set round-trips through zlib, xz and GSRLE at levels
  1, 6 and 9 (through a reused encoder and one-shot) and refuses a buffer
  one byte short, and a 5 MiB input crosses every LZMA2 limit. `stuffit.c`
  tests methods 3, 13 and 15 against streams built by encoders over the
  decoders' own tables and models (including the Arsenic arithmetic
  coder), and the Huffman pool's bounds.
- `tests/unit/suites/peeler` and `peeler_corpus` (and `make -C src/peeler
  test`, 38 real archives) decode every method through the containers;
  `zipgz.c` covers DEFLATE with a writer of its own (every block type,
  resuming at every output boundary, truncation, bad codes).
- `tests/unit/suites/udif` decodes every compressed chunk type and reads a
  whole in-memory image with a chunk of every type through the image layer;
  `ndif` covers ADC, `resfork` dcmp 0, 2 and 3 (and the 16 MiB bound),
  `checkpoint` GSRLE across its 4 MiB pieces.
- `tests/unit/suites/udif_writer` covers the DEFLATE encoder (every level,
  the block-form choice, length-limited codes), Adler-32 and the zero-run
  CRC, and xz images (`writer_xz_round_trips`). The `image-udif-boot`
  integration row exports a booted disk with `codec="xz"` and checks it
  decodes to the zlib export's bytes.
- The UDIF interop workflow ([`udif-interop.yml`](../../../.github/workflows/udif-interop.yml))
  has hdiutil open the writer's samples, xz ones included, and convert
  some to UDCO, UDZO, UDBZ, ULFO and ULMO on macOS; the `udif_writer` suite
  (`GS_UDIF_FOREIGN_DIR`) reads each conversion back through the image
  layer on Linux, verifying hdiutil's checksums and comparing every byte.

## 7. Known debts

- bzip2 and xz now stream like every other decoder, but peeler does not yet
  unwrap `.bz2` and `.xz` files (or `.tar.bz2` / `.tar.xz`) as it does
  gzip: a container to add to peeler, not a codec.
- QCOW2's zstd clusters are refused: there is no zstd decoder.
- The xz encoder's parse is heuristic: about 5% larger than `xz -6`, on a
  par with `xz -0`. A price-based (optimal) parse would close most of it.
  The xz decoder runs at about half liblzma's speed on binary data and a
  fifth on text (§3.1).
- dcmp decodes a resource whole before handing out its first byte (§3): fine
  for resources, which are small, but not a streaming decoder in the sense
  of the others.

## 8. See also

- [`../../reference/formats/udif.md`](../../reference/formats/udif.md) — the
  UDIF chunk types.
- [`../core/storage/image.md`](../core/storage/image.md) — the image layer
  and the UDIF writer.
- [`../../guide/peeler.md`](../../guide/peeler.md) — peeler, the archive
  library built on this one.
- `src/peeler/docs/` — the StuffIt, Compact Pro and BinHex format notes the
  classic codecs follow.
