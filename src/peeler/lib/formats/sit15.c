// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// sit15.c — StuffIt method 15 ("Arsenic") decompressor.
//
// Format spec: sit15.md
//
// This is an internal helper called by sit.c for entries compressed with
// method 15.  It is not part of the public API.
//
// The Arsenic format is a block-based pipeline (sit15.md § 2 "Compression
// Pipeline Overview"):
//   Arithmetic decode → Zero-RLE expand → MTF invert → Inverse BWT
//     → Randomization de-scramble → Final RLE expand

#include "internal.h"

// ============================================================================
// Bitstream Reader — sit15.md §3.1 "Byte-to-Bit Extraction"
// ============================================================================

// Bitstream state — MSB-first extraction from a byte buffer.
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;          // next byte to consume
    uint32_t       window;       // left-aligned shift register
    int            avail;        // valid bits in window (MSB end)
} bs_reader;

// Forward declaration of the error-abort function (needs the full state).
typedef struct arsenic_state arsenic_state;
static void arsenic_abort(arsenic_state *s, const char *fmt, ...);

// Initialise a bitstream reader over a byte buffer.
static void bs_init(bs_reader *r, const uint8_t *buf, size_t len)
{
    r->data   = buf;
    r->len    = len;
    r->pos    = 0;
    r->window = 0;
    r->avail  = 0;
}

// Pull whole bytes into the shift register until we have ≥24 bits or exhausted input.
static void bs_refill(bs_reader *r)
{
    while (r->avail <= 24 && r->pos < r->len) {
        r->window |= (uint32_t)r->data[r->pos++] << (24 - r->avail);
        r->avail  += 8;
    }
}

// Read exactly n bits (1 ≤ n ≤ 25).  Aborts via longjmp on underflow.
static uint32_t bs_read(arsenic_state *s, int n);

// Read n bits that may exceed 25, by splitting into two reads.
static uint32_t bs_read_long(arsenic_state *s, int n);

// ============================================================================
// Adaptive Probability Model — sit15.md §4.1 "Probability Model"
// ============================================================================

#define MODEL_MAX_SYMS 128

// Per-symbol probability model with periodic halving.
typedef struct {
    int nsyms;
    int base_sym;         // symbol value of index 0
    int step;             // increment per decode
    int ceiling;          // rescale when total > ceiling
    int total;
    int freq[MODEL_MAX_SYMS];
} prob_model;

// Initialise a probability model with the given symbol range and parameters.
static void model_setup(prob_model *m, int lo, int hi, int step, int ceiling)
{
    m->nsyms    = hi - lo + 1;
    m->base_sym = lo;
    m->step     = step;
    m->ceiling  = ceiling;
    m->total    = m->nsyms * step;
    for (int i = 0; i < m->nsyms; i++)
        m->freq[i] = step;
}

// Reset all frequencies to their initial values.
static void model_reset(prob_model *m)
{
    m->total = m->nsyms * m->step;
    for (int i = 0; i < m->nsyms; i++)
        m->freq[i] = m->step;
}

// Update the model after decoding symbol at the given index.
static void model_bump(prob_model *m, int idx)
{
    m->freq[idx] += m->step;
    m->total     += m->step;
    if (m->total > m->ceiling) {
        m->total = 0;
        for (int i = 0; i < m->nsyms; i++) {
            m->freq[i] = (m->freq[i] + 1) >> 1;   // halve, round up
            m->total   += m->freq[i];
        }
    }
}

// ============================================================================
// Arithmetic Decoder — sit15.md §4.2 "Decoder State", §4.3 "Decoding One Symbol"
// ============================================================================

#define AC_PREC  26
#define AC_ONE   (1 << (AC_PREC - 1))   // 2^25
#define AC_HALF  (1 << (AC_PREC - 2))   // 2^24

// Arithmetic decoder range/code register pair.
typedef struct {
    int range;
    int code;
} ac_state;

// ============================================================================
// Move-To-Front Table — sit15.md §6.3 "Move-To-Front Decode"
// ============================================================================

// MTF table mapping decoder indices back to byte values.
typedef struct {
    uint8_t tbl[256];
} mtf_table;

// Initialise the MTF table to the identity permutation.
static void mtf_init(mtf_table *m)
{
    for (int i = 0; i < 256; i++)
        m->tbl[i] = (uint8_t)i;
}

