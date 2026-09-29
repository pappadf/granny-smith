import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import CommandBrowser from '@/components/panel-views/terminal/CommandBrowser.svelte';
import { registerTerminalInsert } from '@/components/panel-views/terminal/terminalBridge';
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
      if (path === 'shell.usage')
        return { signature: '', arg_spans: [], text: `USAGE OF ${String(args?.[0])}` };
      if (path === 'shell.alias.list') return [];
      if (path === 'shell.keywords') return [];
      return null;
    },
  };
});

vi.mock('@/state/machine.svelte', () => ({ machine: { status: 'idle' } }));

beforeEach(() => {
  registerTerminalInsert(null);
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

  it('selecting a leaf inserts its path and shows its usage text', async () => {
    const setter = vi.fn();
    registerTerminalInsert(setter);
    const { container } = render(CommandBrowser);
    await open(container, 'machine');
    await open(container, 'cpu');
    const step = await row(container, 'step');
    await fireEvent.click(step.querySelector('.cmd-line')!);
    expect(setter).toHaveBeenCalledWith('machine.cpu.step');
    await waitFor(() =>
      expect(step.querySelector('.usage')?.textContent).toBe('USAGE OF machine.cpu.step'),
    );
  });
});
