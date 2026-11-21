// The two WebGPU takeover cards answer the page the same way: a worker that
// finds no device says why, and a device lost after startup withdraws the
// GPU from the core (a card created later must not be offered it).
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const setGpuUnavailable = vi.fn();
const attachSharedWorker = vi.fn(() => true);
vi.mock('@/bus/emulator', () => ({
  getModule: () => ({}),
  heapBuffer: () => null,
  attachSharedWorker,
  setGpuUnavailable,
}));
const showNotification = vi.fn();
vi.mock('@/state/toasts.svelte', () => ({ showNotification }));

class FakeWorker {
  static last: FakeWorker | null = null;
  onmessage: ((ev: MessageEvent) => void) | null = null;
  onerror: ((ev: ErrorEvent) => void) | null = null;
  constructor() {
    FakeWorker.last = this;
  }
  postMessage(): void {}
  answer(data: Record<string, unknown>): void {
    this.onmessage?.({ data } as MessageEvent);
  }
}

function overlayCanvas(): HTMLCanvasElement {
  const c = document.createElement('canvas');
  (c as unknown as { transferControlToOffscreen: () => unknown }).transferControlToOffscreen =
    () => ({});
  return c;
}

beforeEach(() => {
  vi.resetModules();
  FakeWorker.last = null;
  setGpuUnavailable.mockClear();
  showNotification.mockClear();
  attachSharedWorker.mockClear();
  vi.stubGlobal('Worker', FakeWorker);
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  vi.spyOn(console, 'info').mockImplementation(() => {});
});
afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('Rage 128 takeover host', () => {
  it('withdraws the GPU when the device is lost under an attached card', async () => {
    const r128 = await import('@/gpu/rage128Gpu.svelte');
    r128.registerRage128Canvas(overlayCanvas());
    r128.onRage128GpuAttach(0x1000);
    expect(attachSharedWorker).toHaveBeenCalledWith(FakeWorker.last, 0x1000, '__r128gpu');
    FakeWorker.last!.answer({ type: 'lost', reason: 'unknown: gone' });
    expect(setGpuUnavailable).toHaveBeenCalled();
    expect(showNotification).toHaveBeenCalledWith(expect.stringMatching(/Rage 128/), 'warning');
  });

  it('logs the reason the worker gives for no device', async () => {
    const r128 = await import('@/gpu/rage128Gpu.svelte');
    r128.registerRage128Canvas(overlayCanvas());
    r128.onRage128GpuAttach(0x1000);
    FakeWorker.last!.answer({ type: 'unavailable', reason: 'requestAdapter() returned null' });
    expect(console.warn).toHaveBeenCalledWith(
      expect.stringMatching(/no WebGPU device/),
      'requestAdapter() returned null',
    );
  });
});

// The workers themselves, without WebGPU: init answers "unavailable" with
// the reason, as the page logs it.
describe('takeover workers without WebGPU', () => {
  for (const [name, load] of [
    ['voodoo2', () => import('@/gpu/voodoo2Gpu.worker')],
    ['rage128', () => import('@/gpu/rage128Gpu.worker')],
  ] as const) {
    it(`${name}: init says why there is no device`, async () => {
      const post = vi.fn();
      vi.stubGlobal('postMessage', post);
      expect('gpu' in navigator).toBe(false);
      await load();
      (self.onmessage as (ev: MessageEvent) => void)({
        data: { type: 'init', canvas: {} },
      } as MessageEvent);
      await vi.waitFor(() => expect(post).toHaveBeenCalled());
      expect(post).toHaveBeenCalledWith({
        type: 'unavailable',
        reason: expect.stringMatching(/no WebGPU/),
      });
    });
  }
});
