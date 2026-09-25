// The Debug view's one debug.frame (11-WORK-ORDER D4, F-45).
//
// Registers, FPU, Disassembly and Call Stack each used to fetch their own
// frame — three or more serialised bridge round-trips per pause, each pane
// twice on mount and again on the →running edge, the FPU pane even on
// machines with no FPU.  Now the Debug view fetches once per pause (or step)
// and every pane reads this slice.  Concurrent refreshes coalesce: a
// refresh asked for while one is in flight runs once more after it.

import { loadDebugFrame, type DebugFrame } from '@/bus/debug';

// Rows in the disassembly window, and how many of them precede the PC.
export const DISASM_ROWS = 32;
export const ROWS_BEFORE_PC = 4;

interface DebugFrameState {
  current: DebugFrame | null;
  loading: boolean;
}

export const debugFrame: DebugFrameState = $state({ current: null, loading: false });

let inFlight: Promise<void> | null = null;
let again = false;

// Fetch the frame; coalesce with a fetch already in flight.
export function refreshDebugFrame(): Promise<void> {
  if (inFlight) {
    again = true;
    return inFlight;
  }
  inFlight = (async () => {
    debugFrame.loading = true;
    try {
      do {
        again = false;
        debugFrame.current = await loadDebugFrame(undefined, DISASM_ROWS, ROWS_BEFORE_PC);
      } while (again);
    } finally {
      debugFrame.loading = false;
      inFlight = null;
    }
  })();
  return inFlight;
}

// Forget the frame (no machine, or it stopped).
export function clearDebugFrame(): void {
  debugFrame.current = null;
}
