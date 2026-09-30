import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import CommandBrowser from '@/components/panel-views/terminal/CommandBrowser.svelte';
import {
  registerConsoleInput,
  type ConsoleInputApi,
} from '@/components/panel-views/terminal/terminalBridge';
import { publishCompletion } from '@/state/terminalSync.svelte';
import { invalidate } from '@/lib/commandsTree';

// The browser renders whatever the model says: mock the bus with a small
// tree (machine → cpu, a two-drive collection; debug) and a usage text, and
// drive the component.
vi.mock('@/bus/emulator', () => {
  const t = { kind: 'uint', width: 0, presentation: 'hex', enum: null };
  const members: Record<string, unknown[]> = {
    'meta.members': [
      {
        name: 'machine',
        kind: 'child',
        category: 'basic',
        label: 'M',
        doc: 'The computer',
        domain: 'machine',
        collection: false,
      },
      {
        name: 'debug',
        kind: 'child',
        category: 'basic',
        label: 'debug',
        doc: 'Debugger',
        domain: 'emulator',
        collection: false,
      },
    ],
    'machine.meta.members': [
      {
        name: 'cpu',
        kind: 'child',
        category: 'basic',
        label: 'cpu',
        doc: 'CPU',
        collection: false,
      },
      {
        name: 'drive',
        kind: 'child',
        category: 'basic',
        label: 'Drives',
        doc: 'Floppy drives',
        collection: true,
        indices: [0, 1],
        keys: null,
      },
      {
        name: 'category',
        kind: 'child',
        category: 'basic',
        label: 'category',
        doc: 'Log categories',
        collection: true,
        indices: null,
        keys: ['scsi'],
      },
    ],
    'machine.cpu.meta.members': [
      {
        name: 'pc',
        kind: 'attr',
        category: 'basic',
        label: 'pc',
        doc: 'Program counter. The next instruction.',
        readonly: false,
        type: t,
      },
      {
        name: 'vbr',
        kind: 'attr',
        category: 'advanced',
        label: 'vbr',
        doc: 'Vector base',
        readonly: false,
        type: t,
      },
      {
        name: 'step',
        kind: 'method',
        category: 'basic',
        label: 'step',
        doc: 'Run N instructions',
        hidden: false,
      },
      {
        name: 'insert',
        kind: 'method',
        category: 'basic',
        label: 'insert',
        doc: 'Mount an image',
        hidden: false,
      },
    ],
    'machine.category.meta.members': [
      {
        name: 'entries',
        kind: 'child',
        category: 'basic',
        label: 'entries',
        doc: '',
        indexed: true,
        indices: null,
        keys: ['scsi'],
      },
    ],
    'machine.drive.meta.members': [
      {
        name: 'entries',
        kind: 'child',
        category: 'basic',
        label: 'entries',
        doc: '',
        indexed: true,
        indices: [0, 1],
        keys: null,
      },
    ],
  };
  return {
    isModuleReady: () => true,
    onCoreEvent: () => () => {},
    whenModuleReady: () => Promise.resolve(),
    gsEval: async (path: string, args?: unknown[]) => {
      if (path in members) return members[path];
      if (path === 'shell.highlight' && args?.[0] === 'machine.cpu.insert <path> [writable]')
        return [
          { start: 0, end: 7, class: 'object' },
          { start: 8, end: 11, class: 'object' },
          { start: 12, end: 18, class: 'method' },
        ];
      if (path === 'shell.highlight' && args?.[0] === 'machine.cpu.insert a.img')
        return [{ start: 12, end: 18, class: 'method' }];
      if (path === 'shell.highlight') return [];
      if (path === 'shell.usage' && args?.[0] === 'machine.cpu.insert')
        return {
          signature: 'machine.cpu.insert <path> [writable]',
          arg_spans: [
            [19, 25],
            [26, 36],
          ],
          text: 'machine.cpu.insert <path> [writable]\ne.g.  machine.cpu.insert a.img\n\nMount an image',
        };
      if (path === 'shell.usage')
        return { signature: '', arg_spans: [], text: `USAGE OF ${String(args?.[0])}` };
      if (path === 'shell.alias.list') return [];
      if (path === 'shell.keywords') return [];
      if (path === 'shell.command.list')
        return [
          { name: 'ls', target: 'debug.step', doc: 'Step', builtin: true, available: true },
          { name: 'run', target: 'scheduler.run', doc: '', builtin: true, available: false },
        ];
      return null;
    },
  };
});

vi.mock('@/state/machine.svelte', () => ({ machine: { status: 'idle' } }));

// A console input that records what the browser does to it.
function fakeInput() {
  const writes: string[] = [];
  const api = {
    writes,
    replaceToken: vi.fn((t: string) => void writes.push(t)),
    focusEnd: vi.fn(),
    getState: vi.fn(() => ({ text: 'orig', cursor: 4 })),
    restore: vi.fn(),
  };
  registerConsoleInput(api as ConsoleInputApi);
  return api;
}

