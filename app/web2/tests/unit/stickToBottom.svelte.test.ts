// Auto-scroll: new content keeps a view at the bottom pinned there; a
// scroll away unpins it, coming back or follow() re-pins it; a jump is
// repeated while the height grows under it.
import { describe, it, expect, afterEach } from 'vitest';
import { flushSync, tick } from 'svelte';
import { stickToBottom } from '@/lib/stickToBottom.svelte';

const frame = () => new Promise((r) => requestAnimationFrame(() => r(null)));

// jsdom has no layout: a scroll box with a settable height.
function box(height = 1000, client = 100) {
  const el = document.createElement('div');
  let h = height;
  let top = 0;
  Object.defineProperty(el, 'scrollHeight', { get: () => h, configurable: true });
  Object.defineProperty(el, 'clientHeight', { value: client, configurable: true });
  Object.defineProperty(el, 'scrollTop', {
    get: () => top,
    set: (v: number) => (top = Math.max(0, Math.min(v, h - client))),
    configurable: true,
  });
  return {
    el,
    grow: (n: number) => (h += n),
    scrollTo: (v: number) => {
      el.scrollTop = v;
      el.dispatchEvent(new Event('scroll'));
    },
  };
}

let cleanup: (() => void) | null = null;
afterEach(() => {
  cleanup?.();
  cleanup = null;
});

// Attaches `stick` to `el` with `content` as the reactive dependency.
function mount(el: HTMLElement) {
  let content = $state(0);
  const stick = stickToBottom(() => content);
  cleanup = $effect.root(() => {
    $effect(() => stick.attach(el) ?? undefined);
  });
  flushSync();
  return {
    stick,
    change: async () => {
      content++;
      flushSync();
      await tick();
    },
  };
}

describe('stickToBottom', () => {
  it('follows new content while at the bottom', async () => {
    const b = box();
    const { change } = mount(b.el);
    await tick();
    expect(b.el.scrollTop).toBe(900);
    b.grow(200);
    await change();
    expect(b.el.scrollTop).toBe(1100);
  });

  it('a scroll away unpins it; back at the bottom pins it again', async () => {
    const b = box();
    const { change } = mount(b.el);
    await tick();
    b.scrollTo(100);
    b.grow(200);
    await change();
    expect(b.el.scrollTop).toBe(100);
    b.scrollTo(1100);
    b.grow(50);
    await change();
    expect(b.el.scrollTop).toBe(1150);
  });

  it('follow() re-pins whatever the user scrolled', async () => {
    const b = box();
    const { stick } = mount(b.el);
    await tick();
    b.scrollTo(0);
    stick.follow();
    expect(b.el.scrollTop).toBe(900);
  });

  it('repeats the jump while the height grows without a scroll event', async () => {
    const b = box();
    const { change } = mount(b.el);
    await tick();
    b.grow(100);
    await change();
    expect(b.el.scrollTop).toBe(1000);
    // Content laid out after the jump: the next frame jumps again.
    b.grow(300);
    await frame();
    expect(b.el.scrollTop).toBe(1300);
  });

  it('detaching stops following', async () => {
    const b = box();
    const { change } = mount(b.el);
    await tick();
    cleanup?.();
    cleanup = null;
    b.grow(500);
    await change();
    await frame();
    expect(b.el.scrollTop).toBe(900);
  });
});
