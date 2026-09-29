import '@testing-library/jest-dom/vitest';
import { beforeEach } from 'vitest';
import { MockOpfs } from './helpers/mockOpfs';

// jsdom does not implement HTMLCanvasElement.getContext (it logs a noisy
// "Not implemented" warning otherwise). Tests that exercise canvas painting
// only care that the code path doesn't throw — give them a no-op 2D context.
const noopCanvasContext = {
  fillStyle: '',
  strokeStyle: '',
  lineWidth: 1,
  font: '',
  fillRect: () => undefined,
  strokeRect: () => undefined,
  fillText: () => undefined,
  clearRect: () => undefined,
  beginPath: () => undefined,
  closePath: () => undefined,
  moveTo: () => undefined,
  lineTo: () => undefined,
  stroke: () => undefined,
  fill: () => undefined,
};

// A test that opts into the node environment (// @vitest-environment node)
// has no DOM to patch.
if (typeof HTMLCanvasElement !== 'undefined') {
  HTMLCanvasElement.prototype.getContext = function getContext() {
    return noopCanvasContext as unknown as CanvasRenderingContext2D;
  } as unknown as HTMLCanvasElement['getContext'];
}

// jsdom has no layout: CodeMirror (the console input) measures text through
// Range rects.  Empty rects are enough for it not to throw.
if (typeof Range !== 'undefined') {
  Object.assign(Range.prototype, {
    getClientRects: () => ({ length: 0, item: () => null, [Symbol.iterator]: [][Symbol.iterator] }),
    getBoundingClientRect: () => ({
      x: 0,
      y: 0,
      top: 0,
      left: 0,
      bottom: 0,
      right: 0,
      width: 0,
      height: 0,
    }),
  });
}

// Every test starts on a fresh MockOpfs (the production default throws until
// main.ts installs BrowserOpfs).  Imported inside the hook, not at the top:
// by then the test file's own imports -- and its vi.mock()s -- are in place,
// and this finds the same module instance they did.  (Importing it here
// would load the real bus/emulator ahead of those mocks.)  A test that wants
// other storage sets its own backend in its own beforeEach, which runs after
// this one.
beforeEach(async () => {
  if (typeof document === 'undefined') return; // node-environment tests
  const bus = (await import('@/bus/opfs')) as Partial<typeof import('@/bus/opfs')>;
  bus.setOpfsBackend?.(new MockOpfs());
});