beforeEach(() => {
  registerConsoleInput(null);
  invalidate('');
});

// The row whose name is `name`, once rendered.
async function row(container: HTMLElement, name: string): Promise<HTMLElement> {
  return waitFor(() => {
    const el = Array.from(container.querySelectorAll('.cmd-row')).find(
      (r) => r.querySelector('.name')?.textContent === name,
    ) as HTMLElement | undefined;
    if (!el) throw new Error(`row ${name} not rendered yet`);
    return el;
  });
}

// The details pane's usage text, or null when the pane is closed.
function usageText(container: HTMLElement): string | null {
  return container.querySelector('.details .usage')?.textContent ?? null;
}

async function open(container: HTMLElement, name: string): Promise<void> {
  const r = await row(container, name);
  await fireEvent.click(r.querySelector('.twistie')!);
}

describe('CommandBrowser (structural, model-generated)', () => {
  it('the root is expandable sections; Aliases and Language start collapsed', async () => {
    const { container } = render(CommandBrowser);
    await row(container, 'machine');
    const sections = Array.from(container.querySelectorAll('.cmd-row.kind-section'));
    expect(sections.map((s) => s.querySelector('.name')?.textContent)).toEqual([
      'Commands',
      'Machine',
      'Emulator',
      'Aliases',
      'Language',
    ]);
    expect(sections.map((s) => s.getAttribute('aria-expanded'))).toEqual([
      'true',
      'true',
      'true',
      'false',
      'false',
    ]);
    // Collapsing a domain hides its members.
    await fireEvent.click(sections[1].querySelector('.twistie')!);
    await waitFor(() =>
      expect(
        Array.from(container.querySelectorAll('.name')).some((n) => n.textContent === 'machine'),
      ).toBe(false),
    );
  });

  it('Commands lists the available commands, typed bare', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    const ls = await row(container, 'ls');
    expect(ls.querySelector('.doc')?.textContent).toContain('debug.step');
    expect(
      Array.from(container.querySelectorAll('.name')).some((n) => n.textContent === 'run'),
    ).toBe(false);
    await fireEvent.click(ls.querySelector('.cmd-line')!);
    expect(input.writes).toEqual([]);
    await fireEvent.dblClick(ls.querySelector('.cmd-line')!);
    expect(input.writes).toEqual(['ls ']);
  });

  it('a leaf row shows the first sentence of its doc and its type', async () => {
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const pc = await row(container, 'pc');
    expect(pc.querySelector('.doc')?.textContent).toBe('Program counter.');
    expect(pc.querySelector('.type')?.textContent).toBe('uint, hex');
  });

  it('a collection expands to its live entries', async () => {
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'drive');
    await row(container, '[0]');
    await row(container, '[1]');
  });

  it('lists advanced members too, with no filter row', async () => {
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    await row(container, 'pc');
    await row(container, 'vbr');
    expect(container.querySelector('.chips')).toBeNull();
  });

  it('a click previews: the usage shows in the details pane, nothing is written', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const step = await row(container, 'step');
    await fireEvent.click(step.querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).toBe('USAGE OF machine.cpu.step'));
    expect(input.writes).toEqual([]);
    // Another row replaces the preview; still nothing written.
    await fireEvent.click((await row(container, 'pc')).querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).toBe('USAGE OF machine.cpu.pc'));
    expect(input.writes).toEqual([]);
  });

  it('the details pane closes with ×, with Esc, and with a second click on the row', async () => {
    fakeInput();
    const { container, getByLabelText } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const step = await row(container, 'step');
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.click(step.querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).not.toBeNull());
    await fireEvent.click(getByLabelText('Close'));
    await waitFor(() => expect(usageText(container)).toBeNull());
    await fireEvent.click(step.querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).not.toBeNull());
    await fireEvent.keyDown(tree, { key: 'Escape' });
    await waitFor(() => expect(usageText(container)).toBeNull());
    await fireEvent.click(step.querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).not.toBeNull());
    await fireEvent.click(step.querySelector('.cmd-line')!);
    await waitFor(() => expect(usageText(container)).toBeNull());
  });

  it('Insert writes the selection and hands focus to the console', async () => {
    const input = fakeInput();
    const { container, getByText } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    await fireEvent.click((await row(container, 'step')).querySelector('.cmd-line')!);
    await fireEvent.click(await waitFor(() => getByText('Insert')));
    expect(input.writes).toEqual(['machine.cpu.step ']);
    expect(input.focusEnd).toHaveBeenCalledTimes(1);
  });
});

