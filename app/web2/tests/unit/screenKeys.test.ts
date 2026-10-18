import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import { machine } from '@/state/machine.svelte';
import { startScreenKeyGuard, screenHasKeyboard } from '@/lib/screenKeys';

// #352: keys the grabbed screen receives must not reach the browser's
// shortcuts (the core's proxied callback cannot preventDefault), and a live
// machine asks before the page unloads.

let stop: () => void = () => undefined;
let screen: HTMLCanvasElement;
let locked: Element | null = null;

// jsdom has no pointer lock: the test decides what holds it.
Object.defineProperty(document, 'pointerLockElement', {
  configurable: true,
  get: () => locked,
});

// Dispatch a cancelable key event at the screen and report whether its
// default was prevented, and whether a later document listener still saw it.
function press(type: 'keydown' | 'keyup', key: string, code: string) {
  let seen = false;
  const watch = () => {
    seen = true;
  };
  document.addEventListener(type, watch, true);
  const e = new KeyboardEvent(type, { key, code, bubbles: true, cancelable: true });
  screen.dispatchEvent(e);
  document.removeEventListener(type, watch, true);
  return { prevented: e.defaultPrevented, seen };
}

// Dispatch a beforeunload and report whether the page asked to stay.
function unload(): boolean {
  const e = new Event('beforeunload', { cancelable: true }) as BeforeUnloadEvent;
  window.dispatchEvent(e);
  return e.defaultPrevented;
}

beforeEach(() => {
  screen = document.createElement('canvas');
  screen.id = 'screen';
  document.body.appendChild(screen);
  locked = null;
  machine.status = 'no-machine';
  stop = startScreenKeyGuard();
});

afterEach(() => {
  stop();
  screen.remove();
  locked = null;
  machine.status = 'no-machine';
});

describe('screen key guard', () => {
  it('prevents F5 and Ctrl-D while the screen holds the pointer lock', () => {
    locked = screen;
    expect(screenHasKeyboard()).toBe(true);
    expect(press('keydown', 'F5', 'F5').prevented).toBe(true);
    expect(press('keydown', 'd', 'KeyD').prevented).toBe(true);
    expect(press('keyup', 'F5', 'F5').prevented).toBe(true);
  });

  it('still lets the core listener on document see the event', () => {
    locked = screen;
    expect(press('keydown', 'F5', 'F5').seen).toBe(true);
  });

  it('leaves keys alone while the screen does not have the keyboard', () => {
    expect(press('keydown', 'F5', 'F5').prevented).toBe(false);
    // Another element holding the lock is not the screen.
    locked = document.body;
    expect(screenHasKeyboard()).toBe(false);
    expect(press('keydown', 'F5', 'F5').prevented).toBe(false);
  });

  it('removes its listeners when stopped', () => {
    stop();
    locked = screen;
    expect(press('keydown', 'F5', 'F5').prevented).toBe(false);
  });
});

describe('leaving the page', () => {
  it('asks while a machine is running or paused', () => {
    machine.status = 'running';
    expect(unload()).toBe(true);
    machine.status = 'paused';
    expect(unload()).toBe(true);
  });

  it('does not ask with no live machine', () => {
    for (const s of ['no-machine', 'stopped', 'crashed'] as const) {
      machine.status = s;
      expect(unload()).toBe(false);
    }
  });
});
