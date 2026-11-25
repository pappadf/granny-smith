// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// mkv_mux.c
// Matroska (RFC 9559) muxer for the recorder: one ZMBV video track and an
// optional FLAC audio track.  See mkv_mux.h.
//
// File layout, front to back:
//
//   EBML header        DocType "matroska", version 4, read version 2
//   Segment            size "unknown" (8 bytes of ones) until close
//     Void (64 B)      overwritten at close by a SeekHead: Info, Tracks, Cues
//     Info             TimestampScale 1 ms, the apps, a Void that close
//                      turns into the Duration
//     Tracks           video: V_MS/VFW/FOURCC, a BITMAPINFOHEADER with
//                      FourCC ZMBV, DefaultDuration, pixel and display size;
//                      audio: A_FLAC, "fLaC" + the STREAMINFO block, rate,
//                      channels, bit depth
//     Cluster ...      a Timestamp, then SimpleBlocks in time order
//     Cues             at close: a CuePoint per cluster that opens on a
//                      video keyframe
//
// A cluster is built in memory and written whole, its size known, when it
// closes: at a video keyframe, after about a second, at 1 MB, or when a
// block's 16-bit relative timestamp would not reach.  So the file on disk
// is always a header and complete clusters (and possibly the start of one
// whose write was cut), and a reader of an unclosed file stops cleanly at
// the last complete cluster.  Block timestamps are pts rounded to the
// millisecond: frame k of a track with period P ns is at round(k*P / 1e6).
//
// The tracks are interleaved here: packets wait per track until every
// track has one, and the earliest goes out first (video before audio at
// the same time), so a caller may push each track as it is coded.

#include "mkv_mux.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

// ============================================================================
// Constants and macros
// ============================================================================

// Element IDs (RFC 9559 §5.1; EBML's own, RFC 8794).
#define ID_EBML               0x1A45DFA3u
#define ID_EBML_VERSION       0x4286u
#define ID_EBML_READ_VERSION  0x42F7u
#define ID_EBML_MAX_ID_LEN    0x42F2u
#define ID_EBML_MAX_SIZE_LEN  0x42F3u
#define ID_DOCTYPE            0x4282u
#define ID_DOCTYPE_VERSION    0x4287u
#define ID_DOCTYPE_READ_VER   0x4285u
#define ID_VOID               0xECu
#define ID_SEGMENT            0x18538067u
#define ID_SEEKHEAD           0x114D9B74u
#define ID_SEEK               0x4DBBu
#define ID_SEEK_ID            0x53ABu
#define ID_SEEK_POSITION      0x53ACu
#define ID_INFO               0x1549A966u
#define ID_TIMESTAMP_SCALE    0x2AD7B1u
#define ID_MUXING_APP         0x4D80u
#define ID_WRITING_APP        0x5741u
#define ID_DURATION           0x4489u
#define ID_TRACKS             0x1654AE6Bu
#define ID_TRACK_ENTRY        0xAEu
#define ID_TRACK_NUMBER       0xD7u
#define ID_TRACK_UID          0x73C5u
#define ID_TRACK_TYPE         0x83u
#define ID_FLAG_LACING        0x9Cu
#define ID_CODEC_ID           0x86u
#define ID_CODEC_PRIVATE      0x63A2u
#define ID_DEFAULT_DURATION   0x23E383u
#define ID_VIDEO              0xE0u
#define ID_PIXEL_WIDTH        0xB0u
#define ID_PIXEL_HEIGHT       0xBAu
#define ID_DISPLAY_WIDTH      0x54B0u
#define ID_DISPLAY_HEIGHT     0x54BAu
#define ID_AUDIO              0xE1u
#define ID_SAMPLING_FREQUENCY 0xB5u
#define ID_CHANNELS           0x9Fu
#define ID_BIT_DEPTH          0x6264u
#define ID_CLUSTER            0x1F43B675u
#define ID_CLUSTER_TIMESTAMP  0xE7u
#define ID_SIMPLE_BLOCK       0xA3u
#define ID_CUES               0x1C53BB6Bu
#define ID_CUE_POINT          0xBBu
#define ID_CUE_TIME           0xB3u
#define ID_CUE_TRACK_POS      0xB7u
#define ID_CUE_TRACK          0xF7u
#define ID_CUE_CLUSTER_POS    0xF1u

