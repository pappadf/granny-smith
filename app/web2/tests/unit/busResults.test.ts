import { describe, it, expect, vi, beforeEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { saveCheckpoint } = await import('@/bus/checkpoint');

// Every success message checks its result.
describe('Save State', () => {
  beforeEach(() => bridge.reset());

  it('reports success only when both the save and the download worked', async () => {
    bridge.reply('checkpoint.save', true).reply('download', true).reply('storage.rm', true);
    const res = await saveCheckpoint();
    expect(res.ok).toBe(true);
    if (res.ok) expect(res.name).toMatch(/^saved-state-\d{8}-\d{6}\.bin$/);
  });

  it('reports a failed save and never downloads', async () => {
    bridge.reply('checkpoint.save', false);
    const res = await saveCheckpoint();
    expect(res).toMatchObject({ ok: false, step: 'save' });
    expect(bridge.paths()).not.toContain('download');
  });

  it('reports a failed download', async () => {
    bridge.reply('checkpoint.save', true).reply('download', false).reply('storage.rm', true);
    expect(await saveCheckpoint()).toMatchObject({ ok: false, step: 'download' });
  });

  it('removes the heap-backed /tmp copy after the download', async () => {
    bridge.reply('checkpoint.save', true).reply('download', true).reply('storage.rm', true);
    await saveCheckpoint();
    const saved = bridge.calls.find((c) => c.path === 'checkpoint.save')!.args![0];
    expect(bridge.calls.at(-1)).toEqual({ path: 'storage.rm', args: [saved] });
  });
});
