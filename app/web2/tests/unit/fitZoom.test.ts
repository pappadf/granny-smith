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

  it('shrinks a large screen below 100 % to fit (a 21-inch 1152x870)', () => {
    fitZoom(1152, 870, 1, 1360, 504);
    expect(machine.zoom).toBe(50);
    fitZoom(1152, 870, 1, 1360, 796);
    expect(machine.zoom).toBe(90);
  });

  it('never goes below 50 % (a larger screen scrolls)', () => {
    fitZoom(1152, 870, 1, 300, 200);
    expect(machine.zoom).toBe(50);
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

  it('lets the user zoom down to 50 % and no further', () => {
    setZoom(60);
    expect(machine.zoom).toBe(60);
    setZoom(40);
    expect(machine.zoom).toBe(50);
  });
});
