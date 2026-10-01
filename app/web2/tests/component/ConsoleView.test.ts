// The console view: entries render by kind, a value's JSON gives object
// links and expandable maps/lists, the context menu copies commands and
// output, Ctrl+F finds, and submitted lines run one job at a time, in order.
import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const runs: string[] = [];
let release: (() => void) | null = null;
let blockRuns = false;
// What shell.complete answers (the hint follows its context).
let completion: unknown = null;

vi.mock('@/bus/emulator', () => ({
  seedPrompt: async () => {},
  getRuntimePrompt: () => 'gs>',
  tabComplete: async () => completion,
  gsEval: async (path: string, args: unknown[] = []) =>
    path === 'shell.highlight' && args[0] === 'util.echo 42'
      ? [
          { start: 0, end: 4, class: 'object' },
          { start: 5, end: 9, class: 'method' },
          { start: 10, end: 12, class: 'number' },
        ]
      : path === 'shell.usage' && args[0] === 'machine.floppy.drive[0].insert'
        ? {
            signature: 'machine.floppy.drive[0].insert <path> [writable]',
            arg_spans: [
              [31, 37],
              [38, 48],
            ],
            text: 'machine.floppy.drive[0].insert <path> [writable]',
          }
        : null,
  needsContinuation: async () => false,
  whenModuleReady: async () => {},
  isModuleReady: () => true,
  shellInterrupt: async () => 'nothing',
  gsEvalLine: async (line: string) => {
    runs.push(line);
    if (blockRuns) await new Promise<void>((r) => (release = r));
  },
}));

import ConsoleView from '@/components/panel-views/terminal/ConsoleView.svelte';
import { createConsole, type Console } from '@/state/console.svelte';
import {
  registerBrowserReveal,
  writeToConsole,
} from '@/components/panel-views/terminal/terminalBridge';
import { closeContextMenu } from '@/components/common/ContextMenu.svelte';
import { layout } from '@/state/layout.svelte';
import { systemView } from '@/state/system.svelte';

let clip: string[];
// Each test's own console.
let con: Console;
const view = () => render(ConsoleView, { props: { console: con } });

beforeEach(() => {
  completion = null;
  con = createConsole();
  runs.length = 0;
  blockRuns = false;
  release = null;
  clip = [];
  Object.defineProperty(navigator, 'clipboard', {
    value: {
      writeText: async (t: string) => void clip.push(t),
      readText: async () => '',
    },
    configurable: true,
  });
});

afterEach(() => {
  con.dispose();
  closeContextMenu();
  registerBrowserReveal(null);
});

// Feed one job's records, as the bus would.
function job(id: number, records: Array<Parameters<Console['model']['push']>[0]>) {
  const m = con.model;
  m.push({ kind: 'job_start', job: id });
  for (const r of records) m.push(r);
  m.push({ kind: 'job_end', job: id });
}

const entries = (c: HTMLElement) =>
  Array.from(c.querySelectorAll('.console-output .entry')).map((e) => [
    e.classList[1],
    (e as HTMLElement).textContent,
  ]);

