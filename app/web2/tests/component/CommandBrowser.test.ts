import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import CommandBrowser from '@/components/panel-views/terminal/CommandBrowser.svelte';
import { registerTerminalInsert } from '@/components/panel-views/terminal/terminalBridge';

// The browser is generated from the model, so mock the
// bus to feed buildCommandsTree a tiny tree: one subsystem (`cpu`) with a
// `step` method. Categories are model-derived (subsystem bucket + the static
// Language keywords group), not a hand-listed catalogue.
vi.mock('@/bus/emulator', () => {
  const method = (name: string, doc = '') => ({
    name,
    kind: 'method',
    category: 'basic',
    label: name,
    doc,
    verb: name,
    task: '',
    destructive: false,
    mutate: false,
    hidden: false,
    nargs: 0,
  });
  const child = (name: string) => ({
    name,
    kind: 'child',
    category: 'basic',
    label: name,
    doc: '',
  });
  return {
    isModuleReady: () => true,
    gsEval: async (path: string) => {
      // No root verbs in this fixture; one child, cpu, with a `step` method.
      if (path === 'meta.members') return [child('cpu')];
      if (path === 'cpu.meta.members') return [method('step', 'run N instructions')];
      if (path === 'shell.aliases') return [];
      return null;
    },
  };
});

// machine.status is read by the rebuild effect.
vi.mock('@/state/machine.svelte', () => ({ machine: { status: 'idle' } }));

beforeEach(() => {
  registerTerminalInsert(null);
});

describe('CommandBrowser (model-generated)', () => {
  it('renders model-derived top-level categories', async () => {
    const { container } = render(CommandBrowser);
    await waitFor(() => {
      const names = Array.from(
        container.querySelectorAll('.cmd-row.category > .cmd-line > .name'),
      ).map((c) => c.textContent);
      expect(names).toContain('cpu'); // subsystem bucket
      expect(names).toContain('Language'); // shell keywords group
    });
  });

  it('clicking a category expands it and reveals its commands', async () => {
    const { container } = render(CommandBrowser);
    const cpuCat = await waitFor(() => {
      const el = Array.from(container.querySelectorAll('.cmd-row.category > .cmd-line')).find((l) =>
        l.textContent?.includes('cpu'),
      ) as HTMLElement | undefined;
      if (!el) throw new Error('cpu category not rendered yet');
      return el;
    });
    await fireEvent.click(cpuCat);
    await waitFor(() => {
      const leaf = Array.from(container.querySelectorAll('.cmd-line .name')).some(
        (e) => e.textContent === 'cpu.step',
      );
      expect(leaf).toBe(true);
    });
  });

  it('clicking a leaf row writes its insert text through terminalBridge', async () => {
    const setter = vi.fn();
    registerTerminalInsert(setter);
    const { container } = render(CommandBrowser);
    const cpuCat = await waitFor(() => {
      const el = Array.from(container.querySelectorAll('.cmd-row.category > .cmd-line')).find((l) =>
        l.textContent?.includes('cpu'),
      ) as HTMLElement | undefined;
      if (!el) throw new Error('cpu category not rendered yet');
      return el;
    });
    await fireEvent.click(cpuCat);
    const stepLine = await waitFor(() => {
      const el = Array.from(container.querySelectorAll('.cmd-line')).find(
        (line) => line.querySelector('.name')?.textContent === 'cpu.step',
      ) as HTMLElement | undefined;
      if (!el) throw new Error('cpu.step row not rendered yet');
      return el;
    });
    await fireEvent.click(stepLine);
    expect(setter).toHaveBeenCalledWith('cpu.step');
  });
});
