# FLAC (Free Lossless Audio Codec)

FLAC is a lossless audio codec: each block of samples is predicted from the samples before it and the prediction's
error is Rice-coded, channel by channel, with the stereo pair optionally rewritten as a sum and difference first.
Its specification is RFC 9639 [1]. This page covers what a reader of a **fixed-block-size, 16-bit, mono or stereo**
stream needs where the frames use the CONSTANT, VERBATIM and FIXED subframes: the stream header, STREAMINFO, the
frame and subframe layouts, the residual coding, the stereo decorrelation and the two CRCs. LPC subframes, the other
metadata blocks and variable block sizes are named but not described; [1] has them.

## 1. Identification (magic, sizes)

### 1.1 What FLAC is

A native FLAC file is the four bytes `fLaC`, one or more metadata blocks (the first always STREAMINFO), then the
frames [1]. In a container the frames are the packets and the header travels apart: Matroska's `A_FLAC` carries
`fLaC` and the metadata blocks as the track's CodecPrivate [2] (see [matroska.md](matroska.md) §2.4), and every
block is one frame.

All fields are **big-endian** and frames are read **most significant bit first** [1].

### 1.2 Magic

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | `66 4C 61 43` — ASCII `fLaC` |

A frame begins with the 14-bit sync code `11111111111110`, then a reserved 0 bit and the blocking-strategy bit:
`FF F8` for a fixed-block-size stream, `FF F9` for a variable one [1] §9.1.

### 1.3 Sizes

- Block: up to 65,535 samples per channel in a frame, and at least 16 in all but the last frame of a stream [1].
  In a fixed-block-size stream every frame has STREAMINFO's block size except the last, which may be shorter.
- Sample rate: 1..655,350 Hz as STREAMINFO's 20 bits hold it.
- Channels: 1..8; bits per sample 4..32 [1] §8.2. This page covers 1 or 2 channels at 16 bits.

## 2. Layout tables

### 2.1 Metadata block header

| Bits | Field | Meaning |
|---|---|---|
| 1 | last | 1 on the last metadata block before the frames |
| 7 | type | 0 STREAMINFO, 1 PADDING, 2 APPLICATION, 3 SEEKTABLE, 4 VORBIS_COMMENT, 5 CUESHEET, 6 PICTURE |
| 24 | length | bytes of the block's body that follow |

### 2.2 STREAMINFO (34 bytes)

| Bits | Field | Meaning |
|---|---|---|
| 16 | min_block | smallest block size in samples (the last frame excepted) |
| 16 | max_block | largest; equal to min_block in a fixed-block-size stream |
| 24 | min_frame | smallest frame in bytes; 0: not known |
| 24 | max_frame | largest; 0: not known |
| 20 | sample_rate | in Hz |
| 3 | channels − 1 | |
| 5 | bits − 1 | bits per sample |
| 36 | total_samples | per channel; 0: not known |
| 128 | md5 | MD5 of the decoded samples, interleaved, little-endian, at the stated bit depth; all zero: not computed |

### 2.3 Frame header

| Bits | Field | Meaning |
|---|---|---|
| 15 | sync | `111111111111100` |
| 1 | blocking | 0 fixed block size, 1 variable |
| 4 | block size code | 0 reserved; 1: 192; 2–5: 576 × 2^(n−2); 6: 8-bit (size − 1) at the end of the header; 7: 16-bit (size − 1) there; 8–15: 256 × 2^(n−8) |
| 4 | rate code | 0: STREAMINFO's; 1 88.2 k, 2 176.4 k, 3 192 k, 4 8 k, 5 16 k, 6 22.05 k, 7 24 k, 8 32 k, 9 44.1 k, 10 48 k, 11 96 k; 12: 8-bit kHz at the end of the header; 13: 16-bit Hz there; 14: 16-bit tens of Hz there; 15 invalid |
| 4 | channel assignment | 0–7: that many channels plus one, coded independently; 8 left/side; 9 side/right; 10 mid/side; 11–15 reserved |
| 3 | sample size code | 0: STREAMINFO's; 1 8 bits; 2 12; 3 reserved; 4 16; 5 20; 6 24; 7 32 |
| 1 | reserved | 0 |
| 8–56 | number | fixed blocking: the frame number; variable: the first sample's number — in the UTF-8-like code of §3.1 |
| 0, 8 or 16 | block size | when the code is 6 or 7 |
| 0, 8 or 16 | rate | when the code is 12, 13 or 14 |
| 8 | CRC-8 | of every header byte before it (§3.4) |

