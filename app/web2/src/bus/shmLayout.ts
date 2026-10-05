// The shared-heap layouts of the browser's three media transports -- the
// camera frame slots, the microphone ring and the audio-out ring -- mirrored
// from src/platform/wasm/em_shm_layout.h (NAME here is GS_NAME there;
// tests/unit/shmLayout.test.ts compares the two).  Each block starts with a
// magic and a version; a side that finds one it does not know refuses the
// transport, visibly, rather than write through a layout it has guessed.

// === Camera: a double-buffered frame slot pair ===
export const CAM_MAGIC = 0x4d414347; // 'GCAM'
export const CAM_VERSION = 1;

// Int32 word indices from the block's address.
export const CAM_W_MAGIC = 0; // GS_CAM_MAGIC
export const CAM_W_VERSION = 1; // GS_CAM_VERSION
export const CAM_W_SLOT_OFF = 2; // byte offset of slot 0 from the block
export const CAM_W_SLOT_BYTES = 3; // bytes per slot (width * height * 4)
export const CAM_W_WIDTH = 4;
export const CAM_W_HEIGHT = 5;
export const CAM_W_CONNECTED = 6; // main thread: a track is attached and delivering
export const CAM_W_ACTIVE = 7; // slot holding the newest complete frame (-1 none)
export const CAM_W_SEQ = 8; // bumped after every completed frame (the reader's seqlock)

// === Microphone: an SPSC ring of mono int16 ===
export const MIC_MAGIC = 0x43494d47; // 'GMIC'
export const MIC_VERSION = 1;
export const MIC_W_MAGIC = 0; // GS_MIC_MAGIC
export const MIC_W_VERSION = 1; // GS_MIC_VERSION
export const MIC_W_RING_OFF = 2; // byte offset of ring[0] from the block
export const MIC_W_RING_LEN = 3; // samples, a power of two
export const MIC_W_LABEL_OFF = 4; // byte offset of the capture device's name
export const MIC_W_LABEL_LEN = 5; // bytes, NUL included
export const MIC_W_CONNECTED = 6; // main thread: a track is attached and live
export const MIC_W_WR = 7; // producer (main thread) index, free-running
export const MIC_W_RD = 8; // consumer (worker) index, free-running: its only writer
export const MIC_W_RATE = 9; // the rate the producer is delivering
export const MIC_W_UNDERRUNS = 10;
export const MIC_W_OVERRUNS = 11;
export const MIC_W_JS_RMS = 12; // level of the raw worklet blocks, int16 counts
export const MIC_W_JS_PEAK = 13;
export const MIC_W_RESET_REQ = 14; // producer: bump to ask the consumer to drop the backlog
export const MIC_W_RESET_ACK = 15; // consumer: the last request honoured (rd = wr)

// === Audio out: an SPSC ring of interleaved int16 frames ===
export const ARING_MAGIC = 0x4e495241; // 'ARIN'
export const ARING_VERSION = 1;
export const ARING_W_MAGIC = 0; // GS_ARING_MAGIC
export const ARING_W_VERSION = 1; // GS_ARING_VERSION
export const ARING_W_DATA_OFF = 2; // byte offset of the frame data from the block
export const ARING_W_FRAMES = 3; // ring capacity in frames, a power of two
export const ARING_W_WRITE = 4; // producer (emulator) index, free-running: its only writer
export const ARING_W_READ = 5; // consumer (worklet) index, free-running: its only writer
export const ARING_W_VOL = 6; // 0..7, the latest producer volume
export const ARING_W_SILENT = 7; // consecutive all-equal pushes (silence-aware depth trim)
export const ARING_W_FILL_PM = 8; // worklet -> producer: fill against target depth, per mille
export const ARING_W_PUSH_SEQ = 9; // bumped per push: the worklet's quiet-stream detector
export const ARING_W_RESET_GEN = 10; // producer: bump to ask the consumer to drop the backlog
export const ARING_W_OWNER = 11; // page: the id of the one worklet allowed to consume
export const ARING_HDR_BYTES = 64;

// Whether the block at `ptr` has the magic and version this build speaks.
export function shmBlockOk(i32: Int32Array, ptr: number, magic: number, version: number): boolean {
  const w = ptr >> 2;
  return Atomics.load(i32, w) >>> 0 === magic && Atomics.load(i32, w + 1) === version;
}
