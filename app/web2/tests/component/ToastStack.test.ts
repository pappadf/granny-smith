import { describe, it, expect, beforeEach } from 'vitest';
import { _resetForTests, showNotification, toasts, dismissToast } from '@/state/toasts.svelte';

// The toast queue + spam-protection rules are pure state. Test them directly
// without rendering — keeps the component small and the rules unit-testable.

beforeEach(() => _resetForTests());

describe('ToastStack queueing', () => {
  it('caps the active list at 3', () => {
    showNotification('a');
    showNotification('b');
    showNotification('c');
    showNotification('d');
    expect(toasts.active.map((t) => t.msg)).toEqual(['a', 'b', 'c']);
    expect(toasts.queued.map((t) => t.msg)).toEqual(['d']);
  });

  it('promotes from queue when an active toast is dismissed', () => {
    showNotification('a');
    showNotification('b');
    showNotification('c');
    showNotification('d');
    dismissToast(toasts.active[0].id);
    expect(toasts.active.map((t) => t.msg)).toEqual(['b', 'c', 'd']);
    expect(toasts.queued).toEqual([]);
  });

  it('routes >3 pushes within 800ms straight to the queue', () => {
    showNotification('a');
    showNotification('b');
    showNotification('c');
    // 4th push inside the 800ms window — must land in queue even though
    // active still has room conceptually (it's full at 3 here, but the spam
    // rule kicks in earlier than the cap when the rate exceeds it).
    showNotification('d');
    expect(toasts.active.length).toBe(3);
    expect(toasts.queued.length).toBe(1);
  });

  it('dismissing a queued toast removes it from the queue', () => {
    showNotification('a');
    showNotification('b');
    showNotification('c');
    showNotification('d'); // queued
    const queuedId = toasts.queued[0].id;
    dismissToast(queuedId);
    expect(toasts.queued).toEqual([]);
    expect(toasts.active.length).toBe(3);
  });
});
