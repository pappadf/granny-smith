// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// The gsEval result contract's predicates, pure so lib/ can use them too
// (bus/emulator.ts re-exports them beside gsEval):
//   - a value     — the method or attribute's result;
//   - null        — ONLY a successful method that returns nothing (VK_NONE);
//   - { error }   — failure.  A C-side VK_ERROR carries the core's message;
//                   a failure of the bridge itself (module not ready, a
//                   thrown request) also sets `transport: true`.
// So `r !== null` is never a success test: `{ error }` satisfies it.  Use
// gsOk() for "did it work", `=== true` for a VK_BOOL method, and
// isGsObject() for a read that answers an object.
export interface GsError {
  error: string;
  transport?: true;
}

// True for any failure shape — the core's VK_ERROR or a transport failure.
export function isGsError(res: unknown): res is GsError {
  return !!res && typeof res === 'object' && 'error' in res;
}

// True for an object answer (a VK_MAP read, a method's record): an object
// that is not a failure.
export function isGsObject(res: unknown): res is object {
  return !!res && typeof res === 'object' && !isGsError(res);
}

// "Did the call work?": not an error, and not a VK_BOOL method's `false`.
// A VK_NONE success (null) counts as success.
export function gsOk(res: unknown): boolean {
  return !isGsError(res) && res !== false;
}

// Human-readable reason from a failed gsEval result. Single home for the
// error-shape knowledge so callers don't each re-implement the check.
export function gsErrorText(res: unknown): string {
  if (isGsError(res)) return String(res.error);
  if (res === false) return 'the operation reported failure';
  if (res === null) return 'no result';
  return String(res);
}
