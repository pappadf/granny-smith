// Bridges between the Terminal's two panes.  Each side registers itself on
// mount and clears it on destroy; callers forward to the current one (a
// no-op, returning false, while that pane isn't mounted).
//   console input  ← the command browser writes the path token at the
//                    cursor, snapshots / restores the input, hands focus
//   revealInBrowser ← a console object link selects its node

export { pathPrefixes } from '@/lib/objectPath';

export interface InputState {
  text: string;
  cursor: number;
}

export interface ConsoleInputApi {
  // Replace the path token at the cursor with `text`.
  replaceToken(text: string): void;
  focusEnd(): void;
  getState(): InputState;
  restore(s: InputState): void;
}

let input: ConsoleInputApi | null = null;
let revealer: ((path: string) => void) | null = null;
// Set while the browser writes, so the input change it causes is known
// to be the browser's own (terminalSync.fromBrowser).
let browserWriting = false;

export function registerConsoleInput(api: ConsoleInputApi | null): void {
  input = api;
}

export function writeToConsole(text: string): boolean {
  if (!input) return false;
  browserWriting = true;
  try {
    input.replaceToken(text);
  } finally {
    browserWriting = false;
  }
  return true;
}

export function isBrowserWriting(): boolean {
  return browserWriting;
}

export function focusConsole(): boolean {
  if (!input) return false;
  input.focusEnd();
  return true;
}

export function snapshotConsole(): InputState | null {
  return input ? input.getState() : null;
}

export function restoreConsole(s: InputState): boolean {
  if (!input) return false;
  browserWriting = true;
  try {
    input.restore(s);
  } finally {
    browserWriting = false;
  }
  return true;
}

export function registerBrowserReveal(fn: ((path: string) => void) | null): void {
  revealer = fn;
}

export function revealInBrowser(path: string): boolean {
  if (!revealer) return false;
  revealer(path);
  return true;
}