### 2.4 Subframe

One per channel, in channel order, not byte-aligned:

| Bits | Field | Meaning |
|---|---|---|
| 1 | padding | 0 |
| 6 | type | 0 CONSTANT; 1 VERBATIM; 8–12 FIXED, order type − 8; 32–63 LPC, order type − 31; the rest reserved |
| 1 | wasted flag | 1: a unary count follows |
| unary | wasted − 1 | when flagged: k zeros and a one mean k + 1 wasted bits |

Then, with `bps` the channel's bits per sample (16, or 17 for the side channel of assignments 8, 9 and 10) less
the wasted bits:

| Type | Body |
|---|---|
| CONSTANT | one sample of `bps` bits, two's complement: every sample of the block |
| VERBATIM | every sample, `bps` bits each |
| FIXED, order n | n warm-up samples of `bps` bits, then the residual (§2.5) of the block's other samples |

Every decoded sample is then shifted left by the wasted bits.

### 2.5 Residual

| Bits | Field | Meaning |
|---|---|---|
| 2 | method | 0: 4-bit Rice parameters; 1: 5-bit; 2, 3 reserved |
| 4 | partition order p | 2^p partitions of `block / 2^p` samples; the block size must divide evenly |
| — | partitions | each: its parameter, then its residuals |

A partition's parameter is 4 or 5 bits. The all-ones value (15 or 31) is the **escape**: a 5-bit width w follows,
then the partition's residuals as plain w-bit two's-complement numbers (w = 0: all zero). Any other value k is a
Rice parameter: each residual is coded as `q` zeros, a one, then the `k` low bits of the folded value
`u = q·2^k + low` (§3.2). The first partition holds `block / 2^p − n` residuals (the warm-up samples are not
residuals); the others `block / 2^p`.

### 2.6 Frame footer

Zero bits to the next byte boundary, then a 16-bit CRC-16 of the whole frame from the sync code up to and including
that padding (§3.4).

## 3. Algorithms (checksums, compression)

### 3.1 The frame number

The number is coded like UTF-8 extended to 36 bits [1] §9.1.5: below 0x80 one byte `0xxxxxxx`; otherwise a lead
byte of n ones, a zero and the value's top bits, then n − 1 bytes `10xxxxxx`, n = 2..7 (seven bytes, lead `FE`,
carry 36 bits).

### 3.2 Rice codes and the fold

```
u = (q << k) | low                       // q zeros, a one, then k bits
residual = (u >> 1) ^ -(u & 1)           // 0, 1, 2, 3, 4 ... -> 0, -1, 1, -2, 2 ...
```

An encoder folds the other way, `u = residual >= 0 ? 2·residual : −2·residual − 1`.

### 3.3 Fixed prediction

For order n, sample `x[i]` (i ≥ n) is its residual `e[i]` plus the prediction [1] §9.2.5:

| Order | Prediction |
|---|---|
| 0 | 0 |
| 1 | x[i−1] |
| 2 | 2x[i−1] − x[i−2] |
| 3 | 3x[i−1] − 3x[i−2] + x[i−3] |
| 4 | 4x[i−1] − 6x[i−2] + 4x[i−3] − x[i−4] |

The sum can exceed the sample width in a valid stream's intermediate values; a decoder computes it in 64 bits.

