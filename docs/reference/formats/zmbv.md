# ZMBV (Zip Motion Blocks Video)

ZMBV is a lossless video codec for computer screens: each frame is either the whole picture or, for every block of
the picture, a motion vector into the previous frame and the XOR of what remains, and all of it goes through one
zlib stream that runs from keyframe to keyframe. It was defined by the DOSBox project as its screen-capture codec
[1]; it is named here only because the bitstream is that program's, and is described so that files in it play in
existing players. The decoder in FFmpeg's libavcodec [2] reads it, and players built on that library play it. This
page covers the bitstream at 8, 15, 16 and 32 bits per pixel; the 1, 2, 4 and 24 bpp formats are listed but not
described.

## 1. Identification (magic, sizes)

### 1.1 What ZMBV is

ZMBV has no file format of its own: it is a stream of frames carried in a container that states the picture's size
and frame rate. In AVI and in Matroska's `V_MS/VFW/FOURCC` mapping the stream is identified by the FourCC `ZMBV`
in a `BITMAPINFOHEADER` [3][4][5] (see [matroska.md](matroska.md) §2.4). Nothing inside a frame gives the picture's
width or height; a decoder takes them from the container.

All multi-byte pixel values are **little-endian**.

### 1.2 Frame identification

The first byte of every frame is its flag byte (§2.1). A keyframe follows it with a six-byte header whose first two
bytes are the version, 0 and 1 (version 0.1); no other version is defined [1][2].

### 1.3 Sizes

- Picture: any width and height the container states; blocks at the right and bottom edges are clipped to the
  picture (§2.3).
- Block: `block_w` x `block_h` pixels, each 1..255 (one byte each in the keyframe header). The block size, the
  pixel format and the compression method hold from a keyframe to the next.
- A frame with nothing changed is an inter frame whose block table is all zero: with zlib, about ten bytes (§4).

## 2. Layout tables

### 2.1 Every frame

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | flags | bit 0: keyframe; bit 1: palette delta (8 bpp inter frames); bits 2–7: 0 |
| 1 | 6 | keyframe header | keyframes only (§2.2) |
| 1 or 7 | rest | data | compressed (§3.1) or not, as the last keyframe header said |

A frame of length zero is not a ZMBV frame: a decoder may take it as the previous picture repeated, but that is the
container's business, not the bitstream's.

### 2.2 Keyframe header

| Offset | Size | Field | Values |
|---|---|---|---|
| 1 | 1 | hi_ver | 0 |
| 2 | 1 | lo_ver | 1 |
| 3 | 1 | compression | 0 none, 1 zlib |
| 4 | 1 | format | pixel format, below |
| 5 | 1 | block_w | 1..255 |
| 6 | 1 | block_h | 1..255 |

| format | Pixel | Bytes |
|---|---|---|
| 0 | none | — |
| 1, 2, 3 | 1, 2, 4 bpp | not described here |
| 4 | 8 bpp: a palette index | 1 |
| 5 | 15 bpp: `x RRRRR GGGGG BBBBB`, a little-endian word | 2 |
| 6 | 16 bpp: `RRRRR GGGGGG BBBBB`, a little-endian word | 2 |
| 7 | 24 bpp | not described here |
| 8 | 32 bpp: `0x00RRGGBB`, a little-endian word — bytes B, G, R, 0 | 4 |

### 2.3 Keyframe data

After decompression (§3.1), a keyframe's data is:

| Part | Size | Content |
|---|---|---|
| palette | 768, format 4 only | 256 entries of R, G, B, one byte each |
| picture | width x height x bytes-per-pixel | the rows top first, each row left to right, no padding |

The data is exactly that long; the whole picture is replaced, and so is the palette.

### 2.4 Inter frame data

Blocks are numbered row by row: `bx = ceil(width / block_w)` across, `by = ceil(height / block_h)` down. Block
(i, j) covers columns `i*block_w ..` and rows `j*block_h ..`, clipped to the picture, so the blocks of the last
column and row may be narrower or shorter.

| Part | Size | Content |
|---|---|---|
| palette delta | 768, format 4 with flag bit 1 only | XOR against the current palette |
| block table | `bx * by * 2`, rounded up to a multiple of 4 | two bytes per block (below), then zero padding |
| deltas | the sum over blocks with a delta of `w * h * bytes-per-pixel` | for each such block in block order, its clipped `w x h` pixels row by row |

The two bytes of block *b* in the table:

| Byte | Bits | Meaning |
|---|---|---|
| 2b | 7..1 | dx, a signed 7-bit value (−64..63): the byte as a signed char, shifted right by one |
| 2b | 0 | 1: a delta for this block follows in the deltas |
| 2b+1 | 7..1 | dy, likewise |
| 2b+1 | 0 | 0 |

## 3. Algorithms (checksums, compression)

### 3.1 The zlib stream

With compression 1, a keyframe's data begins a new zlib stream [6] — a two-byte zlib header, then DEFLATE blocks
[7] — and each inter frame's data is the next piece of the same stream: a decoder resets its inflater at every
keyframe and otherwise inflates each frame's bytes where the last frame's stopped, with the history window carried
over. An encoder therefore ends every frame with a flush that leaves the output on a byte boundary with everything
so far decodable: zlib's `Z_SYNC_FLUSH`, an empty stored block, the bytes `00 00 FF FF` [6][7]. The stream is never
finished (no final block, no Adler-32 is read).

