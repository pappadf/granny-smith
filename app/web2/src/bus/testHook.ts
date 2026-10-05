// A test-only door into the gsEval bridge.
//
// web2 deliberately exposes no `window.gsEval`: real users drive the emulator
// through the UI and the terminal.  e2e specs, though, need to read what the
// core holds after a UI action — which bay a disk landed in, whether a
// breakpoint exists — and typing probes into the console races heavy output.  So
// under automation only (`navigator.webdriver`, which drivers set and ordinary
// browsing does not) the page carries `window.__gsEvalForTests`, a thin
// wrapper over the bus `gsEval`.  No build flag, no query parameter, and the
// production surface for a real user is unchanged.

import { gsEval } from './emulator';
import { showNotification, type ToastSeverity } from '@/state/toasts.svelte';
import { showPrintedDocument } from '@/state/printer.svelte';

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

// The UI's own surfaces a spec cannot reach through the core without a long
// detour (a toast, the print viewer): the app-states screenshots open them
// directly.  Automation only, like the eval hook.
export interface GsUiForTests {
  notify: (msg: string, severity: ToastSeverity) => void;
  showPdf: (doc: { name: string; title: string; pages: number; pdf: number[] }) => void;
}

// Install `__gsUiForTests` on `win` when it is driven by automation.
export function installUiHookForAutomation(win: Window = window): boolean {
  if (!win.navigator?.webdriver) return false;
  const hook: GsUiForTests = {
    notify: (msg, severity) => showNotification(msg, severity),
    showPdf: (doc) => showPrintedDocument({ ...doc, pdf: new Uint8Array(doc.pdf) }),
  };
  (win as unknown as { __gsUiForTests?: GsUiForTests }).__gsUiForTests = hook;
  return true;
}