### 3.4 CRCs

Both are MSB-first, initial value 0, no final XOR, over bytes:

- **CRC-8**, polynomial `x^8 + x^2 + x + 1` (0x07), over the frame header up to the CRC byte.
- **CRC-16**, polynomial `x^16 + x^15 + x^2 + 1` (0x8005), over the whole frame up to the CRC-16.

```
crc8(bytes):  c = 0; for b in bytes: c ^= b; repeat 8: c = ((c << 1) ^ (0x07 if c & 0x80 else 0)) & 0xFF
crc16(bytes): c = 0; for b in bytes: c ^= b << 8; repeat 8: c = ((c << 1) ^ (0x8005 if c & 0x8000 else 0)) & 0xFFFF
```

### 3.5 Stereo decorrelation

With subframes a (first) and b (second):

| Assignment | Left | Right |
|---|---|---|
| 8 left/side | a | a − b |
| 9 side/right | a + b | b |
| 10 mid/side | (2a + (b & 1) + b) >> 1 | (2a + (b & 1) − b) >> 1 |

The side channel (b in 8 and 10, a in 9) has one more bit than the stream's samples. Shifts are arithmetic.

## 4. Worked decode of a real specimen

The audio track of a 25.5 s recording of a Macintosh Plus booting [3], 22,255 Hz mono.

Its CodecPrivate, 42 bytes:

```
66 4c 61 43                    fLaC
80 00 00 22                    last block, type 0 STREAMINFO, 34 bytes
10 00 10 00                    min and max block 4096
00 00 00 00 00 00              frame sizes not known
05 6e f0 f0                    rate 0x056EF = 22,255 Hz; channels 0 + 1 = 1; bits 15 + 1 = 16; total samples:
00 00 00 00                      (with the low 4 bits above) 0, not known
00 ... 00 (16 bytes)           MD5 not computed
```

The first frame begins:

```
ff f8                          sync, fixed block size
cd                             block size code 12: 256 × 2^4 = 4096; rate code 13: 16-bit Hz follows
08                             assignment 0: one channel; sample size 4: 16 bits; reserved 0
00                             frame number 0
56 ef                          the rate: 22,255 Hz
de                             CRC-8 of the seven bytes before it
16                             subframe: padding 0, type 0b001011 = 11, FIXED order 3; no wasted bits
```

FFmpeg 6.1.1 decodes the track's 139 frames without error (*observed*), as does the stream's own decoder; both
CRCs check on every frame.

## 5. Open questions

- The MD5 and total-sample fields may be zero ("not computed", "not known") [1] §8.2; whether every player accepts a
  stream whose STREAMINFO leaves them so in a container that states the duration elsewhere has been seen for
  FFmpeg only (*observed*).
- LPC subframes, variable block sizes and the metadata blocks other than STREAMINFO are not described here.

## Appendix A. Granny Smith implementation notes

The screen recorder writes FLAC at the emulated machine's own sample rate, 16-bit, mono or stereo, 4,096 samples a
frame (the last shorter), fixed block size. Each channel's subframe is the cheapest of CONSTANT, VERBATIM and FIXED
orders 0 to 4, with wasted bits, partition orders up to 8 and 4- or 5-bit Rice parameters (never the escape); a
stereo frame is the smallest of the four assignments. STREAMINFO gives the block size and nothing it does not know
at the start (frame sizes, total samples and MD5 are zero). The in-tree decoder reads exactly that and escaped
partitions, refuses LPC, and checks both CRCs. See `docs/internals/codec/codec.md`.

## References

1. M.Q.C. van Beurden, A. Weaver, *Free Lossless Audio Codec (FLAC)*, RFC 9639, IETF, December 2024.
2. *Matroska Media Container Codec Specifications*, IETF CELLAR working group — `A_FLAC`.
3. A Macintosh Plus boot to the floppy prompt, recorded from power-on (specimen) — decoded as in §4.
