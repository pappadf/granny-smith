// What the console tells the command browser while the user types: the
// line, the cursor and `shell.complete(line, cursor, true)` for it.  `seq`
// increases with every update.
import type { CompletionResult } from '@/bus/emulator';

export const terminalSync: {
  seq: number;
  line: string;
  cursor: number;
  result: CompletionResult | null;
} = $state({ seq: 0, line: '', cursor: 0, result: null });

export function publishCompletion(
  line: string,
  cursor: number,
  result: CompletionResult | null,
): void {
  terminalSync.line = line;
  terminalSync.cursor = cursor;
  terminalSync.result = result;
  terminalSync.seq++;
}
