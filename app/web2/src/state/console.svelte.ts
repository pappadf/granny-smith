// The Terminal console's state, outliving the component: the entry list
// (lib/consoleModel fed by bus/logSink's records), the queue of submitted
// input, and the job it runs.  ConsoleView.svelte renders it; SYSTEM echoes
// into it (consoleEcho).
//
// Submitting queues the text; the queue runs one script job at a time
// (gsEvalLine), so what is typed while a command runs is run after it, in
// order.  The `command` entry is added when its job starts, so each
// command's output follows it.

import { ConsoleModel, type ConsoleEntry } from '@/lib/consoleModel';
import { setConsoleSink } from '@/bus/logSink';
import {
  gsEvalLine,
  getRuntimePrompt,
  isModuleReady,
  whenModuleReady,
  shellInterrupt,
} from '@/bus/emulator';

interface ConsoleState {
  entries: readonly ConsoleEntry[];
  prompt: string;
  // When the running job started (ms, performance.now()), or null.
  runningSince: number | null;
  // Submissions waiting behind the running one.
  queued: number;
}

export const consoleState: ConsoleState = $state({
  entries: [],
  prompt: '',
  runningSince: null,
  queued: 0,
});

const frame = (fn: () => void): void => {
  if (typeof requestAnimationFrame === 'function') requestAnimationFrame(() => fn());
  else setTimeout(fn, 16);
};

let model: ConsoleModel | null = null;

// The model, created (and fed) on first use.
export function consoleModel(): ConsoleModel {
  if (!model) {
    model = new ConsoleModel({
      schedule: frame,
      setTimer: (fn, ms) => {
        const t = setTimeout(fn, ms);
        return () => clearTimeout(t);
      },
      onFlush: (entries) => {
        consoleState.entries = entries;
      },
    });
    const m = model;
    setConsoleSink((r) => m.push(r));
  }
  return model;
}

// For tests: drop the model and everything queued.
export function resetConsole(): void {
  setConsoleSink(null);
  model = null;
  queue.length = 0;
  consoleState.entries = [];
  consoleState.prompt = '';
  consoleState.runningSince = null;
  consoleState.queued = 0;
}

export function refreshPrompt(): void {
  consoleState.prompt = getRuntimePrompt() ?? '';
}

// A statement another surface ran on the user's behalf (dimmed).
export function consoleEcho(text: string): void {
  consoleModel().echo(text);
}

export function consoleClear(): void {
  consoleModel().clear();
}

// A line of the console's own (a notice, not the core's output).
export function consoleNotice(text: string): void {
  consoleModel().push({ kind: 'print', line: text });
}

const queue: string[] = [];
let pumping = false;

// Called after every console job (SYSTEM re-reads what it shows).
let jobDone: Array<() => void> = [];
export function onConsoleJobDone(cb: () => void): () => void {
  jobDone.push(cb);
  return () => {
    jobDone = jobDone.filter((f) => f !== cb);
  };
}

export function consoleSubmit(text: string): void {
  if (!text.trim()) return;
  queue.push(text);
  consoleState.queued = queue.length;
  void pump();
}

async function pump(): Promise<void> {
  if (pumping) return;
  pumping = true;
  try {
    while (queue.length) {
      if (!isModuleReady()) await whenModuleReady();
      const text = queue.shift()!;
      consoleState.queued = queue.length;
      const m = consoleModel();
      m.command(text, consoleState.prompt);
      // `clear` is the console's own: no round trip.
      if (text.trim() === 'clear') {
        m.clear();
        continue;
      }
      consoleState.runningSince = performance.now();
      try {
        await gsEvalLine(text);
      } catch (err) {
        m.push({
          kind: 'stderr',
          line: `Command failed: ${err instanceof Error ? err.message : String(err)}`,
        });
      }
      consoleState.runningSince = null;
      refreshPrompt();
      for (const cb of jobDone) cb();
    }
  } finally {
    pumping = false;
  }
}

// Ctrl+C without a selection: drop the type-ahead, then cancel the running
// job, else stop a run this console started; otherwise say so.
export async function consoleInterrupt(): Promise<void> {
  queue.length = 0;
  consoleState.queued = 0;
  if (!isModuleReady()) return;
  try {
    const did = await shellInterrupt();
    if (did === 'nothing') consoleNotice('^C  (nothing to interrupt; Pause stops the machine)');
  } catch (err) {
    consoleNotice(`Interrupt failed: ${err instanceof Error ? err.message : String(err)}`);
  }
}
