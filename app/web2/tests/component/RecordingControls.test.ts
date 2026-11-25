import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';
import { flushSync } from 'svelte';

// The toolbar calls the recorder through the bus; only those calls are faked.
const startRecording = vi.fn(async () => '/opfs/recordings/plus-1.mkv');
const stopRecording = vi.fn(async () => ['/opfs/recordings/plus-1.mkv']);
vi.mock('@/bus/recorder', async () => {
  const actual = await vi.importActual<Record<string, unknown>>('@/bus/recorder');
  return {
    ...actual,
    startRecording: () => startRecording(),
    stopRecording: () => stopRecording(),
  };
});

const DisplayToolbar = (await import('@/components/display/DisplayToolbar.svelte')).default;
const StatusBar = (await import('@/components/status-bar/StatusBar.svelte')).default;
const Toast = (await import('@/components/common/Toast.svelte')).default;
const { machine } = await import('@/state/machine.svelte');
const { recorder, _resetRecorderForTests } = await import('@/state/recorder.svelte');
const { closeContextMenu } = await import('@/components/common/ContextMenu.svelte');
const { toasts, showNotification, _resetForTests } = await import('@/state/toasts.svelte');

beforeEach(() => {
  _resetRecorderForTests();
  _resetForTests();
  startRecording.mockClear();
  stopRecording.mockClear();
  machine.status = 'running';
  machine.model = 'Macintosh Plus';
  machine.ram = '4 MB';
});

const menuLabels = () =>
  Array.from(document.querySelectorAll('.context-menu .item')).map((i) => i.textContent?.trim());

describe('the record button', () => {
  it('is disabled with no machine', () => {
    machine.status = 'no-machine';
    const { container } = render(DisplayToolbar);
    expect((container.querySelector('.record-btn') as HTMLButtonElement).disabled).toBe(true);
  });

  it('opens a menu that starts a recording, with the rate and sound options', async () => {
    const { container } = render(DisplayToolbar);
    const btn = container.querySelector('.record-btn') as HTMLButtonElement;
    expect(btn.getAttribute('aria-label')).toBe('Record the screen and sound');
    await fireEvent.click(btn);
    expect(menuLabels()).toEqual([
      'Start Recording',
      'Full Rate (60 fps)',
      'Half Rate (30 fps)',
      'Quarter Rate (15 fps)',
      'Record Sound',
    ]);
    const items = Array.from(document.querySelectorAll('.context-menu .item'));
    await fireEvent.click(items[2] as HTMLElement);
    expect(recorder.rate).toBe('half');
    await fireEvent.click(btn);
    await fireEvent.click(
      Array.from(document.querySelectorAll('.context-menu .item'))[0] as HTMLElement,
    );
    expect(startRecording).toHaveBeenCalledOnce();
    closeContextMenu();
  });

  it('is a red stop button while recording, and stops with one click', async () => {
    recorder.active = true;
    const { container } = render(DisplayToolbar);
    const btn = container.querySelector('.record-btn') as HTMLButtonElement;
    expect(btn.hasAttribute('data-recording')).toBe(true);
    expect(btn.querySelector('use')?.getAttribute('href')).toContain('#i-stop');
    expect(btn.getAttribute('aria-label')).toBe('Stop recording');
    await fireEvent.click(btn);
    expect(stopRecording).toHaveBeenCalledOnce();
    expect(document.querySelector('.context-menu')).toBeNull();
  });

  it('its options are disabled while recording', async () => {
    recorder.active = true;
    const { container } = render(DisplayToolbar);
    await fireEvent.contextMenu(container.querySelector('.record-btn') as HTMLElement);
    expect(menuLabels()[0]).toBe('Stop Recording');
    const items = Array.from(document.querySelectorAll('.context-menu .item'));
    const disabled = items.map((i) => i.getAttribute('aria-disabled') === 'true');
    expect(disabled).toEqual([false, true, true, true, true]);
    closeContextMenu();
  });
});

describe('the recording indicator', () => {
  it('is absent when nothing records', () => {
    const { queryByTestId } = render(StatusBar);
    expect(queryByTestId('recording')).toBeNull();
  });

  it('shows the recorded time and size, and the dropped frames only when there are some', () => {
    Object.assign(recorder, { active: true, seconds: 75.2, bytes: 2.5 * 1024 * 1024, dropped: 0 });
    const { getByTestId } = render(StatusBar);
    const el = getByTestId('recording');
    expect(el.querySelector('.rec-time')?.textContent).toBe('1:15');
    expect(el.querySelector('.rec-size')?.textContent).toBe('2.5 MB');
    expect(el.querySelector('.rec-dropped')).toBeNull();
    expect(el.querySelector('.rec-paused')).toBeNull();
    recorder.dropped = 4;
    flushSync();
    expect(el.querySelector('.rec-dropped')?.textContent).toBe('4 dropped');
  });

  it('says paused while the machine is paused', () => {
    recorder.active = true;
    machine.status = 'paused';
    const { getByTestId } = render(StatusBar);
    expect(getByTestId('recording').querySelector('.rec-paused')?.textContent).toBe('paused');
  });
});

describe('a toast with actions', () => {
  it('runs the action and dismisses the toast', async () => {
    const act = vi.fn();
    showNotification('Recording saved: a.mkv', 'info', [{ label: 'Download', action: act }]);
    const { container } = render(Toast, { toast: toasts.active[0] });
    const b = container.querySelector('.toast-action') as HTMLButtonElement;
    expect(b.textContent?.trim()).toBe('Download');
    await fireEvent.click(b);
    expect(act).toHaveBeenCalledOnce();
    expect(toasts.active).toHaveLength(0);
  });
});
