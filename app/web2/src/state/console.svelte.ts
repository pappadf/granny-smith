// The Terminal console's state, outliving the component: the entry list
// (lib/consoleModel fed by bus/logSink's records), the queue of submitted
// input, and the job it runs.  ConsoleView.svelte renders it; SYSTEM echoes
// into it (consoleEcho).
//
// Submitting queues the text; the queue runs one script job at a time
// (gsEvalLine), so what is typed while a command runs is run after it, in
// order.  The `command` entry is added when its job starts, so each
// command's output follows it.
//
// The app's console is created when this module loads, so no record the
// core sends before the Terminal first opens is lost.  Tests build their
// own with createConsole().

import { ConsoleModel, type ConsoleEntry } from '@/lib/consoleModel';
import type { HlSpan } from '@/lib/highlight';
import {
  gsEvalLine,
  getRuntimePrompt,
  isModuleReady,
  whenModuleReady,
  shellInterrupt,
} from '@/bus/emulator';

export interface ConsoleState {
  entries: readonly ConsoleEntry[];
  prompt: string;
  // When the running job started (ms, performance.now()), or null.
  runningSince: number | null;
  // Submissions waiting behind the running one.
  queued: number;
}

export interface Console {
  readonly state: ConsoleState;
  readonly model: ConsoleModel;
  // Queue `text`; `spans` are the input's syntax colours for it, kept on
  // its command entry.
  submit(text: string, spans?: readonly HlSpan[]): void;
  // Ctrl+C without a selection.
  interrupt(): Promise<void>;
  // A line of the console's own (a notice, not the core's output).
  notice(text: string): void;
  // The prompt from the last job's result.
  refreshPrompt(): void;
  // Called after every job; returns the unsubscribe.
  onJobDone(cb: () => void): () => void;
  // Drops the queue and the listeners, cancels the model's frame; a job
  // in flight finishes without touching the state.
  dispose(): void;
}

const hasFrame = typeof requestAnimationFrame === 'function';
const frame = (fn: () => void): number =>
  hasFrame ? requestAnimationFrame(() => fn()) : (setTimeout(fn, 16) as unknown as number);
const cancelFrame = (id: number): void => (hasFrame ? cancelAnimationFrame(id) : clearTimeout(id));

export function createConsole(): Console {
  const state: ConsoleState = $state({
    entries: [],
    prompt: '',
    runningSince: null,
    queued: 0,
  });
  const model = new ConsoleModel({
    schedule: frame,
    cancel: cancelFrame,
    onFlush: (entries) => (state.entries = entries),
  });
  const queue: Array<{ text: string; spans?: readonly HlSpan[] }> = [];
  let jobDone: Array<() => void> = [];
  let pumping = false;
  let disposed = false;

  function refreshPrompt(): void {
    state.prompt = getRuntimePrompt() ?? '';
  }

  function notice(text: string): void {
    model.push({ kind: 'print', line: text });
  }

  async function pump(): Promise<void> {
    if (pumping) return;
    pumping = true;
    try {
      while (queue.length && !disposed) {
        if (!isModuleReady()) await whenModuleReady();
        if (disposed) return;
        const { text, spans } = queue.shift()!;
        state.queued = queue.length;
        model.command(text, spans);
        // `clear` is the console's own: no round trip.
        if (text.trim() === 'clear') {
          model.clear();
          continue;
        }
        state.runningSince = performance.now();
        const failed = await gsEvalLine(text);
        if (disposed) return;
        if (failed) notice(`Command failed: ${failed}`);
        state.runningSince = null;
        refreshPrompt();
        for (const cb of jobDone) cb();
      }
    } finally {
      pumping = false;
    }
  }

  return {
    state,
    model,
    refreshPrompt,
    notice,
    submit(text, spans) {
      if (disposed || !text.trim()) return;
      queue.push({ text, spans });
      state.queued = queue.length;
      void pump();
    },
    // Drop the type-ahead, then cancel the running job, else stop a run
    // this console started; otherwise say so.
    async interrupt() {
      queue.length = 0;
      state.queued = 0;
      if (!isModuleReady()) return;
      try {
        const did = await shellInterrupt();
        if (did === 'nothing') notice('^C  (nothing to interrupt; Pause stops the machine)');
      } catch (err) {
        notice(`Interrupt failed: ${err instanceof Error ? err.message : String(err)}`);
      }
    },
    onJobDone(cb) {
      jobDone.push(cb);
      return () => {
        jobDone = jobDone.filter((f) => f !== cb);
      };
    },
    dispose() {
      disposed = true;
      queue.length = 0;
      jobDone = [];
      model.dispose();
    },
  };
}

// The app's console.
export const appConsole = createConsole();

// A statement another surface ran on the user's behalf (dimmed).
export function consoleEcho(text: string): void {
  appConsole.model.echo(text);
}

export function consoleSubmit(text: string, spans?: readonly HlSpan[]): void {
  appConsole.submit(text, spans);
}

// Called after every console job (SYSTEM re-reads what it shows).
export function onConsoleJobDone(cb: () => void): () => void {
  return appConsole.onJobDone(cb);
}
