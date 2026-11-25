import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());
const downloadFiles = vi.fn(async (paths: string[]) => ({
  total: paths.length,
  failures: [] as string[],
  firstError: '',
  forkless: [] as string[],
}));
vi.mock('@/bus/fsOps', () => ({ downloadFiles: (p: string[]) => downloadFiles(p) }));

const { recorder, applyRecorderEvent, formatRecordedTime, _resetRecorderForTests } =
  await import('@/state/recorder.svelte');
const { startRecording, stopRecording, offerRecording, recordingActions, setRecorderRate } =
  await import('@/bus/recorder');
const { setRecordingFinishedHandler } = await import('@/state/recorder.svelte');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const { storageHealth } = await import('@/state/storageHealth.svelte');
const { filesystem } = await import('@/state/filesystem.svelte');
const { layout } = await import('@/state/layout.svelte');

// The core's recorder event, as record.c writes it.
function ev(state: string, extra: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    event: 'recorder',
    state,
    path: '/opfs/recordings/plus-20261125-120000.mkv',
    segment: 1,
    frames: 0,
    seconds: 0,
    bytes: 0,
    dropped: 0,
    external: false,
    ...extra,
  };
}

beforeEach(() => {
  _resetRecorderForTests();
  setRecordingFinishedHandler(offerRecording);
  _resetForTests();
  bridge.reset();
  downloadFiles.mockClear();
  storageHealth.volatile = false;
});

describe('the recorder store', () => {
  it('follows a recording from start, through a segment, to stop', () => {
    applyRecorderEvent(ev('started'));
    expect(recorder.active).toBe(true);
    expect(recorder.paths).toEqual(['/opfs/recordings/plus-20261125-120000.mkv']);
    applyRecorderEvent(ev('recording', { frames: 120, seconds: 1.995, bytes: 4096, dropped: 3 }));
    expect(recorder.frames).toBe(120);
    expect(recorder.seconds).toBeCloseTo(1.995);
    expect(recorder.bytes).toBe(4096);
    expect(recorder.dropped).toBe(3);
    // The page was hidden: the file closed; shown again: the next file
    applyRecorderEvent(ev('suspended', { frames: 130 }));
    expect(recorder.suspended).toBe(true);
    expect(recorder.active).toBe(true);
    applyRecorderEvent(
      ev('segment', { path: '/opfs/recordings/plus-20261125-120000-2.mkv', segment: 2 }),
    );
    expect(recorder.suspended).toBe(false);
    expect(recorder.paths).toHaveLength(2);
    applyRecorderEvent(ev('stopped', { path: '/opfs/recordings/plus-20261125-120000-2.mkv' }));
    expect(recorder.active).toBe(false);
    expect(recorder.paths).toHaveLength(2);
  });

  it('a stopped recording is offered with Download and Show in Files', () => {
    applyRecorderEvent(ev('started'));
    applyRecorderEvent(ev('stopped'));
    expect(toasts.active).toHaveLength(1);
    const t = toasts.active[0];
    expect(t.msg).toBe('Recording saved: plus-20261125-120000.mkv');
    expect(t.actions?.map((a) => a.label)).toEqual(['Download', 'Show in Files']);
  });

  it('a stopped event with no recording running offers nothing', () => {
    applyRecorderEvent(ev('stopped'));
    expect(toasts.active).toHaveLength(0);
  });

  it('a failure is said, and what was written is still offered', () => {
    applyRecorderEvent(ev('started'));
    applyRecorderEvent(ev('error', { message: 'write: No space left on device' }));
    expect(recorder.error).toBe('write: No space left on device');
    applyRecorderEvent(ev('stopped'));
    expect(toasts.active.map((t) => t.severity)).toEqual(['error', 'info']);
    expect(toasts.active[0].msg).toContain('No space left on device');
    // The next recording starts clean
    applyRecorderEvent(ev('started'));
    expect(recorder.error).toBe('');
  });

  it('says that a GPU-presented screen is recorded at half rate', () => {
    applyRecorderEvent(ev('recording', { external: true }));
    expect(recorder.external).toBe(true);
  });

  it('formats the recorded time', () => {
    expect(formatRecordedTime(0)).toBe('0:00');
    expect(formatRecordedTime(59.9)).toBe('0:59');
    expect(formatRecordedTime(61)).toBe('1:01');
    expect(formatRecordedTime(3725)).toBe('1:02:05');
  });
});

describe('the recorder bus', () => {
  it('starts with the chosen options, written before recorder.start', async () => {
    bridge.reply('recorder.rate', null).reply('recorder.audio', null);
    bridge.reply('recorder.start', '/opfs/recordings/plus-1.mkv');
    setRecorderRate('half');
    recorder.audio = false;
    expect(await startRecording()).toBe('/opfs/recordings/plus-1.mkv');
    expect(bridge.calls).toEqual([
      { path: 'recorder.rate', args: ['half'] },
      { path: 'recorder.audio', args: [false] },
      { path: 'recorder.start', args: undefined },
    ]);
    expect(toasts.active).toHaveLength(0);
  });

  it('warns before recording onto storage that will not persist', async () => {
    bridge.reply('recorder.rate', null).reply('recorder.audio', null);
    bridge.reply('recorder.start', '/opfs/recordings/plus-1.mkv');
    storageHealth.volatile = true;
    await startRecording();
    expect(toasts.active).toHaveLength(1);
    expect(toasts.active[0].severity).toBe('warning');
    expect(toasts.active[0].msg).toMatch(/not persistent/);
  });

  it('says why the core refused to start', async () => {
    bridge.reply('recorder.rate', null).reply('recorder.audio', null);
    bridge.reply('recorder.start', { error: 'recorder.start: the machine has no screen' });
    expect(await startRecording()).toBeNull();
    expect(toasts.active[0].severity).toBe('error');
    expect(toasts.active[0].msg).toContain('the machine has no screen');
  });

  it('options do not change while a recording runs', () => {
    recorder.active = true;
    setRecorderRate('quarter');
    expect(recorder.rate).toBe('full');
  });

  it('stop returns the files written', async () => {
    bridge.reply('recorder.stop', ['/opfs/recordings/a.mkv', '/opfs/recordings/a-2.mkv']);
    expect(await stopRecording()).toEqual(['/opfs/recordings/a.mkv', '/opfs/recordings/a-2.mkv']);
  });

  it('Download saves every file; Show in Files reveals the last', async () => {
    const paths = ['/opfs/recordings/a.mkv', '/opfs/recordings/a-2.mkv'];
    const [download, show] = recordingActions(paths);
    expect(download.label).toBe('Download (2 files)');
    download.action();
    await Promise.resolve();
    expect(downloadFiles).toHaveBeenCalledWith(paths);
    show.action();
    expect(layout.activeTab).toBe('filesystem');
    expect(filesystem.reveal).toBe('/opfs/recordings/a-2.mkv');
    expect(filesystem.expanded['/opfs\x1f/opfs/recordings']).toBe(true);
  });
});
