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
// tree (machine → cpu, a two-drive collection; debug), the task chips and a
// usage text, and drive the component.
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
        task: null,
        collection: false,
      },
      {
        name: 'debug',
        kind: 'child',
        category: 'basic',
        label: 'debug',
        doc: 'Debugger',
        domain: 'emulator',
        task: 'debug',
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
        task: 'debug',
        collection: false,
      },
      {
        name: 'drive',
        kind: 'child',
        category: 'basic',
        label: 'Drives',
        doc: 'Floppy drives',
        task: 'storage',
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
        task: 'logs',
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
        task: 'debug',
        readonly: false,
        type: t,
      },
      {
        name: 'vbr',
        kind: 'attr',
        category: 'advanced',
        label: 'vbr',
        doc: 'Vector base',
        task: 'debug',
        readonly: false,
        type: t,
      },
      {
        name: 'step',
        kind: 'method',
        category: 'basic',
        label: 'step',
        doc: 'Run N instructions',
        task: 'debug',
        hidden: false,
      },
      {
        name: 'insert',
        kind: 'method',
        category: 'basic',
        label: 'insert',
        doc: 'Mount an image',
        task: 'debug',
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
        task: 'logs',
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
        task: 'storage',
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
      if (path === 'shell.tasks')
        return [
          { id: 'run', label: 'Run', doc: '' },
          { id: 'storage', label: 'Storage', doc: '' },
          { id: 'debug', label: 'Debug', doc: '' },
        ];
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

async function open(container: HTMLElement, name: string): Promise<void> {
  const r = await row(container, name);
  await fireEvent.click(r.querySelector('.twistie')!);
}

describe('CommandBrowser (structural, model-generated)', () => {
  it('root nodes sit under domain dividers', async () => {
    const { container } = render(CommandBrowser);
    await row(container, 'machine');
    const dividers = Array.from(container.querySelectorAll('.divider')).map((d) => d.textContent);
    expect(dividers).toEqual(['Machine', 'Emulator']);
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

  it('advanced members show only with the Advanced toggle', async () => {
    const { container, getByText } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    await row(container, 'pc');
    expect(
      Array.from(container.querySelectorAll('.name')).some((n) => n.textContent === 'vbr'),
    ).toBe(false);
    await fireEvent.click(getByText('Advanced'));
    await row(container, 'vbr');
  });

  it('a task chip dims rows of other tasks and collapses their subtrees', async () => {
    const { container, getByText } = render(CommandBrowser);
    await open(container, 'machine');
    await row(container, 'drive');
    await open(container, 'cpu');
    await row(container, 'pc');
    await fireEvent.click(await waitFor(() => getByText('Storage')));
    await waitFor(() =>
      expect((container.querySelector('.cmd-row.dim .name') as HTMLElement | null) !== null).toBe(
        true,
      ),
    );
    const drive = await row(container, 'drive');
    expect(drive.classList.contains('dim')).toBe(false);
    // cpu belongs to debug: collapsed while Storage filters.
    await waitFor(() =>
      expect(
        Array.from(container.querySelectorAll('.name')).some((n) => n.textContent === 'pc'),
      ).toBe(false),
    );
  });

  it('selecting a leaf writes its path and shows its usage text', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const step = await row(container, 'step');
    await fireEvent.click(step.querySelector('.cmd-line')!);
    expect(input.writes).toEqual(['machine.cpu.step ']);
    await waitFor(() =>
      expect(step.querySelector('.usage')?.textContent).toBe('USAGE OF machine.cpu.step'),
    );
  });
});

describe('CommandBrowser ↔ console', () => {
  it('writes each kind of row as it is typed', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    const click = async (name: string) =>
      fireEvent.click((await row(container, name)).querySelector('.cmd-line')!);
    await click('machine'); // selecting an object also opens it
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

  it('expanding writes nothing; ↑/↓ select and write', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await row(container, 'cpu');
    expect(input.writes).toEqual([]);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.keyDown(tree, { key: 'ArrowDown' });
    await waitFor(() => expect(input.writes.length).toBe(1));
  });

  it('Esc restores the input as it was when the browser took focus', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.focusIn(tree);
    await fireEvent.click((await row(container, 'machine')).querySelector('.cmd-line')!);
    await fireEvent.click((await row(container, 'cpu')).querySelector('.cmd-line')!);
    expect(input.getState).toHaveBeenCalledTimes(1); // snapshot on the first write only
    await fireEvent.keyDown(tree, { key: 'Escape' });
    expect(input.restore).toHaveBeenCalledWith({ text: 'orig', cursor: 4 });
    expect(input.focusEnd).toHaveBeenCalled();
  });

  it('Enter on a leaf, or Tab, hands focus to the console', async () => {
    const input = fakeInput();
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    await fireEvent.click((await row(container, 'pc')).querySelector('.cmd-line')!);
    const tree = container.querySelector('.cmd-tree') as HTMLElement;
    await fireEvent.keyDown(tree, { key: 'Enter' });
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
        candidates: [{ text: 'step', kind: 'method', doc: '', task: 'debug' }],
        span: { start: 12, end: 14 },
        context: { method: null, argIndex: null, argName: null },
      },
      false,
    );
    const step = await row(container, 'step');
    await waitFor(() => expect(step.classList.contains('selected')).toBe(true));
    expect(step.classList.contains('match')).toBe(true);
    expect((await row(container, 'pc')).classList.contains('dim')).toBe(true);
    await waitFor(() =>
      expect(step.querySelector('.usage')?.textContent).toBe('USAGE OF machine.cpu.step'),
    );
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
    await waitFor(() => expect(ins.querySelector('.usage-arg')?.textContent).toBe('[writable]'));
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
        candidates: [{ text: 'pc', kind: 'attr', doc: '', task: 'debug' }],
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
    await waitFor(() => expect(ins.querySelectorAll('.usage .hl-method').length).toBe(2));
    const methods = Array.from(ins.querySelectorAll('.usage .hl-method')).map((e) => e.textContent);
    expect(methods).toEqual(['insert', 'insert']);
    expect(ins.querySelector('.usage')?.textContent).toBe(
      'machine.cpu.insert <path> [writable]\ne.g.  machine.cpu.insert a.img\n\nMount an image',
    );
  });
});
