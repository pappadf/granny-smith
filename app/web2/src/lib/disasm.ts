// Parses a line of `debug.disasm` output into a structured row. The
// C-side `debugger_disasm` writes lines through
//   snprintf("%s  %04x  %-10s%-12s", addr_str, word, mnem, operands)
// so the concrete shapes are
//   $00400C  4e75  RTS        D1,(A0)+
//   L:$00400C P:$00400C  4e75  RTS        D1,(A0)+
// (the `L:$… P:$…` pair is emitted by addr_format when the MMU is
// active.) We capture the *logical* address, skip the hex instruction
// word, then pick up the mnemonic and operands.

export interface DisasmRow {
  addr: number;
  mnem: string;
  ops: string;
  cmt: string;
}

const LINE_RE =
  /^\s*(?:L:)?\$?([0-9a-fA-F]+)(?:\s+P:\$?[0-9a-fA-F?]+)?\s+(?:[0-9a-fA-F]+\s+)?([A-Z][A-Z0-9.]*)\s*([^;]*?)\s*(?:;\s*(.*))?\s*$/;

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
