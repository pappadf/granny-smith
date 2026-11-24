// What the Logs panel shows of the log buffer (state/logs): which entries
// pass the view's filters, and how a logpoint's line splits into the
// message the user wrote and the where-and-what the core prefixed to it.
// Pure functions, unit-tested.

import type { LogEntry } from '@/state/logs.svelte';

// The view's filters: entries above `maxLevel` (null: no limit) and of a
// hidden category are not shown.  They stay in the buffer either way.
export interface LogFilter {
  maxLevel: number | null;
  hidden: Record<string, boolean>;
}

// The entries a filter lets through, in order (the same array when it lets
// everything through).
export function visibleEntries(entries: readonly LogEntry[], f: LogFilter): readonly LogEntry[] {
  const anyHidden = Object.values(f.hidden).some(Boolean);
  if (f.maxLevel === null && !anyHidden) return entries;
  return entries.filter(
    (e) =>
      (f.maxLevel === null || e.level <= f.maxLevel) && !(anyHidden && f.hidden[e.cat] === true),
  );
}

// A logpoint's line, split: `where` is what the core prefixes (the PC, or
// the access and its value), `message` the text the logpoint was given (or
// the core's own words when it was given none).
export interface LogpointParts {
  where: string;
  message: string;
}

// The core's logpoint lines (debug.c): "logpoint $PC: msg",
// "logpoint WRITE $ADDR.w value=$V: msg", and the message-less forms
// "logpoint hit at $PC (hit count: N)", "logpoint READ $ADDR.b value=$V pc=$PC".
const PC_LINE = /^logpoint (\$[0-9A-Fa-f]{8}): (.*)$/s;
const MEM_LINE = /^logpoint (READ|WRITE) (\$[0-9A-Fa-f]{8}\.[bwl]) value=(\$[0-9A-Fa-f]+): (.*)$/s;
const PC_BARE = /^logpoint hit at (\$[0-9A-Fa-f]{8}) \((hit count: \d+)\)$/;
const MEM_BARE =
  /^logpoint (READ|WRITE) (\$[0-9A-Fa-f]{8}\.[bwl]) value=(\$[0-9A-Fa-f]+) pc=(\$[0-9A-Fa-f]{8})$/;

export function splitLogpoint(msg: string): LogpointParts | null {
  let m = PC_LINE.exec(msg);
  if (m) return { where: m[1], message: m[2] };
  m = MEM_LINE.exec(msg);
  if (m) return { where: `${m[1]} ${m[2]} = ${m[3]}`, message: m[4] };
  m = PC_BARE.exec(msg);
  if (m) return { where: m[1], message: `hit (${m[2]})` };
  m = MEM_BARE.exec(msg);
  if (m) return { where: `pc ${m[4]}`, message: `${m[1]} ${m[2]} = ${m[3]}` };
  return null;
}

// An entry as one line of text, as the buffer holds it (for Copy and
// Download).
export function entryText(e: LogEntry): string {
  return `[${e.cat}] ${e.level} ${e.msg}`;
}
