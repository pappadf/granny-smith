// The terminal pane's input path against a stand-in xterm: `onData` is the
// only input (F-40), what is typed while a command runs waits for its prompt
// (N-55), Ctrl-C interrupts and drops the type-ahead, and an unmount
// disposes the terminal (F-39).
import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import TerminalPane from '@/components/panel-views/terminal/TerminalPane.svelte';

const term = vi.hoisted(() => ({
  instances: [] as {
    data: ((d: string) => void) | null;
    disposed: boolean;
    written: string;
  }[],
}));

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    cols = 80;
    options = {};
    private rec = { data: null as ((d: string) => void) | null, disposed: false, written: '' };
    constructor() {
      term.instances.push(this.rec);
    }
    open() {}
    write(s: string) {
      this.rec.written += s;
    }
    writeln(s: string) {
      this.rec.written += s + '\n';
    }
    loadAddon() {}
    onData(h: (d: string) => void) {
      this.rec.data = h;
    }
    focus() {}
    scrollToBottom() {}
    dispose() {
      this.rec.disposed = true;
    }
  },
}));
vi.mock('@xterm/addon-fit', () => ({
  FitAddon: class {
    fit() {}
  },
}));
vi.mock('@xterm/xterm/css/xterm.css', () => ({}));

const bus = vi.hoisted(() => ({
  lines: [] as string[],
  gate: null as null | Promise<void>,
  interrupts: 0,
}));
vi.mock('@/bus/emulator', async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  isModuleReady: () => true,
  whenModuleReady: () => Promise.resolve(),
  seedPrompt: () => Promise.resolve(),
  getRuntimePrompt: () => '> ',
  tabComplete: async () => null,
  shellInterrupt: async () => {
    bus.interrupts++;
  },
  gsEvalLine: async (line: string) => {
    bus.lines.push(line);
    if (bus.gate) await bus.gate;
  },
}));
vi.mock('@/bus/opfs', async (importOriginal) => ({
  ...(await importOriginal<Record<string, unknown>>()),
  opfs: { readFile: async () => Promise.reject(new Error('none')) },
  writeToOPFS: async () => {},
}));

beforeEach(() => {
  term.instances.length = 0;
  bus.lines = [];
  bus.gate = null;
  bus.interrupts = 0;
});

async function mounted() {
  const r = render(TerminalPane);
  await waitFor(() => {
    if (!term.instances[0]?.data) throw new Error('xterm not open');
  });
  const t = term.instances[0];
  return { ...r, t, send: (d: string) => t.data!(d) };
}

describe('TerminalPane input', () => {
  it('typed text is inserted once and submitted', async () => {
    const { send } = await mounted();
    send('mach');
    send('ine.cpu.pc');
    send('\r');
    await waitFor(() => expect(bus.lines).toEqual(['machine.cpu.pc']));
  });

  it('a multi-line paste runs its lines in order', async () => {
    const { send } = await mounted();
    send('echo one\recho two\r');
    await waitFor(() => expect(bus.lines).toEqual(['echo one', 'echo two']));
  });

  it('type-ahead during a running command waits for the prompt', async () => {
    const { send } = await mounted();
    let release!: () => void;
    bus.gate = new Promise<void>((r) => (release = r));
    send('slow\r');
    await waitFor(() => expect(bus.lines).toEqual(['slow']));
    send('next\r'); // typed while `slow` runs
    await new Promise((r) => setTimeout(r, 20));
    expect(bus.lines).toEqual(['slow']);
    bus.gate = null;
    release();
    await waitFor(() => expect(bus.lines).toEqual(['slow', 'next']));
  });

  it('backspace and cursor keys edit the line', async () => {
    const { send } = await mounted();
    send('abd\x7fc\x1b[D\x1b[Dx\r'); // "abc", two left, insert x → "axbc"
    await waitFor(() => expect(bus.lines).toEqual(['axbc']));
  });

  it('Ctrl-C interrupts and drops the type-ahead', async () => {
    const { send } = await mounted();
    let release!: () => void;
    bus.gate = new Promise<void>((r) => (release = r));
    send('slow\r');
    await waitFor(() => expect(bus.lines).toEqual(['slow']));
    send('queued\r');
    send('\x03');
    await waitFor(() => expect(bus.interrupts).toBe(1));
    bus.gate = null;
    release();
    await new Promise((r) => setTimeout(r, 20));
    expect(bus.lines).toEqual(['slow']);
  });

  it('an unmount disposes the terminal', async () => {
    const { t, unmount } = await mounted();
    unmount();
    expect(t.disposed).toBe(true);
  });
});