#define TRACK_TYPE_VIDEO 1
#define TRACK_TYPE_AUDIO 2

#define MKV_APP           "granny-smith"
#define MKV_TIMESCALE_NS  1000000u // one block-timestamp tick: 1 ms
#define MKV_SIZE_LEN      8 // a size field patched later is 8 bytes long
#define MKV_SEEK_VOID     64 // bytes reserved for the SeekHead
#define MKV_DURATION_LEN  11 // a Duration element: 2-byte ID, 1-byte size, 8-byte float
#define MKV_BUFFER        (1u << 20) // the write buffer, and the cluster size that closes one
#define MKV_CLUSTER_MS    1000 // a cluster's span of time, about
#define MKV_QUEUE_BYTES   (16u << 20) // packets held for interleaving before they go out anyway
#define MKV_QUEUE_PACKETS 1024
#define BMP_HEADER_LEN    40 // BITMAPINFOHEADER
#define FLAC_PRIVATE_LEN  (4 + 4 + MUX_STREAMINFO_LEN) // "fLaC", a block header, STREAMINFO

// ============================================================================
// Type definitions (private)
// ============================================================================

// One packet waiting to be interleaved.
typedef struct mkv_pkt {
    struct mkv_pkt *next;
    uint64_t pts_ns, end_ns;
    bool key;
    size_t len;
    uint8_t data[];
} mkv_pkt_t;

// A track's waiting packets, oldest first.
typedef struct {
    mkv_pkt_t *head, *tail;
    size_t count;
} mkv_queue_t;

// A cue: a cluster that opens on a video keyframe, and its time.
typedef struct {
    uint64_t ms;
    uint64_t pos; // the cluster's offset from the start of the segment's data
} mkv_cue_t;

// The Matroska muxer.
typedef struct {
    muxer_t base;
    FILE *f;
    int err; // the first write error (negative errno), kept
    uint8_t *buf; // what is not yet written: the header at open, then the open cluster
    size_t len, cap;
    uint64_t written; // bytes in the file
    uint64_t seg_at; // file offset of the Segment's size field
    uint64_t seg_data; // file offset of the Segment's data
    uint64_t seek_void_at, duration_at; // file offsets of the two placeholders
    uint64_t info_pos, tracks_pos; // offsets from the segment's data
    int ntracks;
    uint64_t frame_period_ns;
    mkv_queue_t q[2];
    size_t queued_bytes;
    bool cluster_open;
    uint64_t cluster_ms; // the open cluster's timestamp
    size_t cluster_size_at; // where in buf its size field is
    mkv_cue_t *cues;
    size_t ncues, cues_cap;
    uint64_t end_ns; // the latest end of any packet: the duration
} mkv_mux_t;

// ============================================================================
// Static helpers: the buffer and EBML coding
// ============================================================================

// Room for `n` more bytes in the buffer.  0 or -ENOMEM.
static int reserve(mkv_mux_t *m, size_t n) {
    if (m->len + n <= m->cap)
        return 0;
    size_t cap = m->cap ? m->cap : MKV_BUFFER;
    while (cap < m->len + n)
        cap *= 2;
    uint8_t *b = realloc(m->buf, cap);
    if (!b)
        return m->err ? m->err : (m->err = -ENOMEM);
    m->buf = b;
    m->cap = cap;
    return 0;
}

// Append `n` bytes.
static void put_bytes(mkv_mux_t *m, const void *p, size_t n) {
    if (reserve(m, n) == 0) {
        memcpy(m->buf + m->len, p, n);
        m->len += n;
    }
}

