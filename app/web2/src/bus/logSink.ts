// Router between the WASM Module's stdout/log callbacks and the new-UI
// consumers (Terminal pane + Logs view).
//
// Two inputs:
//   - Module.print / Module.printErr — every line the emulator writes
//     to stdout / stderr. Routes to the terminal sink unconditionally;
//     if the line happens to look like a structured log emission we
//     also append to logs as a defensive double-source (covers the
//     window before Module.onLogEmit is installed).
//   - Module.onLogEmit — installed by src/platform/wasm/em_main.c via
//     log_set_sink. Every formatted log line lands here regardless of
//     per-category stdout=on/off. Preferred path.

import { appendLog } from '@/state/logs.svelte';
import { parseLogLine } from '@/lib/logParse';

let terminalWrite: ((line: string) => void) | null = null;

// TerminalPane registers itself on mount; null on unmount.
export function setTerminalSink(fn: ((line: string) => void) | null): void {
  terminalWrite = fn;
}

export function routePrintLine(line: string): void {
  if (terminalWrite) terminalWrite(line);
  // Logs are populated exclusively from routeLogEmit (the C-side global
  // sink, installed at boot). We deliberately do NOT also parse from
  // print here — log.c writes the same formatted line to both
  // (`to_stdout` sink and the global sink), so parsing both would
  // double-count every entry.
}

export function routeLogEmit(line: string): void {
  const parsed = parseLogLine(line);
  if (parsed) appendLog(parsed);
}
