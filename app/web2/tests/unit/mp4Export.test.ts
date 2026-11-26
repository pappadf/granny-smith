// The MP4 export's page side (src/recorder/mp4Export.ts) and its dialog
// (components/dialogs/ExportMp4Dialog.svelte) against a stand-in worker:
// the dialog's steps (reading, the size choice, progress, done), the jobs
// it sends (each .mkv to its .mp4, a scratch file for the samples),
// Cancel, a failure, the finished file offered, and the refusal where the
// browser cannot encode H.264.

import { describe, it, expect, beforeEach, vi } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/svelte';
import { tick } from 'svelte';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());
const downloadFiles = vi.fn(async (paths: string[]) => ({
  total: paths.length,
  failures: [] as string[],
  firstError: '',
  forkless: [] as string[],
}));
vi.mock('@/bus/fsOps', () => ({ downloadFiles: (p: string[]) => downloadFiles(p) }));

// The stand-in worker: what the page posts, and a way to answer.
const posted: Record<string, unknown>[] = [];
const workers: { onmessage: ((ev: { data: unknown }) => void) | null }[] = [];
class FakeWorker {
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onerror: unknown = null;
  constructor() {
    workers.push(this);
  }
  postMessage(m: Record<string, unknown>) {
    posted.push(m);
  }
  terminate() {}
}
(globalThis as unknown as { Worker: unknown }).Worker = FakeWorker;
const reply = (data: unknown) => workers[workers.length - 1].onmessage!({ data });

const { openExportDialog, startExport, cancelExport, mp4PathFor, _resetMp4ExportWorkerForTests } =
  await import('@/recorder/mp4Export');
const { mp4Export, _resetMp4ExportForTests } = await import('@/state/mp4Export.svelte');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const { _resetExportSupportForTests } = await import('@/recorder/probe');
const { recorder } = await import('@/state/recorder.svelte');
const ExportMp4Dialog = (await import('@/components/dialogs/ExportMp4Dialog.svelte')).default;

const info = (path: string, audioRate = 22255) => ({
  path,
  info: {
    width: 512,
    height: 342,
    displayWidth: 512,
    displayHeight: 342,
    periodNs: 16625103,
    frames: 600,
    audioRate,
    audioChannels: 1,
    truncated: false,
  },
  sizes: [1, 2, 3, 4].map((scale) => ({
    scale,
    width: 512 * scale,
    height: 342 * scale,
    ok: scale < 4,
  })),
  audio: 'opus',
});

// WebCodecs as a browser that has H.264 (and Opus, not AAC) would answer.
function withEncoders(on: boolean) {
  const g = globalThis as unknown as Record<string, unknown>;
  if (on) {
    g.VideoEncoder = { isConfigSupported: async () => ({ supported: true }) };
    g.AudioEncoder = {
      isConfigSupported: async (c: { codec: string }) => ({ supported: c.codec === 'opus' }),
    };
  } else {
    delete g.VideoEncoder;
    delete g.AudioEncoder;
  }
}

beforeEach(() => {
  _resetMp4ExportForTests();
  _resetMp4ExportWorkerForTests();
  _resetExportSupportForTests();
  _resetForTests();
  posted.length = 0;
  workers.length = 0;
  downloadFiles.mockClear();
  withEncoders(true);
});

