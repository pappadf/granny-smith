// Router between the WASM Module's output and the new-UI consumers (the
// Terminal's console + the Logs view).
//
// Inputs:
//   - Module.print / Module.printErr -- lines the emulator writes to stdout /
//     stderr outside a job's capture.
//   - job records from the event ring -- a job's output pieces and its
//     annotation records (value_begin / value / error), plus the start and
//     end of the console's own job.
//   - log events -- emitted by src/platform/wasm/em_main.c's log sink via
//     log_set_sink.  Every formatted log line lands here regardless of
//     per-category stdout=on/off; they feed the Logs view only.
//
// The console consumes the first two as ConsoleRecords (lib/consoleModel).

import { appendLog } from '@/state/logs.svelte';
import { parseLogLine } from '@/lib/logParse';
import type { ConsoleRecord } from '@/lib/consoleModel';

let consoleSink: ((r: ConsoleRecord) => void) | null = null;

// Records sent while no console exists (before the Terminal tab is first
// opened), replayed when one registers -- boot output and a script's
// results used to be dropped.  Bounded: the oldest go first.
const BACKLOG_MAX = 2000;
let backlog: ConsoleRecord[] = [];

// The console model registers itself when it is created (state/console);
// null drops it (resetConsole).
export function setConsoleSink(fn: ((r: ConsoleRecord) => void) | null): void {
  consoleSink = fn;
  if (fn && backlog.length) {
    const records = backlog;
    backlog = [];
    for (const r of records) fn(r);
  }
}

export function routeConsole(r: ConsoleRecord): void {
  if (consoleSink) consoleSink(r);
  else {
    backlog.push(r);
    if (backlog.length > BACKLOG_MAX) backlog.splice(0, backlog.length - BACKLOG_MAX);
  }
}

// Module.print: a stdout line outside any job.
export function routePrintLine(line: string): void {
  // Logs are populated exclusively from routeLogEmit (the C-side global
  // sink).  log.c writes the same formatted line to both, so parsing it
  // here too would double-count every entry.
  routeConsole({ kind: 'print', line });
}

// Module.printErr: a stderr line.
export function routeErrorLine(line: string): void {
  routeConsole({ kind: 'stderr', line });
}

export function routeLogEmit(line: string): void {
  const parsed = parseLogLine(line);
  if (parsed) appendLog(parsed);
}
