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
// The console consumes the first two as ConsoleRecords (lib/consoleModel):
// the app's console model exists from the start (state/console), so they
// go straight into it, whether or not the Terminal has been opened.

import { appendLog } from '@/state/logs.svelte';
import { appConsole } from '@/state/console.svelte';
import { parseLogLine } from '@/lib/logParse';
import type { ConsoleRecord } from '@/lib/consoleModel';

export function routeConsole(r: ConsoleRecord): void {
  appConsole.model.push(r);
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
