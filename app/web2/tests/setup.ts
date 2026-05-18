import '@testing-library/jest-dom/vitest';

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

HTMLCanvasElement.prototype.getContext = function getContext() {
  return noopCanvasContext as unknown as CanvasRenderingContext2D;
} as unknown as HTMLCanvasElement['getContext'];
