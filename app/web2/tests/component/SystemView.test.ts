// The SYSTEM tab against a small mocked model: rows by domain and in model
// order, values as the REPL prints them, editing (literal via the bridge
// and echoed, expression and >2^53 via a console statement, errors inline),
// refresh on core events and every 2 s while running, destructive confirm,
// methods-only children as submenus, argument forms, Copy value / path.
import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

type Listener = (ev: { kind: string; event: string; data: Record<string, unknown> }) => void;
const listeners: Listener[] = [];
const calls: Array<[string, unknown[]]> = [];
let d0 = '0x1f';

const hex = { kind: 'uint', width: 4, presentation: 'hex', enum: null };
const int = { kind: 'int', width: 4, presentation: null, enum: null };

const MODEL: Record<string, () => unknown[]> = {
  'meta.members': () => [
    {
      name: 'machine',
      kind: 'child',
      category: 'basic',
      label: 'Macintosh SE/30',
      doc: 'The computer',
      domain: 'machine',
    },
    {
      name: 'files',
      kind: 'child',
      category: 'basic',
      label: 'files',
      doc: 'Files',
      domain: 'emulator',
    },
    {
      name: 'secret',
      kind: 'child',
      category: 'internal',
      label: 'secret',
      doc: '',
      domain: 'emulator',
    },
    { name: 'echo', kind: 'method', category: 'basic', label: 'echo', doc: '' },
  ],
  'machine.meta.members': () => [
    {
      name: 'model',
      kind: 'attr',
      category: 'basic',
      label: 'model',
      doc: 'Model',
      readonly: true,
      value: 'se30',
      type: { kind: 'string', width: 0, presentation: null, enum: null },
    },
    { name: 'cpu', kind: 'child', category: 'basic', label: 'cpu', doc: 'The CPU' },
    { name: 'memory', kind: 'child', category: 'basic', label: 'memory', doc: 'RAM' },
    {
      name: 'mode',
      kind: 'attr',
      category: 'basic',
      label: 'mode',
      doc: 'Mode',
      value: { enum: 'paced', index: 0 },
      type: { kind: 'enum', width: 0, presentation: null, enum: ['paced', 'turbo'] },
    },
    {
      name: 'sound',
      kind: 'attr',
      category: 'basic',
      label: 'sound',
      doc: 'Sound on',
      value: true,
      type: { kind: 'bool', width: 0, presentation: null, enum: null },
    },
    {
      name: 'reset',
      kind: 'method',
      category: 'basic',
      label: 'reset',
      doc: 'Reset the machine',
      verb: 'Reset',
      destructive: true,
      args: [],
    },
  ],
  'machine.memory.meta.members': () => [
    {
      name: 'peek',
      kind: 'method',
      category: 'basic',
      label: 'peek',
      doc: 'Read memory',
      verb: 'Peek',
      args: [
        { name: 'addr', doc: 'Address', type: hex, optional: false, rest: false, default: null },
      ],
    },
  ],
  'machine.cpu.meta.members': () => [
    { name: 'd0', kind: 'attr', category: 'basic', label: 'd0', doc: 'D0', value: d0, type: hex },
    {
      name: 'count',
      kind: 'attr',
      category: 'basic',
      label: 'count',
      doc: 'Count',
      value: 42,
      type: int,
    },
    {
      name: 'sr',
      kind: 'attr',
      category: 'advanced',
      label: 'sr',
      doc: 'SR',
      readonly: true,
      value: '0x2700',
      type: hex,
    },
  ],
  'files.meta.members': () => [
    {
      name: 'create',
      kind: 'method',
      category: 'basic',
      label: 'create',
      doc: 'Create an image',
      verb: 'Create',
      args: [
        {
          name: 'path',
          doc: 'Where',
          type: { kind: 'string', width: 0, presentation: 'path', enum: null },
          optional: false,
          rest: false,
          default: null,
        },
        { name: 'size', doc: 'Bytes', type: int, optional: true, rest: false, default: null },
      ],
    },
  ],
};

vi.mock('@/bus/emulator', () => ({
  isModuleReady: () => true,
  whenModuleReady: async () => {},
  onCoreEvent: (cb: Listener) => {
    listeners.push(cb);
    return () => listeners.splice(listeners.indexOf(cb), 1);
  },
  isGsError: (r: unknown) => !!r && typeof r === 'object' && 'error' in (r as object),
  gsErrorText: (r: { error: string }) => r.error,
  gsEval: async (path: string, args: unknown[] = []) => {
    calls.push([path, args]);
    const m = MODEL[path];
    if (m) return m();
    if (path === 'machine.cpu.d0') {
      if (args[0] === 666) return { error: 'd0: not today' };
      d0 = `0x${(args[0] as number).toString(16)}`;
      return null;
    }
    if (path === 'machine.reset' || path === 'files.create') return true;
    return null;
  },
}));