Nothing in ZMBV states a frame's decompressed size; a decoder inflates the frame's bytes whole and checks the
result against §2.3 or §2.4. With compression 0 the frame's data is the decompressed data as it is.

### 3.2 Decoding an inter frame

```
if flags & 2 and format == 4:
    palette[k] ^= data[k]  for k in 0..767;  data advances 768
table = data; deltas = data + ((bx*by*2 + 3) & ~3)
new = a fresh picture
for j in 0..by-1, i in 0..bx-1:
    b = j*bx + i
    dx = signed8(table[2b]) >> 1;  dy = signed8(table[2b+1]) >> 1
    for each pixel (x, y) of block (i, j), clipped:
        sx = x + dx; sy = y + dy
        new[x, y] = old[sx, sy] if (sx, sy) is inside the picture, else 0
    if table[2b] & 1:
        for each pixel (x, y) of the block, rows first:
            new[x, y] ^= next bytes-per-pixel bytes of deltas
picture = new
```

The source of a block is the *previous* frame, never pixels already rebuilt in this one. A vector pointing outside
the picture reads zero there (FFmpeg's decoder does so [2]); an encoder that never writes such vectors avoids the
question. A frame whose palette and pixels did not change is an all-zero table and no deltas.

### 3.3 Colour

8 bpp indices are looked up in the palette. A decoder that widens 5- and 6-bit channels to 8 bits does so by
repeating the top bits (`v5 << 3 | v5 >> 2`, `v6 << 2 | v6 >> 4`), so full scale stays full scale.

### 3.4 Checksums

None. The zlib stream's Adler-32 is never reached, since the stream does not end.

## 4. Worked decode of a real specimen

A 25.5 s recording of a Macintosh Plus booting [8], 512 x 342 at 8 bpp, 1,535 frames, one keyframe. FFmpeg 6.1.1
decodes every frame (*observed*: `ffmpeg -f framecrc` lists 1,535 video frames, no error).

The first frame, 204 bytes of ZMBV:

```
01                 flags: keyframe
00 01              version 0.1
01                 compression: zlib
04                 format: 8 bpp
40 40              blocks 64 x 64: bx = 8, by = 6 (the bottom row 22 rows tall)
78 9c ec c1 01 ... a new zlib stream: 768 palette bytes + 512 x 342 index bytes
```

The second frame, nothing changed, 10 bytes:

```
00                          flags: inter
62 a0 31 00                 the stream continued: a fixed-Huffman block (BFINAL 0, BTYPE 01)
00 00 00 ff ff              the end of that block's bits, then the sync flush's empty stored block
```

Inflated after the first frame's bytes, the second frame's nine bytes give exactly 96 zero bytes: the block table
of 8 x 6 blocks at two bytes each (already a multiple of four), every vector zero and no delta (*observed*, with
the system zlib).

## 5. Open questions

- No written specification exists; the bitstream here is as [1] writes it and [2] reads it. FFmpeg 6.1.1 decodes
  streams of 64 x 64 blocks at 8 and 32 bpp without error (*observed*); 15 and 16 bpp, and which block sizes
  other decoders accept, have not been tried here.
- The 1, 2, 4 and 24 bpp formats are not described: their packing within a byte or word has not been decoded here.
- What a decoder should do with a vector reaching outside the picture is defined only by what FFmpeg does (§3.2).
- Whether players other than those built on FFmpeg read ZMBV in Matroska at all is not known.

## Appendix A. Granny Smith implementation notes

The screen recorder writes 8 bpp (palettised screens), 15, 16 and 32 bpp, always with zlib, 64 x 64 blocks, and
every vector zero: it looks for no motion, and codes only the blocks in the band of rows that changed. A keyframe is
written at the start of each file, at a change of mode, after frames were dropped, and after 600 changed frames.
See `docs/internals/codec/codec.md` and `docs/internals/core/record/record.md`.

## References

1. DOSBox, `src/libs/zmbv/zmbv.cpp` and `zmbv.h` — the encoder and its frame and header layout, the pixel format
   numbers, the block table.
2. FFmpeg, `libavcodec/zmbv.c` — the decoder: version check, compression 0 and 1, the inflater reset at keyframes,
   palette deltas, block vectors and deltas, out-of-picture sources.
3. Microsoft, *BITMAPINFOHEADER structure*, Windows GDI documentation.
4. Microsoft, *AVI RIFF File Reference*, DirectShow documentation.
5. *Matroska Media Container Codec Specifications*, IETF CELLAR working group — `V_MS/VFW/FOURCC`.
6. P. Deutsch, J-L. Gailly, *ZLIB Compressed Data Format Specification version 3.3*, RFC 1950, May 1996.
7. P. Deutsch, *DEFLATE Compressed Data Format Specification version 1.3*, RFC 1951, May 1996.
8. A Macintosh Plus boot to the floppy prompt, recorded from power-on (specimen) — decoded as in §4.