// Decode an MTF index: extract the byte at position idx and move it to front.
static uint8_t mtf_decode(mtf_table *m, int idx)
{
    uint8_t val = m->tbl[idx];
    if (idx > 0)
        memmove(&m->tbl[1], &m->tbl[0], (size_t)idx);
    m->tbl[0] = val;
    return val;
}

// ============================================================================
// Randomization Table — sit15.md §9.3 "Randomization Table"
// ============================================================================

// 256-entry bzip2-lineage randomization table (sit15.md §9.3).
// clang-format off
static const uint16_t rand_tbl[256] = {
    0xEE,0x56,0xF8,0xC3, 0x9D,0x9F,0xAE,0x2C, 0xAD,0xCD,0x24,0x9D, 0xA6,0x101,0x18,0xB9,
    0xA1,0x82,0x75,0xE9, 0x9F,0x55,0x66,0x6A, 0x86,0x71,0xDC,0x84, 0x56,0x96,0x56,0xA1,
    0x84,0x78,0xB7,0x32, 0x6A,0x03,0xE3,0x02, 0x11,0x101,0x08,0x44, 0x83,0x100,0x43,0xE3,
    0x1C,0xF0,0x86,0x6A, 0x6B,0x0F,0x03,0x2D, 0x86,0x17,0x7B,0x10, 0xF6,0x80,0x78,0x7A,
    0xA1,0xE1,0xEF,0x8C, 0xF6,0x87,0x4B,0xA7, 0xE2,0x77,0xFA,0xB8, 0x81,0xEE,0x77,0xC0,
    0x9D,0x29,0x20,0x27, 0x71,0x12,0xE0,0x6B, 0xD1,0x7C,0x0A,0x89, 0x7D,0x87,0xC4,0x101,
    0xC1,0x31,0xAF,0x38, 0x03,0x68,0x1B,0x76, 0x79,0x3F,0xDB,0xC7, 0x1B,0x36,0x7B,0xE2,
    0x63,0x81,0xEE,0x0C, 0x63,0x8B,0x78,0x38, 0x97,0x9B,0xD7,0x8F, 0xDD,0xF2,0xA3,0x77,
    0x8C,0xC3,0x39,0x20, 0xB3,0x12,0x11,0x0E, 0x17,0x42,0x80,0x2C, 0xC4,0x92,0x59,0xC8,
    0xDB,0x40,0x76,0x64, 0xB4,0x55,0x1A,0x9E, 0xFE,0x5F,0x06,0x3C, 0x41,0xEF,0xD4,0xAA,
    0x98,0x29,0xCD,0x1F, 0x02,0xA8,0x87,0xD2, 0xA0,0x93,0x98,0xEF, 0x0C,0x43,0xED,0x9D,
    0xC2,0xEB,0x81,0xE9, 0x64,0x23,0x68,0x1E, 0x25,0x57,0xDE,0x9A, 0xCF,0x7F,0xE5,0xBA,
    0x41,0xEA,0xEA,0x36, 0x1A,0x28,0x79,0x20, 0x5E,0x18,0x4E,0x7C, 0x8E,0x58,0x7A,0xEF,
    0x91,0x02,0x93,0xBB, 0x56,0xA1,0x49,0x1B, 0x79,0x92,0xF3,0x58, 0x4F,0x52,0x9C,0x02,
    0x77,0xAF,0x2A,0x8F, 0x49,0xD0,0x99,0x4D, 0x98,0x101,0x60,0x93, 0x100,0x75,0x31,0xCE,
    0x49,0x20,0x56,0x57, 0xE2,0xF5,0x26,0x2B, 0x8A,0xBF,0xDE,0xD0, 0x83,0x34,0xF4,0x17,
};
// clang-format on

// ============================================================================
// Master Decompressor State
// ============================================================================

// Complete state for one Arsenic decompression session.
// sit15.md §11.2 "Memory Allocation" — blk_buf (1×cap) + lf_map (4×cap)
//   = 5 × block_size bytes, up to 80 MiB at B=15.
// sit15.md §11.3 "Demand-Driven Block Decoding" — blocks decoded lazily
//   when out_pos ≥ blk_len; buffers reused without reallocation.
struct arsenic_state {
    // Error recovery (§10)
    decode_ctx_t *ctx;
    bool     eos;                   // end-of-stream seen in a block footer

