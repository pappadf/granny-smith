import { render } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import StatusBar from '@/components/status-bar/StatusBar.svelte';
import { tick } from 'svelte';
import { machine, setDriveActivity, type MachineStatus } from '@/state/machine.svelte';
import {
  printer,
  setPrinterStatus,
  showPrintedDocument,
  _resetPrinterForTests,
} from '@/state/printer.svelte';

beforeEach(() => {
  machine.status = 'no-machine';
  machine.model = null;
  machine.ram = null;
  machine.drives = { hd: true, fd: true, cd: true };
  machine.driveActivity = { hd: 'idle', fd: 'idle', cd: 'idle' };
  (URL as unknown as { createObjectURL: unknown }).createObjectURL = () => 'blob:the-pdf';
  (URL as unknown as { revokeObjectURL: unknown }).revokeObjectURL = () => undefined;
  _resetPrinterForTests();
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

  it('shows only the lights the model has (no CD light on a Plus)', () => {
    machine.status = 'running';
    machine.drives = { hd: true, fd: true, cd: false };
    const { container } = render(StatusBar);
    const driveLabels = Array.from(container.querySelectorAll('.sb-drive .drive-ico')).map(
      (d) => d.textContent,
    );
    expect(driveLabels).toEqual(['HD', 'FD', 'CP']);
  });

  it('a core push lights a drive and puts it out again', async () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    const fd = () =>
      Array.from(container.querySelectorAll('.sb-drive')).find(
        (d) => d.querySelector('.drive-ico')?.textContent === 'FD',
      ) as HTMLElement;
    const before = fd().className;
    setDriveActivity(1, 2); // fd, write
    await tick();
    expect(machine.driveActivity.fd).toBe('write');
    expect(fd().className).not.toBe(before);
    setDriveActivity(1, 0);
    await tick();
    expect(fd().className).toBe(before);
    setDriveActivity(7, 1); // an unknown kind is ignored
    expect(machine.driveActivity).toEqual({ hd: 'idle', fd: 'idle', cd: 'idle' });
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

  it('shows what the printer is doing, and nothing while it is idle', async () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    const item = () => container.querySelector('.sb-printer');
    expect(item()).toBeNull();
    setPrinterStatus('status: starting up');
    await tick();
    expect(item()?.textContent).toContain('Printer starting up');
    setPrinterStatus('status: busy; source: AppleTalk; job: MacOS7');
    await tick();
    expect(item()?.textContent).toContain('Printing “MacOS7”');
    expect(item()?.getAttribute('title')).toBe(
      'LaserWriter — status: busy; source: AppleTalk; job: MacOS7',
    );
    setPrinterStatus('status: printing; source: AppleTalk; job: MacOS7; page: 1');
    await tick();
    expect(item()?.textContent).toContain('Printing “MacOS7” · page 1');
    setPrinterStatus('status: idle');
    await tick();
    expect(item()).toBeNull();
  });

  it('a failed job shows why', async () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    setPrinterStatus('status: idle; error: feed failed');
    await tick();
    expect(container.querySelector('.sb-printer')?.textContent).toContain(
      'Print failed: feed failed',
    );
  });

  it('once idle, the last document can be reopened from the bar', async () => {
    machine.status = 'running';
    const { container } = render(StatusBar);
    showPrintedDocument({
      name: '00002-MacOS7.pdf',
      title: 'MacOS7',
      pages: 1,
      pdf: new Uint8Array([0x25]),
    });
    printer.viewerOpen = false;
    await tick();
    const button = container.querySelector('button.sb-printed') as HTMLButtonElement;
    expect(button.textContent).toContain('MacOS7');
    button.click();
    expect(printer.viewerOpen).toBe(true);
  });
});
