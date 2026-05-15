// Toast system — spec §7. Three severities (info/warning/error), max 3
// visible, queueing rule for spam (>3 in 800 ms goes straight to queue),
// hover pauses auto-dismiss timer.

export type ToastSeverity = 'info' | 'warning' | 'error';

export interface Toast {
  id: number;
  msg: string;
  severity: ToastSeverity;
  createdAt: number;
}

const TTL_MS: Record<ToastSeverity, number> = {
  info: 10_000,
  warning: 12_000,
  error: 15_000,
};

const MAX_VISIBLE = 3;
const SPAM_WINDOW_MS = 800;

interface ToastsState {
  active: Toast[];
  queued: Toast[];
}

export const toasts: ToastsState = $state({ active: [], queued: [] });

let nextId = 1;
// Per-toast timer ids; cleared on hover, restarted on hover-out, removed on dismiss.
// Plain Map is fine here — this is internal bookkeeping, not reactive state.
// eslint-disable-next-line svelte/prefer-svelte-reactivity
const timers = new Map<number, ReturnType<typeof setTimeout>>();
// Trailing window of push timestamps to detect spam.
let recentPushes: number[] = [];

export function showNotification(msg: string, severity: ToastSeverity = 'info'): number {
  const now = Date.now();
  // Drop timestamps older than the spam window.
  recentPushes = recentPushes.filter((t) => now - t < SPAM_WINDOW_MS);
  recentPushes.push(now);

  const toast: Toast = { id: nextId++, msg, severity, createdAt: now };

  // Spam protection: if more than MAX_VISIBLE pushes happened inside the
  // trailing window, force the new toast into the queue even when active has
  // room. Otherwise honour the visible cap.
  const spamming = recentPushes.length > MAX_VISIBLE;
  if (spamming || toasts.active.length >= MAX_VISIBLE) {
    toasts.queued.push(toast);
  } else {
    toasts.active.push(toast);
    scheduleDismiss(toast);
  }
  return toast.id;
}

// Legacy alias kept for parity with the prototype's `toast(msg)` calls.
// Plan §4 keeps the alias until Phase 7.
export function toast(msg: string): number {
  return showNotification(msg, 'info');
}

export function dismissToast(id: number): void {
  const idx = toasts.active.findIndex((t) => t.id === id);
  if (idx === -1) {
    // Toast may still be queued — remove it there.
    const qIdx = toasts.queued.findIndex((t) => t.id === id);
    if (qIdx !== -1) toasts.queued.splice(qIdx, 1);
    return;
  }
  toasts.active.splice(idx, 1);
  clearTimer(id);
  promoteFromQueue();
}

export function pauseTimer(id: number): void {
  clearTimer(id);
}

export function resumeTimer(id: number): void {
  const t = toasts.active.find((x) => x.id === id);
  if (t) scheduleDismiss(t);
}

// Clear everything — used in tests.
export function _resetForTests(): void {
  toasts.active.splice(0, toasts.active.length);
  toasts.queued.splice(0, toasts.queued.length);
  for (const timer of timers.values()) clearTimeout(timer);
  timers.clear();
  recentPushes = [];
  nextId = 1;
}

function scheduleDismiss(toast: Toast): void {
  clearTimer(toast.id);
  const timer = setTimeout(() => dismissToast(toast.id), TTL_MS[toast.severity]);
  timers.set(toast.id, timer);
}

function clearTimer(id: number): void {
  const timer = timers.get(id);
  if (timer !== undefined) {
    clearTimeout(timer);
    timers.delete(id);
  }
}

function promoteFromQueue(): void {
  while (toasts.active.length < MAX_VISIBLE && toasts.queued.length > 0) {
    const next = toasts.queued.shift();
    if (next) {
      toasts.active.push(next);
      scheduleDismiss(next);
    }
  }
}
