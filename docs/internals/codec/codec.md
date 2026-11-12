# Codec library

The codec library, [`src/codec/`](../../../src/codec/), is the one place the
tree compresses, decompresses and checksums data: a C99 library with one
public header, [`codec.h`](../../../src/codec/include/codec.h), and no
dependency beyond libc. Its sources are `lib/inflate.c` and `lib/deflate.c`
(DEFLATE / zlib), `lib/adc.c`, `lib/bzip2.c`, `lib/lzfse.c` (with LZVN),
`lib/lzma.c` (LZMA, LZMA2, xz, `.lzma`), `lib/lzma_enc.c` (the xz
encoder), `lib/checksum.c` and `lib/codec.c` (dispatch by id);
`lib/codec_internal.h` and `lib/lzma_internal.h` (the LZMA model the
decoder and encoder share) are private to them.

## 1. Responsibilities & design

Every caller that meets a compressed stream goes through the library, so
each algorithm has one implementation:

| Caller | Uses |
|---|---|
| peeler: zip and gzip members (`src/peeler/lib/formats/zip.c`, `gz.c`) | the resumable inflater, CRC-32 |
| UDIF chunks (`src/core/storage/image_udif.c`) | ADC, zlib, bzip2, LZFSE, xz, by chunk type |
| NDIF chunks (`image_ndif.c`) | ADC |
| QCOW2 clusters (`image_qcow2.c`) | raw DEFLATE |
| the UDIF writer (`udif_writer.c`) | the zlib encoder (or, opted into, the xz encoder), CRC-32 (and its zero-run form) |
| the PNG reader and writer (`src/core/debug/debug.c`), the ImageWriter's page store (`iw_page.c`) and the PDF writer (`pdf_writer.c`) | zlib, both ways |
| MOOF, AFP's append logs, BMAC, the Rage 128 CCE, the PCI option-ROM identity | CRC-32 |

