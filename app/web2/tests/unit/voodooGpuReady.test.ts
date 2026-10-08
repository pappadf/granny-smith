// Page startup awaits the WebGPU adapter's answer before it reports ready
// (bus/emulator.ts), so the wait is bounded: an answer that does not come in
// time counts as no adapter, and says so, rather than holding the page.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const setGpuUnavailable = vi.fn();
vi.mock('@/bus/emulator', () => ({ getModule: () => null, setGpuUnavailable }));

// The GPU worker: the test answers for it.
class FakeWorker {
  static last: FakeWorker | null = null;
  onmessage: ((ev: MessageEvent) => void) | null = null;
  onerror: ((ev: ErrorEvent) => void) | null = null;
  constructor() {
    FakeWorker.last = this;
  }
  postMessage(): void {}
  answer(type: string): void {
    this.onmessage?.({ data: { type } } as MessageEvent);
  }
}

// A canvas that can hand its control to the worker, as a browser's can.
function overlayCanvas(): HTMLCanvasElement {
  const c = document.createElement('canvas');
  (c as unknown as { transferControlToOffscreen: () => unknown }).transferControlToOffscreen =
    () => ({});
  return c;
}

let gpu: typeof import('@/gpu/voodoo2Gpu.svelte');

beforeEach(async () => {
  vi.resetModules();
  vi.useFakeTimers();
  FakeWorker.last = null;
  setGpuUnavailable.mockClear();
  vi.stubGlobal('Worker', FakeWorker);
  Object.defineProperty(navigator, 'gpu', { value: {}, configurable: true });
  gpu = await import('@/gpu/voodoo2Gpu.svelte');
});
afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  delete (navigator as unknown as { gpu?: unknown }).gpu;
});

describe('whenVoodooGpuReady', () => {
  it('answers no at once when the GPU worker was never started', async () => {
    await expect(gpu.whenVoodooGpuReady(2000)).resolves.toBe(false);
  });

  it("answers the worker's yes when it comes within the bound", async () => {
    void gpu.startVoodooGpu(overlayCanvas());
    const answer = gpu.whenVoodooGpuReady(2000);
    await vi.advanceTimersByTimeAsync(500);
    FakeWorker.last!.answer('ready');
    await expect(answer).resolves.toBe(true);
  });

  it("answers the worker's no", async () => {
    void gpu.startVoodooGpu(overlayCanvas());
    const answer = gpu.whenVoodooGpuReady(2000);
    FakeWorker.last!.answer('unavailable');
    await expect(answer).resolves.toBe(false);
  });

  it('counts no answer within the bound as no adapter, and logs it', async () => {
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
    void gpu.startVoodooGpu(overlayCanvas());
    const answer = gpu.whenVoodooGpuReady(2000);
    let settled = false;
    void answer.then(() => (settled = true));
    await vi.advanceTimersByTimeAsync(1999);
    expect(settled).toBe(false);
    await vi.advanceTimersByTimeAsync(1);
    await expect(answer).resolves.toBe(false);
    expect(warn).toHaveBeenCalledWith(
      expect.stringMatching(/no WebGPU adapter answer within 2000 ms/),
    );
  });
});

describe('device loss', () => {
  // A card attached at the time reads the loss from the shared block; the
  // page must stop offering the GPU to cards created later.
  it('tells the core the GPU is gone and drops the overlay', async () => {
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    void gpu.startVoodooGpu(overlayCanvas());
    FakeWorker.last!.answer('ready');
    gpu.gpuOverlay.visible = true;
    FakeWorker.last!.answer('lost');
    expect(setGpuUnavailable).toHaveBeenCalled();
    expect(gpu.gpuOverlay.visible).toBe(false);
  });
});