const echo = vi.fn();
const submit = vi.fn();
vi.mock('@/state/console.svelte', () => ({
  consoleEcho: (s: string) => echo(s),
  consoleSubmit: (s: string) => submit(s),
  onConsoleJobDone: () => () => {},
}));

import SystemView from '@/components/panel-views/machine/SystemView.svelte';
import { machine } from '@/state/machine.svelte';
import { systemView, revealInSystem } from '@/state/system.svelte';
import { layout } from '@/state/layout.svelte';
import { closeContextMenu } from '@/components/common/ContextMenu.svelte';

let clip: string[];
beforeEach(() => {
  calls.length = 0;
  listeners.length = 0;
  d0 = '0x1f';
  echo.mockClear();
  submit.mockClear();
  systemView.expanded = {};
  systemView.showAdvanced = false;
  clip = [];
  Object.defineProperty(navigator, 'clipboard', {
    value: { writeText: async (t: string) => void clip.push(t) },
    configurable: true,
  });
});
afterEach(() => {
  closeContextMenu();
  vi.useRealTimers();
});

const rowEl = (c: HTMLElement, p: string) =>
  c.querySelector(`.sys-row[data-path='${p}']`) as HTMLElement | null;

async function open(c: HTMLElement, p: string): Promise<void> {
  const r = await waitFor(() => {
    const e = rowEl(c, p);
    expect(e).not.toBeNull();
    return e!;
  });
  await fireEvent.click(r.querySelector('.sys-line') as HTMLElement);
}

const valueText = (c: HTMLElement, p: string) =>
  rowEl(c, p)?.querySelector('.value .text')?.textContent ?? null;

function menuItem(label: string): HTMLElement {
  const el = Array.from(document.querySelectorAll<HTMLElement>('[role="menuitem"]')).find(
    (e) => e.textContent?.trim() === label,
  );
  if (!el) throw new Error(`no menu item ${label}`);
  return el;
}

describe('SystemView structure and values', () => {
  it('shows root children under domain dividers, never internal ones', async () => {
    const { container } = render(SystemView);
    await waitFor(() =>
      expect(
        Array.from(container.querySelectorAll('.group-divider')).map((e) => e.textContent),
      ).toEqual(['Machine', 'Emulator']),
    );
    expect(rowEl(container, 'machine')?.textContent).toContain('Macintosh SE/30');
    expect(rowEl(container, 'secret')).toBeNull();
    expect(rowEl(container, 'echo')).toBeNull(); // methods are menu items, not rows
  });

  it('shows values as the REPL prints them, in model order; methods-only nodes are not folders', async () => {
    const { container } = render(SystemView);
    await open(container, 'machine');
    await waitFor(() => expect(rowEl(container, 'machine.cpu')).not.toBeNull());
    const order = Array.from(container.querySelectorAll('.sys-row')).map((e) =>
      e.getAttribute('data-path'),
    );
    expect(order).toEqual([
      'machine',
      'machine.model',
      'machine.cpu',
      'machine.mode',
      'machine.sound',
      'files',
    ]);
    expect(rowEl(container, 'machine.memory')).toBeNull();
    expect(valueText(container, 'machine.model')).toBe('se30');
    expect(valueText(container, 'machine.mode')).toBe('paced');
    expect(
      rowEl(container, 'machine.sound')
        ?.querySelector('[role="switch"]')
        ?.getAttribute('aria-checked'),
    ).toBe('true');
    expect(rowEl(container, 'machine.model')?.classList.contains('readonly')).toBe(true);

    await open(container, 'machine.cpu');
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x1f'));
    expect(valueText(container, 'machine.cpu.count')).toBe('42');
    // Advanced members wait for the toggle.
    expect(rowEl(container, 'machine.cpu.sr')).toBeNull();
    await fireEvent.click(container.querySelector('.adv-toggle input') as HTMLElement);
    await waitFor(() => expect(valueText(container, 'machine.cpu.sr')).toBe('0x2700'));
  });
});