The library is a sibling of peeler rather than part of it: peeler is an
archive reader, while the codecs are services its archives, the image layer
and the writers share. peeler depends on the library (its include list
carries the codec's — `src/sources.mk`), never the other way round, and
nothing in the library includes anything from `src/core` or `src/peeler`.

Every decoder is first-party, written from the formats' published
descriptions; no reference implementation's code was translated into it.
Independent encoders (zlib, libbz2 and liblzma through Python, Apple's
LZFSE encoder through pyliblzfse) are used only to generate test vectors
(§6).

All one-shot decoders share one contract: read `len` bytes of `src`, write
at most `cap` bytes to `dst`, return the count written or a negative
`CODEC_ERR_*` code (`DATA`, `TRUNCATED`, `OVERFLOW`, `NOMEM`,
`UNSUPPORTED`, `CHECKSUM`, `ARG`). Decoding past `cap` is
`CODEC_ERR_OVERFLOW`, never a silent clamp. ADC is the one exception: its
stream has no end marker, so it stops when either buffer runs out and the
caller compares the count with the size it expected. `codec_decode(id, ...)`
reaches every decoder by `codec_id_t`, which is how a container maps its
method field to a codec (`udif_chunk_codec()` for UDIF).

The decoders take untrusted input — disk images and archives from anywhere
— so every length, distance, table index and state is bounds-checked
against the buffers it addresses, and a malformed stream is an error.

## 2. Key types & files

| | |
|---|---|
| `codec_inflater_t` | the resumable raw-DEFLATE decoder: input pulled through a `codec_pull_fn`, output stopping wherever the caller's buffer fills (mid-block, mid-match) and resuming exactly there |
| `codec_deflate_state_t` | reusable encoder state (hash chains, tokens) for inputs up to a size, so a caller compressing many chunks allocates once |
| `codec_id_t` | `CODEC_COPY`, `DEFLATE`, `ZLIB`, `ADC`, `BZIP2`, `LZFSE`, `XZ`, `LZMA_ALONE` |
| `codec_crc32`, `codec_crc32_zeros`, `codec_crc32_msb`, `codec_crc64`, `codec_adler32` | CRC-32 (reflected; zero runs in O(log n)), CRC-32 MSB-first (bzip2), CRC-64/XZ, Adler-32; constant tables, so callable from any thread |

## 3. Behaviour/algorithms

- **DEFLATE.** The inflater decodes Huffman symbols through a 9-bit lookup
  table with a canonical walk for longer codes, keeping a 32 KB window so it
  can resume. `codec_zlib_inflate` checks the RFC 1950 header and, when the
  stream carries one, the Adler-32 trailer (a stream that stops right after
  its final block is accepted, as some writers omit the trailer). The
  encoder is greedy LZ77 over hash chains (depth by level), each block
  written as dynamic Huffman, fixed Huffman or stored, whichever is smallest;
  its dynamic codes are length-limited and complete.
- **bzip2.** Per block: the symbol map, the selectors (move-to-front coded),
  the delta-coded code lengths of up to six canonical Huffman tables, then
  the run-length and move-to-front coded symbols; the inverse
  Burrows-Wheeler transform runs as a linked walk over one 32-bit word per
  byte (symbol in the low byte, link above), and the initial run-length
  coding (four equal bytes then a count) is undone on output. Each block's
  CRC and each stream's combined CRC are checked; concatenated streams
  decode as one. The block buffer is allocated per call (up to 3.6 MB at
  level 9).
- **LZFSE.** Stored (`bvx-`), LZVN (`bvxn`) and v2 (`bvx2`) blocks until
  `bvx$`; matches may reach into earlier blocks. A v2 block's header packs
  the counts, the initial states and the payloads' bit offsets into three
  64-bit words and codes the four frequency tables (literals, L, M, D) with
  a 2- to 14-bit prefix code. The finite-state (tANS) tables give each
  symbol its frequency's worth of consecutive states, each naming the bits
  to read for the next state. Literals decode first, by four interleaved
  states; then each (L, M, D) triple copies L literals and an M-byte match
  (D = 0 repeating the last distance). Both payloads are bit streams read
  backwards from their ends. The unpacked `bvx1` header is an
  encoder-internal form and is refused as unsupported.
- **LZMA / xz.** A binary range decoder with 11-bit adaptive probabilities
  drives the 12-state literal/match machine, the literal coders (selected by
  the previous byte and the position), the length coders and the distance
  coder (6-bit slot, then modelled, direct and aligned bits). The output
  buffer is the dictionary, so a one-shot decode needs no window. LZMA2
  chunks reset the dictionary, the state or the properties as their control
  byte says, and each must decode exactly its stated sizes. The xz container
  is parsed fully — stream header, block headers (only an LZMA2 filter),
  block padding, CRC-32 or CRC-64 checks (SHA-256 is skipped), the index and
  the stream footer — and concatenated streams with zero padding decode as
  one.
- **ADC.** Literal runs and short/long back-references, byte-aligned.
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

## 4. Object-model / shell surface

None. The codecs are reached through their callers: `files.convert`,
`files.verify` and attaching a `.dmg` (UDIF), `files.archive.*` and the VFS
(peeler).

## 5. Checkpointing

None: the library holds no state between calls.

## 6. Testing

- `tests/unit/suites/codec` runs under ASan, UBSan and LeakSanitizer, and
  again on wasm32 (`make -C tests/unit run-wasm32`). Its vectors
  (`vectors.h`, from `make_vectors.py`) are encoded by independent encoders;
  the data they decode to is regenerated by `vec_gen.h`. Every vector must
  decode exactly, refuse a buffer one byte short, write nothing past its
  data; every proper prefix must fail (or, where the format lets a stream
  stop early, decode exactly); a flipped bit at every byte must not crash
  or overrun, and in bzip2 and xz must be caught by their checksums unless
  it lands in bits the decoder never reads. It also
  covers concatenated streams, the unsupported paths and the checksums'
  published check values, and the xz encoder: every data set round-trips
  at levels 1, 6 and 9 (through a reused state and a state of its own),
  refuses a buffer one byte short, and a 5 MiB input crosses every LZMA2
  limit (64 KiB coded, 2 MiB decoded, a stored chunk and the model reset
  after it). xz-utils and Python's `lzma` were checked to read its output
  when it was written; the suite itself depends on neither.
- `tests/unit/suites/udif` decodes every compressed chunk type and reads a
  whole in-memory image with a chunk of every type through the image layer.
- `tests/unit/suites/udif_writer` covers the DEFLATE encoder (every level,
  the block-form choice, length-limited codes), Adler-32 and the zero-run
  CRC, and xz images (`writer_xz_round_trips`: every chunk size, the
  default and extreme levels, LZMA chunks only, smaller than zlib on text,
  level 0 uncompressed). The `image-udif-boot` integration row exports a
  booted disk with `codec="xz"` and checks it decodes to the zlib export's
  bytes.
  `tests/unit/suites/peeler` covers the resumable inflater.
- The UDIF interop workflow ([`udif-interop.yml`](../../../.github/workflows/udif-interop.yml))
  has hdiutil open the writer's samples, xz ones included, and convert
  some to UDCO, UDZO, UDBZ, ULFO and ULMO on macOS; the `udif_writer` suite
  (`GS_UDIF_FOREIGN_DIR`) reads each conversion back through the image
  layer on Linux, verifying hdiutil's checksums and comparing every byte.

## 7. Known debts

- bzip2, LZFSE and xz are one-shot only. A streaming form (as the inflater
  has) would let peeler unwrap `.bz2` and `.xz` files, and `.tar.bz2` /
  `.tar.xz`, as it does gzip.
- QCOW2's zstd clusters are refused: there is no zstd decoder.
- The xz encoder's parse is heuristic: about 5% larger than `xz -6`, on a
  par with `xz -0`. A price-based (optimal) parse would close most of it.
  On data that is mostly incompressible it comes out a fraction of a
  percent larger than deflate, which stores such bytes verbatim.
- Speed, measured on one x86-64 core: LZFSE 150–800 MB/s, zlib 190–300,
  xz 27–130, bzip2 18–32 decoded. The inflater emits a byte at a time, and
  the bzip2 decoder finds each Huffman code by walking the code lengths; a
  lookup table in each would speed up reading UDZO and UDBZ images, whose
  1 MiB chunks hdiutil writes decode whole on a cache miss.
- Compression code that stays with its owner, being part of one format
  rather than a general codec: the System 7 resource decompressors
  (`src/core/storage/rsrc_dcmp.c`), the checkpoint file's RLE
  (`src/core/checkpoint.c`), and StuffIt's and Compact Pro's methods
  (peeler's `formats/`).

## 8. See also

- [`../../reference/formats/udif.md`](../../reference/formats/udif.md) — the
  UDIF chunk types.
- [`../core/storage/image.md`](../core/storage/image.md) — the image layer
  and the UDIF writer.
- [`../../guide/peeler.md`](../../guide/peeler.md) — peeler, the archive
  library built on this one.
