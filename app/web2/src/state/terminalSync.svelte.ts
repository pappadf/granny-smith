// What the console tells the command browser while the user types: the
// line, the cursor and `shell.complete(line, cursor, true)` for it.  `seq`
// increases with every update; `fromBrowser` marks an update caused by the
// browser's own write to the input (the browser then keeps its selection).
import type { CompletionResult } from '@/bus/emulator';

export const terminalSync: {
  seq: number;
  line: string;
  cursor: number;
  result: CompletionResult | null;
  fromBrowser: boolean;
} = $state({ seq: 0, line: '', cursor: 0, result: null, fromBrowser: false });

export function publishCompletion(
  line: string,
  cursor: number,
  result: CompletionResult | null,
  fromBrowser: boolean,
): void {
  terminalSync.line = line;
  terminalSync.cursor = cursor;
  terminalSync.result = result;
  terminalSync.fromBrowser = fromBrowser;
  terminalSync.seq++;
}
