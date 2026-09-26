// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// em_shm_layout.h
// The shared-heap layouts of the browser's three media transports (T1): the
// camera frame slots (em_camera.c), the microphone ring (em_audio_in.c) and
// the audio-out ring (em_audio.c).
//
// Each block starts with a magic and a version, then the (offset, size)
// facts and the word indices the JS side needs.  C writes the block before
// it announces the address; JS reads every offset from it and refuses,
// visibly, a block whose magic or version it does not know.  The Voodoo2 and
// printer transports already worked this way (voodoo2_gpu_protocol.h,
// laserwriter_ring_protocol.h); these three hard-coded their layouts on both
// sides with nothing to catch a drift.
//
// Mirrored in app/web2/src/bus/shmLayout.ts, NAME for GS_NAME;
// tests/unit/shmLayout.test.ts compares every #define here with its export.

#ifndef EM_SHM_LAYOUT_H
#define EM_SHM_LAYOUT_H

// === Camera: a double-buffered frame slot pair ==============================
#define GS_CAM_MAGIC   0x4D414347u // 'GCAM'
#define GS_CAM_VERSION 1u

// Int32 word indices from the block's address.
#define GS_CAM_W_MAGIC      0 // GS_CAM_MAGIC
#define GS_CAM_W_VERSION    1 // GS_CAM_VERSION
#define GS_CAM_W_SLOT_OFF   2 // byte offset of slot 0 from the block
#define GS_CAM_W_SLOT_BYTES 3 // bytes per slot (width * height * 4)
#define GS_CAM_W_WIDTH      4
#define GS_CAM_W_HEIGHT     5
#define GS_CAM_W_CONNECTED  6 // main thread: a track is attached and delivering
#define GS_CAM_W_ACTIVE     7 // slot holding the newest complete frame (-1 none)
#define GS_CAM_W_SEQ        8 // bumped after every completed frame (the reader's seqlock)

// === Microphone: an SPSC ring of mono int16 =================================
#define GS_MIC_MAGIC   0x43494D47u // 'GMIC'
#define GS_MIC_VERSION 1u

#define GS_MIC_W_MAGIC     0 // GS_MIC_MAGIC
#define GS_MIC_W_VERSION   1 // GS_MIC_VERSION
#define GS_MIC_W_RING_OFF  2 // byte offset of ring[0] from the block
#define GS_MIC_W_RING_LEN  3 // samples, a power of two
#define GS_MIC_W_LABEL_OFF 4 // byte offset of the capture device's name
#define GS_MIC_W_LABEL_LEN 5 // bytes, NUL included
#define GS_MIC_W_CONNECTED 6 // main thread: a track is attached and live
#define GS_MIC_W_WR        7 // producer (main thread) index, free-running
#define GS_MIC_W_RD        8 // consumer (worker) index, free-running: its only writer
#define GS_MIC_W_RATE      9 // the rate the producer is delivering
#define GS_MIC_W_UNDERRUNS 10
#define GS_MIC_W_OVERRUNS  11
#define GS_MIC_W_JS_RMS    12 // level of the raw worklet blocks, int16 counts
#define GS_MIC_W_JS_PEAK   13
#define GS_MIC_W_RESET_REQ 14 // producer: bump to ask the consumer to drop the backlog
#define GS_MIC_W_RESET_ACK 15 // consumer: the last request honoured (rd = wr)

// === Audio out: an SPSC ring of interleaved int16 frames ====================
#define GS_ARING_MAGIC   0x4E495241u // 'ARIN'
#define GS_ARING_VERSION 1u

#define GS_ARING_W_MAGIC     0 // GS_ARING_MAGIC
#define GS_ARING_W_VERSION   1 // GS_ARING_VERSION
#define GS_ARING_W_DATA_OFF  2 // byte offset of the frame data from the block
#define GS_ARING_W_FRAMES    3 // ring capacity in frames, a power of two
#define GS_ARING_W_WRITE     4 // producer (emulator) index, free-running: its only writer
#define GS_ARING_W_READ      5 // consumer (worklet) index, free-running: its only writer
#define GS_ARING_W_VOL       6 // 0..7, the latest producer volume
#define GS_ARING_W_SILENT    7 // consecutive all-equal pushes (silence-aware depth trim)
#define GS_ARING_W_FILL_PM   8 // worklet -> producer: fill against target depth, per mille
#define GS_ARING_W_PUSH_SEQ  9 // bumped per push: the worklet's quiet-stream detector
#define GS_ARING_W_RESET_GEN 10 // producer: bump to ask the consumer to drop the backlog
#define GS_ARING_W_OWNER     11 // page: the id of the one worklet allowed to consume
#define GS_ARING_HDR_BYTES   64

#endif // EM_SHM_LAYOUT_H
