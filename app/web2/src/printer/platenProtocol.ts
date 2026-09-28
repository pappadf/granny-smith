// The LaserWriter interpreter ring's wire protocol — the TypeScript mirror
// of src/core/network/laserwriter_ring_protocol.h.  The printer bridge on
// the emulator's pthread writes OPEN / FEED / FINISH / ABANDON records into
// the outbound ring; the platen worker (platen.worker.ts, the loop in
// platenRing.ts) consumes them and answers OPENED / OPEN_FAILED / FED /
// FINISHED through the inbound ring.  Keep the two files in step: every
// name and value here is the header's, and PROTOCOL_VERSION is checked at
// attach — bump it in both when a layout or a record changes.
//
// Framing (the header's rule): a record is {u32 kind, u32 len} followed
// by its payload; `len` counts the header and is a multiple of 8 (pad8 of
// header + payload; text fields inside a payload are padded to 4 with
// pad4).  A record never wraps: when one would cross the ring's end the
// writer emits a PAD record whose len reaches the end and the next record
// starts at 0.  Every record therefore starts at a multiple of 8, so the
// remainder before the end is never less than a PAD's 8-byte header.  A
// reader rejects a len that is not a multiple of 8.  HEAD / TAIL are
// monotonic byte counts mod 2^32.  All words are little-endian.

export const PROTOCOL_VERSION = 2;
export const MAGIC = 0x4c575250; // 'LWRP'

// Control-block word indices (Int32Array view at the control base).
export const C_MAGIC = 0;
export const C_VERSION = 1;
export const C_OUT_OFF = 2;
export const C_OUT_SIZE = 3;
export const C_IN_OFF = 4;
export const C_IN_SIZE = 5;
export const C_OUT_HEAD = 6; // core: outbound bytes published (the worker waits on it)
export const C_OUT_TAIL = 7; // worker: outbound bytes consumed
export const C_IN_HEAD = 8; // worker: inbound bytes published
export const C_IN_TAIL = 9; // core: inbound bytes consumed (notified for a worker waiting for room)
export const C_STATUS = 10; // worker: STATUS_*
export const C_STAT_JOBS = 11; // worker: jobs opened
export const C_STAT_FEEDS = 12; // worker: feeds executed
export const CTRL_WORDS = 32;

export const STATUS_DETACHED = 0;
export const STATUS_ATTACHED = 1;
export const STATUS_LOST = 2;

// Bytes in one FEED: one PAP flow quantum (8 x 512).
export const FEED_MAX = 8 * 512;
// Cap on each text field of an inbound record; a longer channel is cut
// and FED_F_TRUNCATED set.
export const TEXT_MAX = 64 << 10;
// Cap on the FINISH title.
export const TITLE_MAX = 63;
// The record header: {kind, len}.

// Records, core -> worker.

export const R_OPEN = 1;
export const OPEN_JOB = 0;
export const OPEN_COMPRESS = 1;
export const OPEN_EMBED = 2;
export const OPEN_BUDGET_L = 3;
export const OPEN_BUDGET_H = 4;
export const OPEN_PASSWORD = 5;
export const OPEN_ID_COUNT = 6;
export const OPEN_ID_BYTES = 7;
export const OPEN_PRELUDE = 8;
export const OPEN_WORDS = 9;

export const R_FEED = 2;
export const FEED_JOB = 0;
export const FEED_SEQ = 1;
export const FEED_LEN = 2;
export const FEED_WORDS = 3;

export const R_FINISH = 3;
export const FINISH_JOB = 0;
export const FINISH_TITLE_LEN = 1;
export const FINISH_WORDS = 2;

export const R_ABANDON = 4;
export const ABANDON_JOB = 0;
export const ABANDON_WORDS = 1;

// Records, worker -> core.
export const R_OPENED = 16;
export const OPENED_JOB = 0;
export const OPENED_WORDS = 1;

export const R_OPEN_FAILED = 17;
export const OPEN_FAILED_JOB = 0;
export const OPEN_FAILED_TEXT = 1;
export const OPEN_FAILED_WORDS = 2;

export const R_FED = 18;
export const FED_JOB = 0;
export const FED_SEQ = 1;
export const FED_STATUS = 2;
export const FED_PAGES = 3;
export const FED_REPLY_LEN = 4;
export const FED_ERROR_LEN = 5;
export const FED_FLAGS = 6;
export const FED_WORDS = 7;

export const FEED_WAITING = 0;
export const FEED_ENDED = 1;
export const FEED_FAILED = 2;

export const FED_F_TRUNCATED = 1 << 0;

export const R_FINISHED = 19;
export const FINISHED_JOB = 0;
export const FINISHED_OUTCOME = 1;
export const FINISHED_PAGES = 2;
export const FINISHED_ERRNAME_LEN = 3;
export const FINISHED_OFFEND_LEN = 4;
export const FINISHED_REPLY_LEN = 5;
export const FINISHED_ERROR_LEN = 6;
export const FINISHED_FLAGS = 7;
export const FINISHED_WORDS = 8;

export const OUTCOME_OK = 0;
export const OUTCOME_ERROR = 1;
export const OUTCOME_BUDGET = 2;
export const OUTCOME_FAILED = 3;

// --- helpers ---------------------------------------------------------------
//
// The ring itself -- records, PADs, the 8-byte rule, the little-endian
// header words -- is the record-ring primitive every transport on the page
// shares (bus/mailboxRing.ts, mirroring src/core/mailbox/mailbox_ring.h).
// Re-exported here so the platen worker and its tests keep one import.

export {
  HDR_BYTES,
  R_PAD,
  pad4,
  pad8,
  getU32,
  putU32,
  recordBytes,
  ringWrite,
  ringRead,
  type Ring,
  type RecordHeader,
} from '@/bus/mailboxRing';
