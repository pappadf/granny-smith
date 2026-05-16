import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import CommandBrowser from '@/components/panel-views/terminal/CommandBrowser.svelte';
import { registerTerminalInsert } from '@/components/panel-views/terminal/terminalBridge';

beforeEach(() => {
  registerTerminalInsert(null);
});

describe('CommandBrowser', () => {
  it('renders every top-level category as a category row', () => {
    const { container } = render(CommandBrowser);
    const cats = Array.from(container.querySelectorAll('.cmd-row.category > .cmd-line > .name'));
    const names = cats.map((c) => c.textContent);
    expect(names).toContain('Scheduler');
    expect(names).toContain('Debugger');
    expect(names).toContain('Logging');
    expect(names).toContain('Storage');
  });

  it('categories start collapsed (no description visible)', () => {
    const { container } = render(CommandBrowser);
    expect(container.querySelector('.desc')).toBeNull();
  });

  it('clicking a category expands it and reveals its commands', async () => {
    const { container } = render(CommandBrowser);
    const schedulerName = Array.from(
      container.querySelectorAll('.cmd-row.category > .cmd-line'),
    ).find((line) => line.textContent?.includes('Scheduler')) as HTMLElement;
    await fireEvent.click(schedulerName);
    // After expansion, the description block appears, plus the `run` row.
    await waitFor(() => {
      expect(container.querySelector('.desc')).not.toBeNull();
      const runLeaf = Array.from(container.querySelectorAll('.cmd-line .name')).some(
        (e) => e.textContent === 'run',
      );
      expect(runLeaf).toBe(true);
    });
  });

  it('clicking a leaf row writes its insert text through terminalBridge', async () => {
    const setter = vi.fn();
    registerTerminalInsert(setter);
    const { container } = render(CommandBrowser);
    const schedulerName = Array.from(
      container.querySelectorAll('.cmd-row.category > .cmd-line'),
    ).find((line) => line.textContent?.includes('Scheduler')) as HTMLElement;
    await fireEvent.click(schedulerName);
    const runLine = await waitFor(() => {
      const el = Array.from(container.querySelectorAll('.cmd-line')).find(
        (line) => line.querySelector('.name')?.textContent === 'run',
      ) as HTMLElement | undefined;
      if (!el) throw new Error('run row not rendered yet');
      return el;
    });
    await fireEvent.click(runLine);
    expect(setter).toHaveBeenCalledWith('run');
  });
});