    // Bitstream (§3)
    bs_reader bits;

    // Arithmetic decoder (§4.2)
    ac_state  ac;

    // Probability models (§4.1, §5, §6)
    prob_model m_primary;           // persists across blocks
    prob_model m_sel;               // per-block selector model
    prob_model m_grp[7];            // per-block MTF group models

    // Block geometry (§5.1)
    int block_exp;                  // B from the header (0..15)
    int blk_cap;                    // 1 << (B+9)

    // Block data buffer (§6)
    uint8_t  *blk_buf;             // decoded MTF output, blk_cap bytes
    uint32_t *lf_map;              // inverse-BWT LF-mapping, blk_cap entries
    int       blk_len;              // actual decoded length of current block
    int       bwt_origin;           // BWT primary index

    // Output cursor within current block
    int       out_pos;              // bytes emitted from block so far
    int       bwt_idx;              // current LF-mapping chase index

    // Randomization (§9)
    bool      randomized;
    int       rand_ti;              // table index
    int       rand_next;            // next position to XOR

    // Final RLE (§8)
    int       rle_prev;             // last emitted byte value
    int       rle_streak;           // consecutive identical count (0-4)
    int       rle_repeat;           // buffered repeat bytes still to emit
};

// ============================================================================
// Error Handling
// ============================================================================

// Abort decompression with a printf-style error message.
// sit15.md §10 "Error Conditions" — all fatal conditions funnel here.
// sit15.md §11.1 "Error Recovery" — longjmp unwinds to the setjmp site.
static void arsenic_abort(arsenic_state *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->ctx->errmsg, sizeof(s->ctx->errmsg), fmt, ap);
    va_end(ap);
    longjmp(s->ctx->jmp, 1);
}

// ============================================================================
// Bitstream Implementation
// ============================================================================

// sit15.md §3.1 "Byte-to-Bit Extraction" — shift-register: reads top n
//   bits via window >> (32−n), refills when avail ≤ 24.  Max single
//   read 25 bits; bs_read_long splits wider fields (e.g. 26-bit AC
//   bootstrap) into two reads.
static uint32_t bs_read(arsenic_state *s, int n)
{
    bs_reader *r = &s->bits;
    if (n > r->avail) {
        bs_refill(r);
        if (n > r->avail)
            arsenic_abort(s, "sit15: bitstream exhaustion");
    }
    uint32_t v = r->window >> (32 - n);
    r->window <<= n;
    r->avail  -= n;
    return v;
}

// sit15.md §3.1 "Byte-to-Bit Extraction" — Read-long: splits reads
//   wider than 25 bits into (25) + (n−25), assembling (hi << rem) | lo.
//   Used for the 26-bit AC bootstrap (§4.2).
static uint32_t bs_read_long(arsenic_state *s, int n)
{
    if (n <= 25)
        return bs_read(s, n);
    uint32_t hi = bs_read(s, 25);
    uint32_t lo = bs_read(s, n - 25);
    return (hi << (n - 25)) | lo;
}

// ============================================================================
// Arithmetic Decode Helpers
// ============================================================================

// Decode one arithmetic-coded symbol from the given model.
// sit15.md §4.3 "Decoding One Symbol" — scale the range, find the
// symbol via cumulative frequency, narrow interval, renormalize.
static int ac_decode_sym(arsenic_state *s, prob_model *m)
{
    if (m->total == 0)
        arsenic_abort(s, "sit15: model total frequency is zero");

    int scale = s->ac.range / m->total;
    if (scale == 0)
        arsenic_abort(s, "sit15: arithmetic decoder scale is zero");

    int target = s->ac.code / scale;

    // Walk the cumulative distribution to find the symbol.
    int cum = 0, k;
    for (k = 0; k < m->nsyms - 1; k++) {
        if (cum + m->freq[k] > target)
            break;
        cum += m->freq[k];
    }

    int lo  = cum;
    int hi  = cum + m->freq[k];
    int w   = m->freq[k];

    // Narrow the interval.
    int base_off = scale * lo;
    s->ac.code -= base_off;
    if (hi == m->total)
        s->ac.range -= base_off;
    else
        s->ac.range = w * scale;

    // Renormalize (§4.3 step 6).
    while (s->ac.range <= AC_HALF) {
        s->ac.range <<= 1;
        s->ac.code   = (int)(((uint32_t)s->ac.code << 1) | bs_read(s, 1));
    }

    model_bump(m, k);
    return m->base_sym + k;
}