describe('MP4 export, page side', () => {
  it('names the .mp4 after the .mkv', () => {
    expect(mp4PathFor('/opfs/recordings/plus-1.mkv')).toBe('/opfs/recordings/plus-1.mp4');
    expect(mp4PathFor('/opfs/recordings/plus-1.MKV')).toBe('/opfs/recordings/plus-1.mp4');
  });

  it('reads the recording, offers the sizes, exports, and offers the result', async () => {
    render(ExportMp4Dialog);
    await openExportDialog(['/opfs/recordings/plus-1.mkv']);
    expect(mp4Export.phase).toBe('loading');
    expect(posted[0]).toMatchObject({ type: 'info', paths: ['/opfs/recordings/plus-1.mkv'] });
    expect(String(posted[0].moduleUrl)).toMatch(/recdec\.js\?v=/);
    reply({ type: 'info', files: [info('/opfs/recordings/plus-1.mkv')] });
    await tick();
    expect(mp4Export.phase).toBe('choose');
    expect(mp4Export.scale).toBe(1);
    // The sizes, the one this browser cannot encode disabled; the sound it will get
    expect(screen.getByLabelText(/1× — 512 × 342/)).toBeChecked();
    expect(screen.getByLabelText(/4× — 2048 × 1368/)).toBeDisabled();
    expect(screen.getByText(/Sound: Opus/)).toBeInTheDocument();
    expect(screen.getByText(/plus-1\.mp4 beside the recording/)).toBeInTheDocument();
    await fireEvent.click(screen.getByLabelText(/2× — 1024 × 684/));
    expect(mp4Export.scale).toBe(2);

    await fireEvent.click(screen.getByRole('button', { name: 'Export' }));
    expect(mp4Export.phase).toBe('running');
    const job = posted[1] as {
      type: string;
      scale: number;
      jobs: { src: string; dst: string; tmp: string }[];
    };
    expect(job.type).toBe('export');
    expect(job.scale).toBe(2);
    expect(job.jobs[0].src).toBe('/opfs/recordings/plus-1.mkv');
    expect(job.jobs[0].dst).toBe('/opfs/recordings/plus-1.mp4');
    expect(job.jobs[0].tmp).toMatch(/^\/opfs\/upload\/\.scratch\/.+plus-1\.mkv\.mdat$/);

    reply({ type: 'progress', file: 0, files: 1, done: 150, total: 600 });
    await tick();
    expect(screen.getByRole('progressbar')).toHaveAttribute('aria-valuenow', '25');
    expect(screen.getByText(/Frame 150 of 600/)).toBeInTheDocument();

    reply({
      type: 'done',
      outputs: [
        {
          src: job.jobs[0].src,
          dst: job.jobs[0].dst,
          bytes: 1,
          frames: 600,
          width: 1024,
          height: 684,
          audio: 'opus',
          hadAudio: true,
        },
      ],
    });
    await tick();
    expect(mp4Export.open).toBe(false);
    expect(toasts.active[0].msg).toBe('Exported: plus-1.mp4');
    const [download] = toasts.active[0].actions!;
    download.action();
    await Promise.resolve();
    expect(downloadFiles).toHaveBeenCalledWith(['/opfs/recordings/plus-1.mp4']);
  });

  it('says when the sound is left out', async () => {
    await openExportDialog(['/opfs/recordings/a.mkv']);
    reply({ type: 'info', files: [info('/opfs/recordings/a.mkv')] });
    startExport();
    reply({
      type: 'done',
      outputs: [
        {
          src: 'a',
          dst: '/opfs/recordings/a.mp4',
          bytes: 1,
          frames: 1,
          width: 1,
          height: 1,
          audio: null,
          hadAudio: true,
        },
      ],
    });
    expect(toasts.active[0].msg).toMatch(/video only/);
  });

  it('exports each file of a recording that spans several', async () => {
    const paths = ['/opfs/recordings/a.mkv', '/opfs/recordings/a-2.mkv'];
    await openExportDialog(paths);
    reply({ type: 'info', files: paths.map((p) => info(p)) });
    startExport();
    const job = posted[1] as { jobs: { dst: string }[] };
    expect(job.jobs.map((j) => j.dst)).toEqual([
      '/opfs/recordings/a.mp4',
      '/opfs/recordings/a-2.mp4',
    ]);
  });

  it('Cancel asks the worker to stop; its answer closes the dialog', async () => {
    await openExportDialog(['/opfs/recordings/a.mkv']);
    reply({ type: 'info', files: [info('/opfs/recordings/a.mkv')] });
    startExport();
    cancelExport();
    expect(posted[2]).toEqual({ type: 'cancel' });
    reply({ type: 'cancelled' });
    expect(mp4Export.phase).toBe('idle');
    expect(mp4Export.open).toBe(false);
    expect(toasts.active[0].msg).toBe('MP4 export cancelled');
  });

  it('shows a failure in the dialog', async () => {
    render(ExportMp4Dialog);
    await openExportDialog(['/opfs/recordings/a.mkv']);
    reply({
      type: 'info',
      files: [{ path: '/opfs/recordings/a.mkv', error: 'not a recording', sizes: [], audio: null }],
    });
    await tick();
    expect(mp4Export.phase).toBe('failed');
    expect(screen.getByText(/a\.mkv: not a recording/)).toBeInTheDocument();
  });

  it('will not export the file a recording is still writing', async () => {
    recorder.active = true;
    recorder.path = '/opfs/recordings/a.mkv';
    try {
      await openExportDialog(['/opfs/recordings/a.mkv']);
      expect(mp4Export.open).toBe(false);
      expect(posted).toHaveLength(0);
      expect(toasts.active[0].msg).toMatch(/still being recorded/);
    } finally {
      recorder.active = false;
    }
  });

  it('refuses where the browser cannot encode H.264, without starting the worker', async () => {
    withEncoders(false);
    render(ExportMp4Dialog);
    await openExportDialog(['/opfs/recordings/a.mkv']);
    await tick();
    expect(mp4Export.phase).toBe('failed');
    expect(mp4Export.support?.ok).toBe(false);
    expect(posted).toHaveLength(0);
    expect(screen.getByText(/cannot encode H\.264/)).toBeInTheDocument();
  });
});
