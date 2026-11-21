// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// One policy for the window globals the page leaves for specs and for a
// person debugging in the console (__gsCoreEvents, __micStats, the shared
// workers' memory and control block, the graphics verdict): they exist
// under automation (`navigator.webdriver`, as bus/testHook.ts's hooks) or
// when the page is opened with `?debug`, never on an ordinary visit, so the
// wasm Memory is not left on window in production.  The public readiness
// contract (__gsReady, __gsBootError) is not this: it is always set.

// Whether this page exposes its debug globals.
export function debugGlobalsEnabled(win: Window = window): boolean {
  if (win.navigator?.webdriver) return true;
  try {
    return new URLSearchParams(win.location?.search ?? '').has('debug');
  } catch {
    return false;
  }
}

// Set window[name] = value when debug globals are enabled.  Returns
// whether it did.
export function exposeDebug(name: string, value: unknown, win: Window = window): boolean {
  if (typeof win === 'undefined' || !debugGlobalsEnabled(win)) return false;
  (win as unknown as Record<string, unknown>)[name] = value;
  return true;
}
