// A test-only door into the gsEval bridge (11-WORK-ORDER unit 0.3,
// proposal-misc-cleanup §4).
//
// web2 deliberately exposes no `window.gsEval`: real users drive the emulator
// through the UI and the terminal.  e2e specs, though, need to read what the
// core holds after a UI action — which bay a disk landed in, whether a
// breakpoint exists — and typing probes into xterm races heavy output.  So
// under automation only (`navigator.webdriver`, which drivers set and ordinary
// browsing does not) the page carries `window.__gsEvalForTests`, a thin
// wrapper over the bus `gsEval`.  No build flag, no query parameter, and the
// production surface for a real user is unchanged.

import { gsEval } from './emulator';

// The hook's shape, as a spec sees it through page.evaluate.
export type GsEvalForTests = (
  path: string,
  args?: unknown[] | Record<string, unknown>,
) => Promise<unknown>;

// Install `__gsEvalForTests` on `win` when it is driven by automation.
// Returns whether the hook was installed.
export function installEvalHookForAutomation(win: Window = window): boolean {
  if (!win.navigator?.webdriver) return false;
  (win as unknown as { __gsEvalForTests?: GsEvalForTests }).__gsEvalForTests = gsEval;
  return true;
}
