// Welcome's "Open Checkpoint...": a picked file that carries the checkpoint
// signature is staged through the transfer window and loaded, then the staging
// copy removed; anything else is refused by name without touching the core.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { bridge } from '../helpers/bridgeMock';

const heap = { u8: new Uint8Array(1 << 16), i16: new Int16Array(1), i32: new Int32Array(1) };

vi.mock('@/bus/emulator', async () => ({
  ...(await (await import('../helpers/bridgeMock')).emulatorModule()),
  getModuleHeap: () => heap,
}));
vi.mock('@/bus/boot', () => ({
  reconcileUiWithMachine: vi.fn(async () => {}),
  prepareFreshMachine: vi.fn(async () => {}),
}));

const { pickAndLoadCheckpoint } = await import('@/bus/upload');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const { reconcileUiWithMachine } = await import('@/bus/boot');

// jsdom's Blob has no arrayBuffer(); the browser's does.
if (!Blob.prototype.arrayBuffer) {
  Blob.prototype.arrayBuffer = function (this: Blob) {
    return new Promise<ArrayBuffer>((resolve) => {
      const r = new FileReader();
      r.onload = () => resolve(r.result as ArrayBuffer);
      r.readAsArrayBuffer(this);
    });
  };
}

// The OS dialog, answered with `file` as soon as the picker opens it.
function pickerAnswers(file: File): void {
  vi.spyOn(HTMLInputElement.prototype, 'click').mockImplementation(function (
    this: HTMLInputElement,
  ) {
    Object.defineProperty(this, 'files', { value: [file] });
    this.dispatchEvent(new Event('change'));
  });
}

const written: string[] = [];

beforeEach(() => {
  bridge.reset();
  _resetForTests();
  written.length = 0;
  bridge.reply('storage.xfer_buffer', 1024);
  bridge.reply('storage.xfer_size', 4096);
  bridge.reply('storage.xfer_write', (args: unknown) => {
    written.push((args as [string])[0]);
    return true;
  });
  bridge.reply('checkpoint.load', true);
  bridge.reply('storage.rm', true);
});
afterEach(() => vi.restoreAllMocks());

const checkpoint = () =>
  new File([new TextEncoder().encode('GSCHKPT3'), new Uint8Array(100)], 'saved-state-1.bin');

describe('Open Checkpoint...', () => {
  it('stages the file, loads it, removes the staging copy', async () => {
    pickerAnswers(checkpoint());
    await pickAndLoadCheckpoint();
    const staged = written[0];
    expect(staged).toMatch(/^\/opfs\/upload\/.*saved-state-1\.bin$/);
    const load = bridge.calls.find((c) => c.path === 'checkpoint.load');
    expect(load?.args).toEqual([staged]);
    expect(bridge.calls.find((c) => c.path === 'storage.rm')?.args).toEqual([staged]);
    expect(reconcileUiWithMachine).toHaveBeenCalledWith('restore');
    expect(toasts.active.some((t) => /Checkpoint loaded/.test(t.msg))).toBe(true);
  });

  it('refuses a file that is not a checkpoint, before touching the core', async () => {
    pickerAnswers(new File([new Uint8Array(64)], 'plus.rom'));
    await pickAndLoadCheckpoint();
    expect(bridge.paths()).toEqual([]);
    expect(toasts.active.some((t) => /not a Granny Smith checkpoint/.test(t.msg))).toBe(true);
  });

  it('a failed load says so', async () => {
    bridge.reply('checkpoint.load', { error: 'wrong machine' });
    pickerAnswers(checkpoint());
    await pickAndLoadCheckpoint();
    expect(toasts.active.some((t) => /Checkpoint load failed/.test(t.msg))).toBe(true);
  });
});
