import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { saveCheckpoint } = await import('@/bus/checkpoint');

// Every success message checks its result.
describe('Save State', () => {
  beforeEach(() => bridge.reset());

  it('reports success only when both the save and the download worked', async () => {
    bridge.reply('checkpoint.save', true).reply('files.download', true).reply('files.rm', true);
    const res = await saveCheckpoint();
    expect(res.ok).toBe(true);
    if (res.ok) expect(res.name).toMatch(/^saved-state-\d{8}-\d{6}\.bin$/);
  });

  it('reports a failed save and never downloads', async () => {
    bridge.reply('checkpoint.save', false);
    const res = await saveCheckpoint();
    expect(res).toMatchObject({ ok: false, step: 'save' });
    expect(bridge.paths()).not.toContain('files.download');
  });

  it('reports a failed download', async () => {
    bridge.reply('checkpoint.save', true).reply('files.download', false).reply('files.rm', true);
    expect(await saveCheckpoint()).toMatchObject({ ok: false, step: 'save to computer' });
  });

  it('saves in a scratch directory of its own, under the name it downloads as', async () => {
    bridge.reply('checkpoint.save', true).reply('files.download', true).reply('files.rm', true);
    const res = await saveCheckpoint();
    const saved = bridge.calls.find((c) => c.path === 'checkpoint.save')!.args![0] as string;
    const dir = saved.slice(0, saved.lastIndexOf('/'));
    expect(dir.startsWith('/opfs/upload/.scratch/')).toBe(true);
    expect(res.ok && saved.endsWith(`/${res.name}`)).toBe(true);
    expect(bridge.calls.find((c) => c.path === 'files.mkdir')?.args).toEqual([dir]);
    expect(bridge.calls.at(-1)).toEqual({ path: 'files.rm', args: [dir] });
  });

  it('removes the scratch copy on every exit, a failed save included', async () => {
    bridge.reply('checkpoint.save', false).reply('files.rm', true);
    await saveCheckpoint();
    expect(bridge.calls.at(-1)?.path).toBe('files.rm');
    bridge.reset();
    bridge.reply('checkpoint.save', true).reply('files.download', false).reply('files.rm', true);
    await saveCheckpoint();
    expect(bridge.calls.at(-1)?.path).toBe('files.rm');
  });
});
