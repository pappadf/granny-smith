// The producer half of the microphone ring's index arithmetic
// (src/platform/wasm/em_mic_ring.h is the consumer's). The indices are
// free-running uint32 stored in Int32 words, and the ring length is a power
// of two, so a slot is the same function of the same integer on both sides:
// `(index >>> 0) & (len - 1)` here, `index & (len - 1)` in C -- across the
// 2^31 sign flip and the 2^32 wrap alike. (It was `(wr + i) % ringLen`: once
// the Int32 went negative, JS's `%` was negative too, and the producer wrote
// up to ~48 KB before the ring.)
export function micRingSlot(wr: number, i: number, len: number): number {
  return ((wr + i) >>> 0) & (len - 1);
}
