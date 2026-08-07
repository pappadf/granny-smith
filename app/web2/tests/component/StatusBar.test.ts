import { render } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import StatusBar from '@/components/status-bar/StatusBar.svelte';
import { machine, type MachineStatus } from '@/state/machine.svelte';

beforeEach(() => {
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
});

describe('StatusBar', () => {
  it('renders nothing when machine is in no-machine state', () => {
    const { container } = render(StatusBar);
    expect(container.querySelector('.gs-statusbar')).toBeNull();
  });

  it.each([
    ['running', 'Running'],
    ['paused', 'Paused'],
    ['stopped', 'Stopped'],
  ] as Array<[MachineStatus, string]>)('shows %s state with label %s', (status, label) => {
    machine.status = status;
    const { container } = render(StatusBar);
    const bar = container.querySelector('.gs-statusbar') as HTMLElement;
    expect(bar).not.toBeNull();
    expect(bar.classList.contains(status)).toBe(true);
    expect(bar.querySelector('.sb-state .label')?.textContent).toBe(label);
  });

  it('shows machine description (model · ram) on the right', () => {
    machine.status = 'running';
    machine.model = 'Macintosh SE/30';
    machine.ram = '8 MB';
    const { container } = render(StatusBar);
    expect(container.querySelector('.sb-desc')?.textContent).toBe('SE/30 · 8 MB');
  });

  it('renders the drive indicators plus the checkpoint glyph (HD, FD, CD, CP)', () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    const driveLabels = Array.from(container.querySelectorAll('.sb-drive .drive-ico')).map(
      (d) => d.textContent,
    );
    expect(driveLabels).toEqual(['HD', 'FD', 'CD', 'CP']);
  });

  it('checkpoint glyph tooltip carries the last save time and duration', () => {
    machine.status = 'running';
    machine.checkpoint = { at: Date.now(), ms: 12.34 };
    const { container } = render(StatusBar);
    const cp = Array.from(container.querySelectorAll('.sb-drive')).find(
      (d) => d.querySelector('.drive-ico')?.textContent === 'CP',
    );
    expect(cp?.getAttribute('title')).toMatch(/Last background checkpoint .+\(12\.3 ms\)/);
  });
});