// Decode an n-bit integer from a binary model (LSB-first assembly).
// sit15.md §3.2 "Arithmetic-Coded Multi-Bit Fields" and §4.4
// "Decoding a Multi-Bit Field".
static int ac_decode_field(arsenic_state *s, prob_model *m, int n)
{
    int val = 0;
    for (int i = 0; i < n; i++) {
        if (ac_decode_sym(s, m))
            val |= 1 << i;
    }
    return val;
}

// ============================================================================
// Inverse BWT — sit15.md §7.2 "Build the LF-Mapping Table"
// ============================================================================

// Build the LF-mapping permutation table from the decoded block data.
static void build_lf_map(uint32_t *map, const uint8_t *buf, int len)
{
    int freq[256];
    int base[256];
    int seen[256];

    memset(freq, 0, sizeof freq);
    for (int i = 0; i < len; i++)
        freq[buf[i]]++;

    int acc = 0;
    for (int c = 0; c < 256; c++) {
        base[c] = acc;
        acc += freq[c];
    }

    memset(seen, 0, sizeof seen);
    for (int i = 0; i < len; i++) {
        int c = buf[i];
        map[base[c] + seen[c]] = (uint32_t)i;
        seen[c]++;
    }
}

// ============================================================================
// BWT + Randomization Output — sit15.md §7.3, §9
// ============================================================================

// Emit one byte from the inverse BWT and apply randomization if active.
// sit15.md §7.3 "Reconstruct Original Bytes" — chase the LF-mapping.
// sit15.md §9 "Randomization" — XOR with 1 at positions determined by
// the randomization table.
static uint8_t emit_bwt_byte(arsenic_state *s)
{
    // Follow one step of the LF-mapping chain.
    s->bwt_idx = (int)s->lf_map[s->bwt_idx];
    if (s->bwt_idx < 0 || s->bwt_idx >= s->blk_len)
        arsenic_abort(s, "sit15: BWT index out of bounds");
    uint8_t b = s->blk_buf[s->bwt_idx];

    // §9.2  Randomization de-scramble.
    if (s->randomized && s->rand_next == s->out_pos) {
        b ^= 1;
        s->rand_ti   = (s->rand_ti + 1) & 0xFF;
        s->rand_next += rand_tbl[s->rand_ti];
    }
    s->out_pos++;
    return b;
}

// ============================================================================
// Block Decoding — sit15.md §5.2, §6
// ============================================================================

// Per-block model parameters (§5.2.2).
// sit15.md §5.2.2 "Block Data" — seven group models partition the
// MTF index space into ranges with different step/ceiling.
static const int grp_lo[]   = {  2,   4,   8,  16,  32,  64, 128 };
static const int grp_hi[]   = {  3,   7,  15,  31,  63, 127, 255 };
static const int grp_step[] = {  8,   4,   4,   4,   2,   2,   1 };

// Consume a zero-run from the selector stream (§6.2).
// sit15.md §6.2 "Zero Run-Length Decoding" — bijective positional
// accumulation: selector token t at ordinal position p contributes
// (t + 1) << p to the total.  Returns the accumulated zero count.
// *out_sel receives the first non-run selector (≥ 2) that ends the sub-loop.
static int consume_zero_run(arsenic_state *s, int first_tok, int *out_sel)
{
    int total   = 0;
    int bit_pos = 0;
    int tok     = first_tok;

    do {
        total += (tok + 1) << bit_pos;
        bit_pos++;
        tok = ac_decode_sym(s, &s->m_sel);
    } while (tok < 2);

    *out_sel = tok;
    return total;
}