describe('ConsoleView', () => {
  it('renders entries by kind', async () => {
    const { container } = view();
    const m = con.model;
    m.command('echo hi');
    job(1, [{ kind: 'output', job: 1, text: 'hi\n' }]);
    m.push({ kind: 'stderr', line: 'warn' });
    m.echo('machine.cpu.d0 = 0x1');
    job(2, [{ kind: 'error', job: 2, lines: ['bad'] }]);
    await waitFor(() =>
      expect(entries(container)).toEqual([
        ['command', 'echo hi'],
        ['text', 'hi'],
        ['stderr', 'warn'],
        ['echo', 'machine.cpu.d0 = 0x1'],
        ['error', 'bad'],
      ]),
    );
  });

  it('an object value links to its node in the browser', async () => {
    const reveal = vi.fn();
    registerBrowserReveal(reveal);
    const { container } = view();
    job(3, [
      { kind: 'value_begin', job: 3 },
      { kind: 'output', job: 3, text: '<object>\n' },
      { kind: 'value', job: 3, json: { object: 'cpu', name: 'cpu', path: 'machine.cpu' } },
    ]);
    const link = await waitFor(() => {
      const a = container.querySelector('.entry.value .obj-link');
      expect(a).not.toBeNull();
      return a as HTMLElement;
    });
    await fireEvent.click(link);
    expect(reveal).toHaveBeenCalledWith('machine.cpu');
    // Ctrl/Cmd-click reveals it in SYSTEM instead.
    await fireEvent.click(link, { ctrlKey: true });
    expect(reveal).toHaveBeenCalledTimes(1);
    expect(layout.activeTab).toBe('machine');
    expect(systemView.reveal).toBe('machine.cpu');
  });

  it('a map value expands to its keys', async () => {
    const { container } = view();
    job(4, [
      { kind: 'value_begin', job: 4 },
      { kind: 'output', job: 4, text: '{"a":1,"b":[2,3]}\n' },
      { kind: 'value', job: 4, json: { a: 1, b: [2, 3] } },
    ]);
    const tree = await waitFor(() => {
      const d = container.querySelector('.entry.value details.value-tree');
      expect(d).not.toBeNull();
      return d as HTMLDetailsElement;
    });
    expect(tree.querySelector('summary')?.textContent).toBe('{"a":1,"b":[2,3]}');
    const keys = Array.from(tree.querySelectorAll(':scope > .kv > .kv-row > .kv-key')).map(
      (k) => k.textContent,
    );
    expect(keys).toEqual(['a', 'b']);
  });

  it('Copy as commands copies the clicked command; Copy output its job', async () => {
    const { container } = view();
    const m = con.model;
    m.command('let a = 1');
    job(5, [{ kind: 'output', job: 5, text: 'one\ntwo\n' }]);
    const cmd = await waitFor(() => {
      const e = container.querySelector('.entry.command');
      expect(e).not.toBeNull();
      return e as HTMLElement;
    });
    await fireEvent.contextMenu(cmd);
    await fireEvent.click(await waitFor(() => findMenuItem('Copy as commands')));
    await waitFor(() => expect(clip).toEqual(['let a = 1']));

    await fireEvent.contextMenu(container.querySelector('.entry.text') as HTMLElement);
    await fireEvent.click(await waitFor(() => findMenuItem('Copy output')));
    await waitFor(() => expect(clip[1]).toBe('one\ntwo'));

    await fireEvent.contextMenu(container.querySelector('.entry.text') as HTMLElement);
    await fireEvent.click(await waitFor(() => findMenuItem('Clear')));
    await waitFor(() => expect(entries(container)).toEqual([]));
  });

  it('Ctrl+F finds and marks matches', async () => {
    const { container } = view();
    const m = con.model;
    m.push({ kind: 'print', line: 'alpha beta' });
    m.push({ kind: 'print', line: 'gamma' });
    m.push({ kind: 'print', line: 'Beta again' });
    await waitFor(() => expect(entries(container).length).toBe(3));
    await fireEvent.keyDown(container.querySelector('.console') as HTMLElement, {
      key: 'f',
      ctrlKey: true,
    });
    const box = await waitFor(() => {
      const i = container.querySelector('.find-input');
      expect(i).not.toBeNull();
      return i as HTMLInputElement;
    });
    await fireEvent.input(box, { target: { value: 'beta' } });
    await waitFor(() => expect(container.querySelectorAll('.entry.find-hit').length).toBe(2));
    expect(container.querySelector('.find-count')?.textContent).toBe('1 of 2');
    expect(container.querySelector('.entry.find-current mark')?.textContent).toBe('beta');
    // Match case: only the lower-case one.
    await fireEvent.click(container.querySelector('.find-btn[title="Match case"]') as HTMLElement);
    await waitFor(() => expect(container.querySelectorAll('.entry.find-hit').length).toBe(1));
  });

  it('runs submitted lines one at a time, in order, each after its command entry', async () => {
    const { container } = view();
    blockRuns = true;
    con.submit('echo one');
    con.submit('echo two');
    await waitFor(() => expect(runs).toEqual(['echo one']));
    expect(con.state.queued).toBe(1);
    expect(container.querySelector('.console')?.classList.contains('busy')).toBe(true);
    release?.();
    await waitFor(() => expect(runs).toEqual(['echo one', 'echo two']));
    release?.();
    await waitFor(() =>
      expect(entries(container)).toEqual([
        ['command', 'echo one'],
        ['command', 'echo two'],
      ]),
    );
  });

  it('Ctrl+C drops the type-ahead and says when there is nothing to interrupt', async () => {
    const { container } = view();
    blockRuns = true;
    con.submit('echo one');
    con.submit('echo two');
    await waitFor(() => expect(runs).toEqual(['echo one']));
    await con.interrupt();
    release?.();
    await waitFor(() =>
      expect(entries(container).map((e) => e[1])).toContain(
        '^C  (nothing to interrupt; Pause stops the machine)',
      ),
    );
    expect(runs).toEqual(['echo one']);
  });

  it('shows the prompt beside the input', async () => {
    const { container } = view();
    await waitFor(() =>
      expect(container.querySelector('.console-prompt')?.textContent).toBe('gs>'),
    );
  });
});