describe('SystemView editing', () => {
  async function editD0(c: HTMLElement, text: string): Promise<HTMLInputElement> {
    await waitFor(() => expect(valueText(c, 'machine.cpu.d0')).not.toBeNull());
    await fireEvent.dblClick(rowEl(c, 'machine.cpu.d0')!.querySelector('.value') as HTMLElement);
    const input = await waitFor(() => {
      const i = rowEl(c, 'machine.cpu.d0')?.querySelector('.value input');
      expect(i).toBeTruthy();
      return i as HTMLInputElement;
    });
    await fireEvent.input(input, { target: { value: text } });
    await fireEvent.keyDown(input, { key: 'Enter' });
    return input;
  }

  it('writes a literal through the bridge, echoes it and shows the new value', async () => {
    systemView.expanded = { machine: true, 'machine.cpu': true };
    const { container } = render(SystemView);
    await editD0(container, '0x1234');
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x1234'));
    expect(calls).toContainEqual(['machine.cpu.d0', [0x1234]]);
    expect(echo).toHaveBeenCalledWith('machine.cpu.d0 = 0x1234');
    expect(submit).not.toHaveBeenCalled();
  });

  it('runs an expression, or an integer above 2^53, as a console statement', async () => {
    systemView.expanded = { machine: true, 'machine.cpu': true };
    const { container } = render(SystemView);
    await editD0(container, '$a5 + 0x10');
    expect(submit).toHaveBeenCalledWith('machine.cpu.d0 = $a5 + 0x10');
    await editD0(container, '0x20000000000001');
    expect(submit).toHaveBeenLastCalledWith('machine.cpu.d0 = 0x20000000000001');
    expect(echo).not.toHaveBeenCalled();
  });

  it('shows an error under the field and reverts', async () => {
    systemView.expanded = { machine: true, 'machine.cpu': true };
    const { container } = render(SystemView);
    const input = await editD0(container, '666');
    await waitFor(() =>
      expect(rowEl(container, 'machine.cpu.d0')?.querySelector('.error')?.textContent).toBe(
        'd0: not today',
      ),
    );
    expect(input.value).toBe('0x1f');
    expect(echo).not.toHaveBeenCalled();
  });

  it('a bool toggles with one click; an enum edits with a dropdown', async () => {
    systemView.expanded = { machine: true };
    const { container } = render(SystemView);
    const sw = await waitFor(() => {
      const s = rowEl(container, 'machine.sound')?.querySelector('[role="switch"]');
      expect(s).toBeTruthy();
      return s as HTMLElement;
    });
    await fireEvent.click(sw);
    await waitFor(() => expect(calls).toContainEqual(['machine.sound', [false]]));
    expect(echo).toHaveBeenCalledWith('machine.sound = false');

    await fireEvent.dblClick(
      rowEl(container, 'machine.mode')!.querySelector('.value') as HTMLElement,
    );
    const sel = await waitFor(() => {
      const s = rowEl(container, 'machine.mode')?.querySelector('select');
      expect(s).toBeTruthy();
      return s as HTMLSelectElement;
    });
    await fireEvent.change(sel, { target: { value: 'turbo' } });
    await waitFor(() => expect(calls).toContainEqual(['machine.mode', ['turbo']]));
    expect(echo).toHaveBeenCalledWith('machine.mode = "turbo"');
  });

  it('a read-only value has no editor', async () => {
    systemView.expanded = { machine: true };
    const { container } = render(SystemView);
    await waitFor(() => expect(valueText(container, 'machine.model')).toBe('se30'));
    await fireEvent.dblClick(
      rowEl(container, 'machine.model')!.querySelector('.value') as HTMLElement,
    );
    expect(rowEl(container, 'machine.model')?.querySelector('input')).toBeNull();
  });
});

describe('SystemView refresh', () => {
  const reads = () => calls.filter(([p]) => p === 'machine.cpu.meta.members').length;

  it('re-reads open levels on a state event', async () => {
    systemView.expanded = { machine: true, 'machine.cpu': true };
    const { container } = render(SystemView);
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x1f'));
    const before = reads();
    d0 = '0x99';
    for (const l of listeners) l({ kind: 'state', event: 'mode_ended', data: {} });
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x99'));
    expect(reads()).toBeGreaterThan(before);
  });

  it('polls every 2 s while the machine runs, not while stopped', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    systemView.expanded = { machine: true, 'machine.cpu': true };
    machine.status = 'stopped' as typeof machine.status;
    const { container } = render(SystemView);
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x1f'));
    const idle = reads();
    await vi.advanceTimersByTimeAsync(4100);
    expect(reads()).toBe(idle);
    machine.status = 'running' as typeof machine.status;
    await vi.advanceTimersByTimeAsync(100);
    const start = reads();
    await vi.advanceTimersByTimeAsync(4100);
    expect(reads()).toBeGreaterThanOrEqual(start + 2);
    machine.status = 'stopped' as typeof machine.status;
  });
});