// Decode a complete block: selector loop → MTF → BWT prep.
static void decode_block(arsenic_state *s)
{
    // (Re)initialise per-block models.
    model_setup(&s->m_sel, 0, 10, 8, 1024);
    for (int g = 0; g < 7; g++)
        model_setup(&s->m_grp[g], grp_lo[g], grp_hi[g], grp_step[g], 1024);

    mtf_table mtf;
    mtf_init(&mtf);

    // §5.2.1  Block header (via primary model).
    s->randomized = ac_decode_sym(s, &s->m_primary) != 0;
    s->bwt_origin = ac_decode_field(s, &s->m_primary, s->block_exp + 9);
    s->blk_len    = 0;

    // §6.1–6.3  Main selector loop — fills blk_buf with MTF-decoded bytes.
    int sel = ac_decode_sym(s, &s->m_sel);
    while (sel != 10) {

        // Zero-run tokens (sel 0 or 1): decode via positional accumulator.
        if (sel < 2) {
            int trailing;
            int run_len = consume_zero_run(s, sel, &trailing);

            if (s->blk_len + run_len > s->blk_cap)
                arsenic_abort(s, "sit15: block buffer overflow (zero run)");

            uint8_t fill = mtf_decode(&mtf, 0);
            memset(s->blk_buf + s->blk_len, fill, (size_t)run_len);
            s->blk_len += run_len;

            // The trailing selector that ended the run is our next token.
            sel = trailing;
            continue;
        }

        // Literal / group-coded symbol (sel 2 … 9).
        int mtf_idx = (sel == 2) ? 1
                                 : ac_decode_sym(s, &s->m_grp[sel - 3]);

        if (s->blk_len >= s->blk_cap)
            arsenic_abort(s, "sit15: block buffer overflow");
        s->blk_buf[s->blk_len++] = mtf_decode(&mtf, mtf_idx);

        sel = ac_decode_sym(s, &s->m_sel);
    }

    // Validate BWT primary index (§10).
    // sit15.md §10 "Error Conditions" — primary index must be < block length.
    if (s->blk_len > 0 && s->bwt_origin >= s->blk_len)
        arsenic_abort(s, "sit15: BWT primary index >= block length");

    // §5.2.3  Reset per-block models, then read footer via primary model.
    model_reset(&s->m_sel);
    for (int g = 0; g < 7; g++)
        model_reset(&s->m_grp[g]);

    if (ac_decode_sym(s, &s->m_primary)) {
        // End-of-stream: read (and discard) the 32-bit CRC.
        ac_decode_field(s, &s->m_primary, 32);
        s->eos = true;
    }

    // §7.2  Build inverse-BWT LF-mapping.
    if (s->blk_len > 0)
        build_lf_map(s->lf_map, s->blk_buf, s->blk_len);

    // Prepare output cursor for this block.
    s->out_pos     = 0;
    s->bwt_idx     = s->bwt_origin;
    s->rand_ti     = 0;
    s->rand_next   = rand_tbl[0];
    s->rle_prev    = 0;
    s->rle_streak  = 0;
    s->rle_repeat  = 0;
}

// ============================================================================
// Final RLE Expansion — sit15.md §8 "Final Run-Length Expansion"
// ============================================================================

// Produce one decompressed output byte through the final RLE stage.
// sit15.md §8 "Final Run-Length Expansion" — after 4 identical bytes
// the next upstream byte K encodes K additional copies (total = 4 + K).
static uint8_t produce_byte(arsenic_state *s)
{
    // Iterative RLE expansion loop (§8).
    // sit15.md §8 "Final Run-Length Expansion" — after 4 identical bytes
    // the next upstream byte K encodes K additional copies.
    // sit15.md §11.5 "Additional Notes" — K=0: extension byte consumed
    // and discarded, not re-interpreted as data; loop fetches fresh byte.
    for (;;) {
        // 1.  Drain buffered repeats from a prior extension count.
        if (s->rle_repeat > 0) {
            s->rle_repeat--;
            return (uint8_t)s->rle_prev;
        }

        // 2.  Fetch the next block when the current one is exhausted.
        if (s->out_pos >= s->blk_len) {
            if (s->eos)
                arsenic_abort(s, "sit15: unexpected end of stream");
            decode_block(s);
        }

        uint8_t b = emit_bwt_byte(s);

        // 3.  After 4 identical bytes the next upstream byte is the
        //     extension count K (0 … 255).  Total run = 4 + K.
        if (s->rle_streak == 4) {
            s->rle_streak = 0;
            if (b > 0) {
                s->rle_repeat = (int)b - 1;
                return (uint8_t)s->rle_prev;
            }
            // K == 0: run was exactly 4 (already emitted). Loop back.
            continue;
        }

        // 4.  Track how many consecutive identical bytes we've seen.
        if (b != (uint8_t)s->rle_prev) {
            s->rle_prev   = b;
            s->rle_streak = 1;
        } else {
            s->rle_streak++;
        }
        return b;
    }
}

