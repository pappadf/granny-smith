// Parser for log lines emitted by src/core/debug/log.c::log_vemit.
//
// Canonical shape: `[<cat>] <level> [optional @<instr_count>] [optional
// PC=<8 hex>] <msg>\n`. The optional `@…` / `PC=…` prefixes are toggled
// per-category by `log <cat> ts=on|off pc=on|off`; we preserve them as
// part of `msg` so the UI shows what the user configured.
//
// Lines that don't match the prefix (plain shell stdout, multi-line
// dumps) return null — callers route those to the terminal pane only.

import type { LogEntry } from '@/state/logs.svelte';

const RE = /^\[([^\]]+)\]\s+(\d+)\s(.*)$/;

export function parseLogLine(line: string): LogEntry | null {
  if (!line) return null;
  const trimmed = line.replace(/\r?\n$/, '');
  const m = RE.exec(trimmed);
  if (!m) return null;
  const level = parseInt(m[2], 10);
  if (!Number.isFinite(level) || level < 0) return null;
  return { ts: Date.now(), cat: m[1], level, msg: m[3] };
}
