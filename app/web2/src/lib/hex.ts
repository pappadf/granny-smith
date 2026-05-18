// Hex parsing + formatting helpers used by the Debug view. Keeps the
// shell's conventions: leading `$` or `0x` is tolerated on read; output
// is always uppercase, zero-padded to the register's width.

export function fmtHex32(n: number): string {
  return (n >>> 0).toString(16).padStart(8, '0').toUpperCase();
}

export function fmtHex16(n: number): string {
  return (n & 0xffff).toString(16).padStart(4, '0').toUpperCase();
}

export function fmtHex8(n: number): string {
  return (n & 0xff).toString(16).padStart(2, '0').toUpperCase();
}

// Parse a hex string. Accepts `0x…`, `$…`, or bare hex. Returns null on
// failure. Negative values are rejected — registers don't take signed
// input from the user.
export function parseHex(s: string): number | null {
  if (!s) return null;
  let t = s.trim();
  if (t.startsWith('$')) t = t.slice(1);
  else if (t.toLowerCase().startsWith('0x')) t = t.slice(2);
  if (t.length === 0 || !/^[0-9a-fA-F]+$/.test(t)) return null;
  const n = parseInt(t, 16);
  if (!Number.isFinite(n) || n < 0) return null;
  return n >>> 0; // unsigned 32-bit
}