// Append `v` big-endian in `n` bytes.
static void put_be(mkv_mux_t *m, uint64_t v, int n) {
    uint8_t b[8];
    for (int i = 0; i < n; i++)
        b[i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    put_bytes(m, b, (size_t)n);
}

// Bytes an element ID takes (its marker bit is part of it).
static int id_len(uint32_t id) {
    return id > 0xFFFFFF ? 4 : id > 0xFFFF ? 3 : id > 0xFF ? 2 : 1;
}

// Bytes the shortest size field for `n` takes (all-ones values are reserved).
static int size_len(uint64_t n) {
    int len = 1;
    while (len < 8 && n >= (1ull << (7 * len)) - 1)
        len++;
    return len;
}

// A size field of `len` bytes: the length marker, then the value.
static void put_size_n(mkv_mux_t *m, uint64_t n, int len) {
    put_be(m, n | 1ull << (7 * len), len);
}

// Write a size field of `len` bytes for `n` into `p`.
static void poke_size(uint8_t *p, uint64_t n, int len) {
    uint64_t v = n | 1ull << (7 * len);
    for (int i = 0; i < len; i++)
        p[i] = (uint8_t)(v >> (8 * (len - 1 - i)));
}

static void put_id(mkv_mux_t *m, uint32_t id) {
    put_be(m, id, id_len(id));
}

// Bytes an unsigned integer's value takes (at least one).
static int uint_len(uint64_t v) {
    int n = 1;
    while (n < 8 && v >> (8 * n))
        n++;
    return n;
}

// An unsigned integer element.
static void put_uint(mkv_mux_t *m, uint32_t id, uint64_t v) {
    int n = uint_len(v);
    put_id(m, id);
    put_size_n(m, (uint64_t)n, 1);
    put_be(m, v, n);
}

// A binary or string element.
static void put_binary(mkv_mux_t *m, uint32_t id, const void *p, size_t n) {
    put_id(m, id);
    put_size_n(m, n, size_len(n));
    put_bytes(m, p, n);
}

// An 8-byte float element.
static void put_float(mkv_mux_t *m, uint32_t id, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_id(m, id);
    put_size_n(m, 8, 1);
    put_be(m, bits, 8);
}

// Open a master element whose size is patched by end_master: the offset
// of its size field in the buffer.
static size_t begin_master(mkv_mux_t *m, uint32_t id) {
    put_id(m, id);
    size_t at = m->len;
    put_size_n(m, 0, MKV_SIZE_LEN);
    return at;
}

// Close a master element opened at `at`: its size is what follows the field.
static void end_master(mkv_mux_t *m, size_t at) {
    if (m->err)
        return;
    poke_size(m->buf + at, m->len - at - MKV_SIZE_LEN, MKV_SIZE_LEN);
}

// A Void element of exactly `total` (>= 2) bytes.
static void put_void(mkv_mux_t *m, size_t total) {
    int sl = total - 2 < 127 ? 1 : 2;
    put_id(m, ID_VOID);
    put_size_n(m, total - 1 - (size_t)sl, sl);
    if (reserve(m, total) == 0) {
        memset(m->buf + m->len, 0, total - 1 - (size_t)sl);
        m->len += total - 1 - (size_t)sl;
    }
}

// Write the buffer to the file and empty it.
static void flush_buf(mkv_mux_t *m) {
    if (m->err || m->len == 0) {
        m->len = 0;
        return;
    }
    if (fwrite(m->buf, 1, m->len, m->f) != m->len || fflush(m->f) != 0)
        m->err = errno ? -errno : -EIO;
    m->written += m->len;
    m->len = 0;
}

// Overwrite `n` bytes at file offset `at` (already written), then return
// to the end.
static void patch(mkv_mux_t *m, uint64_t at, const uint8_t *p, size_t n) {
    if (m->err)
        return;
    if (fseeko(m->f, (off_t)at, SEEK_SET) != 0 || fwrite(p, 1, n, m->f) != n || fseeko(m->f, 0, SEEK_END) != 0)
        m->err = errno ? -errno : -EIO;
}

// ============================================================================
// Static helpers: the header
// ============================================================================

// The video track's CodecPrivate: a BITMAPINFOHEADER (little-endian) whose
// compression is the FourCC ZMBV.
static void bitmap_header(const mux_video_t *v, uint8_t *b) {
    memset(b, 0, BMP_HEADER_LEN);
    uint32_t image = (uint32_t)v->width * (uint32_t)v->height * (uint32_t)(v->bits_per_pixel / 8);
    uint32_t fields[] = {BMP_HEADER_LEN, (uint32_t)v->width, (uint32_t)v->height};
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            b[4 * i + k] = (uint8_t)(fields[i] >> (8 * k));
    b[12] = 1; // planes
    b[14] = (uint8_t)v->bits_per_pixel;
    memcpy(b + 16, "ZMBV", 4);
    for (int k = 0; k < 4; k++)
        b[20 + k] = (uint8_t)(image >> (8 * k));
}

// The EBML header, the Segment's start and everything before the first
// cluster, into the buffer.
static void write_header(mkv_mux_t *m, const mux_video_t *v, const mux_audio_t *a) {
    size_t at = begin_master(m, ID_EBML);
    put_uint(m, ID_EBML_VERSION, 1);
    put_uint(m, ID_EBML_READ_VERSION, 1);
    put_uint(m, ID_EBML_MAX_ID_LEN, 4);
    put_uint(m, ID_EBML_MAX_SIZE_LEN, 8);
    put_binary(m, ID_DOCTYPE, "matroska", 8);
    put_uint(m, ID_DOCTYPE_VERSION, 4);
    put_uint(m, ID_DOCTYPE_READ_VER, 2);
    end_master(m, at);

    // The Segment, its size unknown until close: all ones.
    put_id(m, ID_SEGMENT);
    m->seg_at = m->len;
    put_be(m, 0x01FFFFFFFFFFFFFFull, MKV_SIZE_LEN);
    m->seg_data = m->len;

    m->seek_void_at = m->len;
    put_void(m, MKV_SEEK_VOID);

    m->info_pos = m->len - m->seg_data;
    at = begin_master(m, ID_INFO);
    put_uint(m, ID_TIMESTAMP_SCALE, MKV_TIMESCALE_NS);
    put_binary(m, ID_MUXING_APP, MKV_APP, strlen(MKV_APP));
    put_binary(m, ID_WRITING_APP, MKV_APP, strlen(MKV_APP));
    // Room for the Duration, known only at close; a Void until then.
    m->duration_at = m->len;
    put_void(m, MKV_DURATION_LEN);
    end_master(m, at);

    m->tracks_pos = m->len - m->seg_data;
    size_t tracks = begin_master(m, ID_TRACKS);
    at = begin_master(m, ID_TRACK_ENTRY);
    put_uint(m, ID_TRACK_NUMBER, 1);
    put_uint(m, ID_TRACK_UID, 1);
    put_uint(m, ID_TRACK_TYPE, TRACK_TYPE_VIDEO);
    put_uint(m, ID_FLAG_LACING, 0);
    put_binary(m, ID_CODEC_ID, "V_MS/VFW/FOURCC", 15);
    uint8_t bmp[BMP_HEADER_LEN];
    bitmap_header(v, bmp);
    put_binary(m, ID_CODEC_PRIVATE, bmp, sizeof(bmp));
    put_uint(m, ID_DEFAULT_DURATION, v->frame_period_ns);
    size_t video = begin_master(m, ID_VIDEO);
    put_uint(m, ID_PIXEL_WIDTH, (uint64_t)v->width);
    put_uint(m, ID_PIXEL_HEIGHT, (uint64_t)v->height);
    put_uint(m, ID_DISPLAY_WIDTH, (uint64_t)(v->display_width ? v->display_width : v->width));
    put_uint(m, ID_DISPLAY_HEIGHT, (uint64_t)(v->display_height ? v->display_height : v->height));
    end_master(m, video);
    end_master(m, at);
    if (a) {
        at = begin_master(m, ID_TRACK_ENTRY);
        put_uint(m, ID_TRACK_NUMBER, 2);
        put_uint(m, ID_TRACK_UID, 2);
        put_uint(m, ID_TRACK_TYPE, TRACK_TYPE_AUDIO);
        put_uint(m, ID_FLAG_LACING, 0);
        put_binary(m, ID_CODEC_ID, "A_FLAC", 6);
        // FLAC's own stream header: the marker, then STREAMINFO as the
        // last (and only) metadata block.
        uint8_t fp[FLAC_PRIVATE_LEN] = {'f', 'L', 'a', 'C', 0x80, 0, 0, MUX_STREAMINFO_LEN};
        memcpy(fp + 8, a->streaminfo, MUX_STREAMINFO_LEN);
        put_binary(m, ID_CODEC_PRIVATE, fp, sizeof(fp));
        size_t audio = begin_master(m, ID_AUDIO);
        put_float(m, ID_SAMPLING_FREQUENCY, (double)a->sample_rate);
        put_uint(m, ID_CHANNELS, (uint64_t)a->channels);
        put_uint(m, ID_BIT_DEPTH, (uint64_t)a->bits_per_sample);
        end_master(m, audio);
        end_master(m, at);
    }
    end_master(m, tracks);
}

// ============================================================================
// Static helpers: clusters and blocks
// ============================================================================

// Write the open cluster, its size now known, and flush it to the file.
static void close_cluster(mkv_mux_t *m) {
    if (!m->cluster_open)
        return;
    end_master(m, m->cluster_size_at);
    flush_buf(m);
    m->cluster_open = false;
}

// Open a cluster at `ms`, noting a cue when it opens on a video keyframe.
static void open_cluster(mkv_mux_t *m, uint64_t ms, bool video_key) {
    uint64_t pos = m->written + m->len - m->seg_data;
    // The buffer is empty here, so it will hold this cluster alone.
    m->cluster_size_at = begin_master(m, ID_CLUSTER);
    put_uint(m, ID_CLUSTER_TIMESTAMP, ms);
    m->cluster_open = true;
    m->cluster_ms = ms;
    if (video_key) {
        if (m->ncues == m->cues_cap) {
            size_t cap = m->cues_cap ? m->cues_cap * 2 : 64;
            mkv_cue_t *c = realloc(m->cues, cap * sizeof(*c));
            if (!c) {
                m->err = m->err ? m->err : -ENOMEM;
                return;
            }
            m->cues = c;
            m->cues_cap = cap;
        }
        m->cues[m->ncues++] = (mkv_cue_t){ms, pos};
    }
}

// Put one packet in the file as a SimpleBlock, opening and closing
// clusters as the rules say.
static void write_block(mkv_mux_t *m, int track, const mkv_pkt_t *p) {
    uint64_t ms = (p->pts_ns + MKV_TIMESCALE_NS / 2) / MKV_TIMESCALE_NS;
    bool video_key = track == MUX_TRACK_VIDEO && p->key;
    if (m->cluster_open) {
        int64_t rel = (int64_t)ms - (int64_t)m->cluster_ms;
        // A keyframe starts a cluster (one cue each); so do a second of
        // time, a full buffer, and a timestamp the block's 16 bits cannot reach.
        if (video_key || rel >= MKV_CLUSTER_MS || rel < 0 || m->len >= MKV_BUFFER)
            close_cluster(m);
    }
    if (!m->cluster_open)
        open_cluster(m, ms, video_key);
    int16_t rel = (int16_t)((int64_t)ms - (int64_t)m->cluster_ms);
    put_id(m, ID_SIMPLE_BLOCK);
    put_size_n(m, 4 + p->len, size_len(4 + p->len));
    uint8_t head[4] = {(uint8_t)(0x80 | (track + 1)), (uint8_t)((uint16_t)rel >> 8), (uint8_t)rel,
                       p->key ? 0x80 : 0x00};
    put_bytes(m, head, sizeof(head));
    put_bytes(m, p->data, p->len);
    if (p->end_ns > m->end_ns)
        m->end_ns = p->end_ns;
}

// Take the oldest packet off a track's queue.
static mkv_pkt_t *dequeue(mkv_mux_t *m, int track) {
    mkv_queue_t *q = &m->q[track];
    mkv_pkt_t *p = q->head;
    q->head = p->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    m->queued_bytes -= p->len;
    return p;
}

// Write out queued packets in time order: while every track has one
// waiting (all of them when `all`), or while too much is held.
static void drain(mkv_mux_t *m, bool all) {
    for (;;) {
        bool every = true, any = false;
        for (int t = 0; t < m->ntracks; t++) {
            every &= m->q[t].head != NULL;
            any |= m->q[t].head != NULL;
        }
        bool full = m->queued_bytes > MKV_QUEUE_BYTES || m->q[0].count + m->q[1].count > MKV_QUEUE_PACKETS;
        if (!any || !(every || all || full))
            return;
        // The earliest head; a tie goes to the lower track (video first).
        int pick = -1;
        for (int t = 0; t < m->ntracks; t++)
            if (m->q[t].head && (pick < 0 || m->q[t].head->pts_ns < m->q[pick].head->pts_ns))
                pick = t;
        mkv_pkt_t *p = dequeue(m, pick);
        write_block(m, pick, p);
        free(p);
    }
}

// The Cues: one CuePoint per keyframe cluster.
static void write_cues(mkv_mux_t *m) {
    if (!m->ncues)
        return;
    size_t at = begin_master(m, ID_CUES);
    for (size_t i = 0; i < m->ncues; i++) {
        size_t cp = begin_master(m, ID_CUE_POINT);
        put_uint(m, ID_CUE_TIME, m->cues[i].ms);
        size_t tp = begin_master(m, ID_CUE_TRACK_POS);
        put_uint(m, ID_CUE_TRACK, 1);
        put_uint(m, ID_CUE_CLUSTER_POS, m->cues[i].pos);
        end_master(m, tp);
        end_master(m, cp);
    }
    end_master(m, at);
}

// One Seek entry into the buffer: an element's ID and its position.
static void put_seek(mkv_mux_t *m, uint32_t id, uint64_t pos) {
    uint8_t idb[4];
    int n = id_len(id);
    for (int i = 0; i < n; i++)
        idb[i] = (uint8_t)(id >> (8 * (n - 1 - i)));
    int body = 2 + 1 + n + 2 + 1 + uint_len(pos);
    put_id(m, ID_SEEK);
    put_size_n(m, (uint64_t)body, 1);
    put_binary(m, ID_SEEK_ID, idb, (size_t)n);
    put_uint(m, ID_SEEK_POSITION, pos);
}

// The SeekHead, padded with a Void to exactly the reserved 64 bytes, into
// the (empty) buffer.
static void build_seekhead(mkv_mux_t *m, bool cues, uint64_t cues_pos) {
    // The entries alone first, to learn their size.
    put_seek(m, ID_INFO, m->info_pos);
    put_seek(m, ID_TRACKS, m->tracks_pos);
    if (cues)
        put_seek(m, ID_CUES, cues_pos);
    size_t body = m->len;
    uint8_t entries[MKV_SEEK_VOID];
    if (m->err || body > sizeof(entries)) {
        m->len = 0;
        return;
    }
    memcpy(entries, m->buf, body);
    m->len = 0;
    // A one-byte size field normally; two when one byte would leave a gap
    // of exactly one byte, which no Void can fill.
    int sl = MKV_SEEK_VOID - (4 + 1 + (int)body) == 1 ? 2 : 1;
    put_id(m, ID_SEEKHEAD);
    put_size_n(m, body, sl);
    put_bytes(m, entries, body);
    if (m->len < MKV_SEEK_VOID)
        put_void(m, MKV_SEEK_VOID - m->len);
}

// ============================================================================
// Operations (the muxer interface)
// ============================================================================

static int mkv_packet(muxer_t *base, int track, uint64_t pts_ns, uint64_t dur_ns, bool keyframe, const uint8_t *data,
                      size_t len) {
    mkv_mux_t *m = (mkv_mux_t *)base;
    if (track < 0 || track >= m->ntracks || (!data && len))
        return -EINVAL;
    if (m->err)
        return m->err;
    mkv_pkt_t *p = malloc(sizeof(*p) + len);
    if (!p)
        return -ENOMEM;
    p->next = NULL;
    p->pts_ns = pts_ns;
    p->end_ns = pts_ns + (dur_ns ? dur_ns : track == MUX_TRACK_VIDEO ? m->frame_period_ns : 0);
    p->key = keyframe;
    p->len = len;
    if (len)
        memcpy(p->data, data, len);
    mkv_queue_t *q = &m->q[track];
    if (q->tail)
        q->tail->next = p;
    else
        q->head = p;
    q->tail = p;
    q->count++;
    m->queued_bytes += len;
    drain(m, false);
    return m->err;
}

static uint64_t mkv_bytes(const muxer_t *base) {
    const mkv_mux_t *m = (const mkv_mux_t *)base;
    return m->written + m->len;
}

// Free the muxer's memory (the file is closed by the caller).
static void mkv_free(mkv_mux_t *m) {
    for (int t = 0; t < 2; t++)
        while (m->q[t].head)
            free(dequeue(m, t));
    free(m->cues);
    free(m->buf);
    free(m);
}

static int mkv_close(muxer_t *base) {
    mkv_mux_t *m = (mkv_mux_t *)base;
    drain(m, true);
    close_cluster(m);
    uint64_t cues_pos = m->written - m->seg_data;
    bool cues = m->ncues > 0;
    write_cues(m);
    flush_buf(m);
    // The Segment's size, now that it ends.
    uint8_t b[MKV_SIZE_LEN];
    poke_size(b, m->written - m->seg_data, MKV_SIZE_LEN);
    patch(m, m->seg_at, b, sizeof(b));
    // The SeekHead over its Void.
    build_seekhead(m, cues, cues_pos);
    if (m->len == MKV_SEEK_VOID)
        patch(m, m->seek_void_at, m->buf, m->len);
    m->len = 0;
    // The Duration over its Void, in ticks of the timestamp scale.
    put_float(m, ID_DURATION, (double)m->end_ns / MKV_TIMESCALE_NS);
    if (m->len == MKV_DURATION_LEN)
        patch(m, m->duration_at, m->buf, m->len);
    m->len = 0;
    int rc = m->err;
    if (fclose(m->f) != 0 && !rc)
        rc = errno ? -errno : -EIO;
    mkv_free(m);
    return rc;
}

static const muxer_ops_t k_mkv_ops = {mkv_packet, mkv_close, mkv_bytes};

// ============================================================================
// Lifecycle: constructor
// ============================================================================

muxer_t *mkv_mux_open(const char *path, const mux_video_t *video, const mux_audio_t *audio, int *err) {
    int e0 = 0;
    if (!err)
        err = &e0;
    *err = -EINVAL;
    if (!path || !video || video->width <= 0 || video->height <= 0 || video->frame_period_ns == 0 ||
        (video->bits_per_pixel != 8 && video->bits_per_pixel != 16 && video->bits_per_pixel != 32))
        return NULL;
    if (audio && (audio->sample_rate == 0 || audio->channels < 1 || audio->channels > 8))
        return NULL;
    mkv_mux_t *m = calloc(1, sizeof(*m));
    if (!m) {
        *err = -ENOMEM;
        return NULL;
    }
    m->base.ops = &k_mkv_ops;
    m->ntracks = audio ? 2 : 1;
    m->frame_period_ns = video->frame_period_ns;
    m->f = fopen(path, "wb");
    if (!m->f) {
        *err = errno ? -errno : -EIO;
        mkv_free(m);
        return NULL;
    }
    // Our own buffer does the batching; stdio's would only copy again.
    setvbuf(m->f, NULL, _IONBF, 0);
    write_header(m, video, audio);
    // The header reaches the file at once, so even an empty recording is
    // a readable file.  Its offsets so far are file offsets too.
    flush_buf(m);
    if (m->err) {
        *err = m->err;
        fclose(m->f);
        mkv_free(m);
        return NULL;
    }
    *err = 0;
    return &m->base;
}
