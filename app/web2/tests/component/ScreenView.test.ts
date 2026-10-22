import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach, vi } from 'vitest';
import ScreenView from '@/components/display/ScreenView.svelte';
import { machine, setZoom } from '@/state/machine.svelte';

beforeEach(() => {
  machine.screen.width = 512;
  machine.screen.height = 342;
  setZoom(200);
});

describe('ScreenView', () => {
  it('renders a canvas with the machine intrinsic dimensions', () => {
    const { container } = render(ScreenView);
    const canvas = container.querySelector('canvas') as HTMLCanvasElement;
    expect(canvas).not.toBeNull();
    expect(canvas.width).toBe(512);
    expect(canvas.height).toBe(342);
  });

  it('CSS width/height scale with the zoom level', () => {
    const { container } = render(ScreenView);
    const canvas = container.querySelector('canvas') as HTMLCanvasElement;
    expect(canvas.style.width).toBe('1024px'); // 512 * 2.0
    expect(canvas.style.height).toBe('684px'); // 342 * 2.0
  });

  it('only the primary button grabs the pointer of a running machine', async () => {
    const { container } = render(ScreenView);
    const canvas = container.querySelector('canvas') as HTMLCanvasElement;
    const lock = vi.fn();
    canvas.requestPointerLock = lock;
    machine.status = 'running';
    try {
      await fireEvent.mouseDown(canvas, { button: 2 });
      expect(lock).not.toHaveBeenCalled();
      await fireEvent.mouseDown(canvas, { button: 0 });
      expect(lock).toHaveBeenCalledTimes(1);
    } finally {
      machine.status = 'no-machine';
    }
  });
});