describe('ConsoleView signature hint', () => {
  it('shows the signature with the current argument underlined; Esc hides it, Ctrl+Shift+Space brings it back', async () => {
    const { container } = view();
    const cm = await waitFor(() => {
      const el = container.querySelector('.cm-content');
      expect(el).toBeTruthy();
      return el as HTMLElement;
    });
    await waitFor(() => expect(writeToConsole('')).toBe(true)); // the input is registered
    completion = {
      candidates: [],
      span: { start: 42, end: 42 },
      context: { method: 'machine.floppy.drive[0].insert', argIndex: 1, argName: 'writable' },
    };
    writeToConsole('machine.floppy.drive[0].insert /a.img writable=');
    const hint = await waitFor(() => {
      const h = container.querySelector('.sig-hint');
      expect(h).toBeTruthy();
      return h as HTMLElement;
    });
    expect(hint.textContent).toBe('machine.floppy.drive[0].insert <path> [writable]');
    expect(hint.querySelector('.sig-arg')?.textContent).toBe('[writable]');

    await fireEvent.keyDown(cm, { key: 'Escape' });
    await waitFor(() => expect(container.querySelector('.sig-hint')).toBeNull());
    await fireEvent.keyDown(cm, { key: ' ', code: 'Space', ctrlKey: true, shiftKey: true });
    await waitFor(() => expect(container.querySelector('.sig-hint')).toBeTruthy());

    // Out of the arguments: no hint.
    completion = {
      candidates: [],
      span: { start: 0, end: 0 },
      context: { method: null, argIndex: null, argName: null },
    };
    writeToConsole('x');
    await waitFor(() => expect(container.querySelector('.sig-hint')).toBeNull());
  });
});

describe('ConsoleView scrolling', () => {
  it('stays where the user scrolled, but a submitted command returns it to the bottom', async () => {
    const { container } = view();
    const out = container.querySelector('.console-output') as HTMLElement;
    // jsdom has no layout: give the output a fixed geometry.
    Object.defineProperty(out, 'scrollHeight', { value: 1000, configurable: true });
    Object.defineProperty(out, 'clientHeight', { value: 100, configurable: true });
    Object.defineProperty(out, 'scrollTop', { value: 100, writable: true, configurable: true });
    await fireEvent.scroll(out);
    con.model.command('echo more');
    await waitFor(() => expect(entries(container).length).toBe(1));
    await new Promise((r) => requestAnimationFrame(() => r(null)));
    expect(out.scrollTop).toBe(100);

    const cm = await waitFor(() => {
      const el = container.querySelector('.cm-content');
      expect(el).toBeTruthy();
      return el as HTMLElement;
    });
    await waitFor(() => expect(writeToConsole('')).toBe(true));
    writeToConsole('echo hi');
    await fireEvent.keyDown(cm, { key: 'Enter' });
    await waitFor(() => expect(out.scrollTop).toBe(1000));
  });
});

describe('ConsoleView highlighting', () => {
  it('colours the input and keeps the colours on the command entry', async () => {
    const { container } = view();
    const cm = await waitFor(() => {
      const el = container.querySelector('.cm-content');
      expect(el).toBeTruthy();
      return el as HTMLElement;
    });
    await waitFor(() => expect(writeToConsole('')).toBe(true));
    writeToConsole('util.echo 42');
    await waitFor(() =>
      expect(container.querySelector('.console-input .hl-method')?.textContent).toBe('echo'),
    );
    await fireEvent.keyDown(cm, { key: 'Enter' });
    const entry = await waitFor(() => {
      const e = container.querySelector('.entry.command');
      expect(e).toBeTruthy();
      return e as HTMLElement;
    });
    expect(entry.querySelector('.hl-method')?.textContent).toBe('echo');
    expect(entry.querySelector('.hl-number')?.textContent).toBe('42');
    expect(entry.textContent).toBe('util.echo 42');
  });
});

function findMenuItem(label: string): HTMLElement {
  const item = Array.from(document.querySelectorAll<HTMLElement>('[role="menuitem"]')).find(
    (el) => el.textContent?.trim() === label,
  );
  if (!item) throw new Error(`no menu item ${label}`);
  return item;
}