describe('SystemView menus', () => {
  it('a destructive method confirms, then runs and echoes', async () => {
    const { container } = render(SystemView);
    await waitFor(() => expect(rowEl(container, 'machine')).not.toBeNull());
    await fireEvent.contextMenu(
      rowEl(container, 'machine')!.querySelector('.sys-line') as HTMLElement,
    );
    await fireEvent.click(await waitFor(() => menuItem('Reset')));
    const dialog = await waitFor(() => {
      const d = document.querySelector('[role="dialog"]');
      expect(d).not.toBeNull();
      return d as HTMLElement;
    });
    expect(dialog.textContent).toContain('Reset machine?');
    expect(calls.find(([p]) => p === 'machine.reset')).toBeUndefined();
    const confirm = Array.from(
      document.querySelectorAll<HTMLElement>('.modal-actions button'),
    ).find((b) => b.textContent === 'Reset')!;
    await fireEvent.click(confirm);
    await waitFor(() => expect(calls).toContainEqual(['machine.reset', []]));
    expect(echo).toHaveBeenCalledWith('machine.reset');
  });

  it("a methods-only child's methods are a submenu of its parent", async () => {
    const { container } = render(SystemView);
    await waitFor(() => expect(rowEl(container, 'machine')).not.toBeNull());
    await fireEvent.contextMenu(
      rowEl(container, 'machine')!.querySelector('.sys-line') as HTMLElement,
    );
    await waitFor(() => expect(menuItem('memory ▸ Peek…')).toBeTruthy());
  });

  it('a method with arguments opens a form and calls with typed values', async () => {
    const { container } = render(SystemView);
    await waitFor(() => expect(rowEl(container, 'files')).not.toBeNull());
    await fireEvent.contextMenu(
      rowEl(container, 'files')!.querySelector('.sys-line') as HTMLElement,
    );
    await fireEvent.click(await waitFor(() => menuItem('Create…')));
    const pathInput = await waitFor(() => {
      const i = document.querySelector('[role="dialog"] input[aria-label="path"]');
      expect(i).toBeTruthy();
      return i as HTMLInputElement;
    });
    await fireEvent.input(pathInput, { target: { value: '/opfs/a.img' } });
    const size = document.querySelector(
      '[role="dialog"] input[aria-label="size"]',
    ) as HTMLInputElement;
    await fireEvent.input(size, { target: { value: '0x100' } });
    const go = Array.from(document.querySelectorAll<HTMLElement>('.modal-actions button')).find(
      (b) => b.textContent === 'Create',
    )!;
    await fireEvent.click(go);
    await waitFor(() => expect(calls).toContainEqual(['files.create', ['/opfs/a.img', 256]]));
    expect(echo).toHaveBeenCalledWith('files.create "/opfs/a.img" 256');
  });

  it('Copy value and Copy path', async () => {
    systemView.expanded = { machine: true, 'machine.cpu': true };
    const { container } = render(SystemView);
    await waitFor(() => expect(valueText(container, 'machine.cpu.d0')).toBe('0x1f'));
    const line = rowEl(container, 'machine.cpu.d0')!.querySelector('.sys-line') as HTMLElement;
    await fireEvent.contextMenu(line);
    await fireEvent.click(await waitFor(() => menuItem('Copy value')));
    await fireEvent.contextMenu(line);
    await fireEvent.click(await waitFor(() => menuItem('Copy path')));
    await waitFor(() => expect(clip).toEqual(['0x1f', 'machine.cpu.d0']));
  });
});

describe('SystemView reveal', () => {
  it('revealInSystem opens the tab at a node, levels above it open and the row selected', async () => {
    revealInSystem('machine.cpu.d0');
    expect(layout.activeTab).toBe('machine');
    const { container } = render(SystemView);
    await waitFor(() =>
      expect(rowEl(container, 'machine.cpu.d0')?.classList.contains('selected')).toBe(true),
    );
    expect(systemView.reveal).toBe('');
  });
});
