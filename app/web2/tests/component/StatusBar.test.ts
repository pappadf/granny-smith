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

  it('renders three drive indicators (HD, FD, CD)', () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    const driveLabels = Array.from(container.querySelectorAll('.sb-drive .drive-ico')).map(
      (d) => d.textContent,
    );
    expect(driveLabels).toEqual(['HD', 'FD', 'CD']);
  });
});
