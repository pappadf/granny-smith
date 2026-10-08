import { describe, it, expect } from 'vitest';
import { machine, fitZoom, setZoom } from '@/state/machine.svelte';

// Until the user picks a zoom, the screen is fitted to the display area:
// the largest 10 % step up to the 200 % default.  (One module, one
// sequence: the user's choice is sticky.)
describe('fitZoom', () => {
  it('fits a 640x480 screen into a 1380x560 area at 110 %', () => {
    fitZoom(640, 480, 1, 1380, 560);
    expect(machine.zoom).toBe(110);
  });

  it('keeps a compact Mac at the 200 % default', () => {
    fitZoom(512, 342, 1, 1380, 900);
    expect(machine.zoom).toBe(200);
  });

  it('never goes below 100 % (a larger screen scrolls)', () => {
    fitZoom(1152, 870, 1, 800, 500);
    expect(machine.zoom).toBe(100);
  });

  it('counts tall pixels (the Lisa: 1.5)', () => {
    fitZoom(720, 364, 1.5, 1500, 700);
    expect(machine.zoom).toBe(120);
  });

  it('leaves a zoom the user chose alone', () => {
    setZoom(170);
    fitZoom(640, 480, 1, 1380, 560);
    expect(machine.zoom).toBe(170);
  });
});