describe('CommandBrowser ↔ console', () => {
  it('writes each kind of row as it is typed', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    // A click opens an object; a double-click inserts the row.
    const click = async (name: string) => {
      const line = (await row(container, name)).querySelector('.cmd-line')!;
      await fireEvent.click(line);
      await fireEvent.dblClick(line);
    };
    await click('machine');
    await click('cpu');
    await click('pc');
    await click('step');
    await click('drive');
    await click('[0]');
    await click('category');
    await click('["scsi"]');
    expect(input.writes).toEqual([
      'machine.',
      'machine.cpu.',
      'machine.cpu.pc',
      'machine.cpu.step ',
      'machine.drive[',
      'machine.drive[0].',
      'machine.category["',
      'machine.category["scsi"].',
    ]);
  });

  it('expanding and ↑/↓ write nothing', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await row(container, 'cpu');
    expect(input.writes).toEqual([]);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.keyDown(tree, { key: 'ArrowDown' });
    await fireEvent.keyDown(tree, { key: 'ArrowDown' });
    await waitFor(() => expect(container.querySelector('.cmd-row.selected')).not.toBeNull());
    expect(input.writes).toEqual([]);
  });

  it('Esc restores the input as it was when the browser took focus', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.focusIn(tree);
    await fireEvent.click((await row(container, 'machine')).querySelector('.cmd-line')!);
    await fireEvent.dblClick((await row(container, 'machine')).querySelector('.cmd-line')!);
    await fireEvent.dblClick((await row(container, 'cpu')).querySelector('.cmd-line')!);
    expect(input.getState).toHaveBeenCalledTimes(1); // snapshot on the first write only
    await fireEvent.keyDown(tree, { key: 'Escape' });
    expect(input.restore).toHaveBeenCalledWith({ text: 'orig', cursor: 4 });
    expect(input.focusEnd).toHaveBeenCalled();
  });

  it('Enter on a leaf inserts it; Tab hands focus to the console', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    await fireEvent.click((await row(container, 'pc')).querySelector('.cmd-line')!);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.keyDown(tree, { key: 'Enter' });
    expect(input.writes).toEqual(['machine.cpu.pc']);
    expect(input.focusEnd).toHaveBeenCalledTimes(1);
    await fireEvent.keyDown(tree, { key: 'Tab' });
    expect(input.focusEnd).toHaveBeenCalledTimes(2);
  });

  it('follows the typed token: opens its levels, marks and selects the match, dims the rest', async () => {
    fakeInput();
    const { container } = render(CommandBrowser);
    await row(container, 'machine');
    publishCompletion(
      'machine.cpu.st',
      14,
      {
        candidates: [{ text: 'step', kind: 'method', doc: '' }],
        span: { start: 12, end: 14 },
        context: { method: null, argIndex: null, argName: null },
      },
      false,
    );
    const step = await row(container, 'step');
    await waitFor(() => expect(step.classList.contains('selected')).toBe(true));
    expect(step.classList.contains('match')).toBe(true);
    expect((await row(container, 'pc')).classList.contains('dim')).toBe(true);
    await waitFor(() => expect(usageText(container)).toBe('USAGE OF machine.cpu.step'));
  });

  it('in a method’s arguments: selects the method and marks the argument in its usage', async () => {
    fakeInput();
    const { container } = render(CommandBrowser);
    await row(container, 'machine');
    publishCompletion(
      'machine.cpu.insert /a.img writable=',
      35,
      {
        candidates: [],
        span: { start: 35, end: 35 },
        context: { method: 'machine.cpu.insert', argIndex: 1, argName: 'writable' },
      },
      false,
    );
    const ins = await row(container, 'insert');
    await waitFor(() =>
      expect(container.querySelector('.details .usage-arg')?.textContent).toBe('[writable]'),
    );
    expect(ins.classList.contains('selected')).toBe(true);
  });

  it("keeps its own selection when the input change was the browser's", async () => {
    fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const pc = await row(container, 'pc');
    await fireEvent.click(pc.querySelector('.cmd-line')!);
    publishCompletion(
      'machine.cpu.pc',
      14,
      {
        candidates: [{ text: 'pc', kind: 'attr', doc: '' }],
        span: { start: 12, end: 14 },
        context: { method: null, argIndex: null, argName: null },
      },
      true,
    );
    await waitFor(() => expect(pc.classList.contains('match')).toBe(true));
    expect(pc.classList.contains('selected')).toBe(true);
  });

  it('colours the usage block: the signature and the example lines', async () => {
    fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const ins = await row(container, 'insert');
    await fireEvent.click(ins.querySelector('.cmd-line')!);
    const pane = () => container.querySelector('.details .usage');
    await waitFor(() => expect(pane()?.querySelectorAll('.hl-method').length).toBe(2));
    const methods = Array.from(pane()!.querySelectorAll('.hl-method')).map((e) => e.textContent);
    expect(methods).toEqual(['insert', 'insert']);
    expect(pane()?.textContent).toBe(
      'machine.cpu.insert <path> [writable]\ne.g.  machine.cpu.insert a.img\n\nMount an image',
    );
  });
});