// ============================================================================
// Stream Header — sit15.md §5.1 "Stream Header"
// ============================================================================

// Parse the Arsenic stream header: signature, block-size exponent, initial EOS.
static bool parse_header(arsenic_state *s)
{
    // §4.2  Bootstrap the arithmetic decoder.
    s->ac.range = AC_ONE;
    s->ac.code  = (int)bs_read_long(s, AC_PREC);

    // §5.1  Primary model: symbols {0,1}, increment 1, limit 256.
    model_setup(&s->m_primary, 0, 1, 1, 256);

    // Signature "As" (each byte is an 8-bit arithmetic-coded field).
    if (ac_decode_field(s, &s->m_primary, 8) != 'A')
        arsenic_abort(s, "sit15: invalid signature (expected 'A')");
    if (ac_decode_field(s, &s->m_primary, 8) != 's')
        arsenic_abort(s, "sit15: invalid signature (expected 's')");

    // Block-size exponent B (4-bit field) → block_size = 1 << (B+9).
    s->block_exp = ac_decode_field(s, &s->m_primary, 4);
    unsigned bsz = 1u << (unsigned)(s->block_exp + 9);
    if (bsz > (unsigned)INT32_MAX)
        arsenic_abort(s, "sit15: block size overflow");
    s->blk_cap = (int)bsz;

    // Initial end-of-stream flag.
    s->eos = ac_decode_sym(s, &s->m_primary) != 0;

    // Allocate block buffers.
    s->blk_buf = malloc((size_t)s->blk_cap);
    s->lf_map  = malloc((size_t)s->blk_cap * sizeof(uint32_t));
    if (!s->blk_buf || !s->lf_map)
        arsenic_abort(s, "sit15: out of memory allocating block buffers");

    return true;
}

// Release the two block-level buffers.
// sit15.md §11.2 "Memory Allocation" — releases blk_buf + lf_map.
static void free_buffers(arsenic_state *s)
{
    free(s->blk_buf);
    free(s->lf_map);
    s->blk_buf = NULL;
    s->lf_map  = NULL;
}

// ============================================================================
// Entry Point (Internal)
// ============================================================================

// Decompress method-15 (Arsenic) compressed data into a freshly allocated buffer.
// Called by sit.c for entries using compression method 15.
//
// sit15.md § "Appendix A: Complete Decompression Walkthrough"
//   1. Parse stream header, bootstrap arithmetic decoder.
//   2. Decode blocks (selector loop → MTF → inverse BWT).
//   3. Expand via randomization + final RLE.
//   4. Return the output buffer.
peel_buf_t peel_sit15(const uint8_t *src, size_t len, size_t uncomp_len, peel_err_t **err) {
    *err = NULL;

    // Handle degenerate case: zero-length output
    if (uncomp_len == 0) {
        return (peel_buf_t){.data = NULL, .size = 0, .owned = false};
    }

    // Allocate the output buffer up front (known size from container metadata)
    uint8_t *out = malloc(uncomp_len);
    if (!out) {
        *err = make_err("sit15: out of memory allocating %zu-byte output buffer", uncomp_len);
        return (peel_buf_t){0};
    }

    // Use setjmp/longjmp for deep-error abort during decompression
    decode_ctx_t dctx;
    if (setjmp(dctx.jmp) != 0) {
        // Arrived here via arsenic_abort — propagate the error message
        free(out);
        *err = make_err("%s", dctx.errmsg);
        return (peel_buf_t){0};
    }

    // The decoder state is large, so heap-allocate to avoid stack overflow.
    arsenic_state *s = calloc(1, sizeof *s);
    if (!s) {
        free(out);
        *err = make_err("sit15: out of memory allocating decoder state");
        return (peel_buf_t){0};
    }

    // Wire up the decode context for longjmp error handling
    s->ctx = &dctx;

    // Initialise the bit reader over the compressed input
    bs_init(&s->bits, src, len);

    // Parse the Arsenic stream header (signature, block size, initial EOS)
    parse_header(s);

    // Decompress uncomp_len bytes through the full pipeline
    for (size_t i = 0; i < uncomp_len; i++)
        out[i] = produce_byte(s);

    // Clean up decoder state
    free_buffers(s);
    free(s);

    return (peel_buf_t){.data = out, .size = uncomp_len, .owned = true};
}
