// Parses a line of `debug.disasm` output into a structured row. The
// shell prints lines in the form:
//   00400C  MOVE.L (A0)+,D1        ; comment text
// We parse the leading address, the mnemonic (first word after the
// address), the operand block (everything up to a trailing comment), and
// any `; comment` tail. Unit-tested.

export interface DisasmRow {
  addr: number;
  mnem: string;
  ops: string;
  cmt: string;
}

// Permissive — accept any number of leading-spaces, address optionally
// prefixed with `$`, mnemonic word with optional `.size` suffix, and
// the rest is treated as operands.
const LINE_RE = /^\s*\$?([0-9a-fA-F]+)\s+([A-Z][A-Z0-9.]*)\s*([^;]*?)\s*(?:;\s*(.*))?\s*$/;

export function parseDisasmLine(line: string): DisasmRow | null {
  if (!line) return null;
  const m = LINE_RE.exec(line);
  if (!m) return null;
  const addr = parseInt(m[1], 16);
  if (!Number.isFinite(addr)) return null;
  return {
    addr: addr >>> 0,
    mnem: m[2],
    ops: (m[3] ?? '').trim(),
    cmt: (m[4] ?? '').trim(),
  };
}

export function parseDisasmBlock(text: string): DisasmRow[] {
  if (!text) return [];
  const out: DisasmRow[] = [];
  for (const line of text.split(/\r?\n/)) {
    const row = parseDisasmLine(line);
    if (row) out.push(row);
  }
  return out;
}
