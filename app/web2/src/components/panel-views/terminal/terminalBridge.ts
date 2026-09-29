// Bridges between the Terminal's two panes.  Each side registers a function
// on mount and clears it on destroy; callers forward to the current one (a
// no-op, returning false, while that pane isn't mounted).
//   insertIntoTerminal   CommandBrowser → console input (click-to-insert)
//   revealInBrowser      console object link → CommandBrowser selection

export { pathPrefixes } from '@/lib/objectPath';

let setter: ((text: string) => void) | null = null;
let revealer: ((path: string) => void) | null = null;

export function registerTerminalInsert(fn: ((text: string) => void) | null): void {
  setter = fn;
}

export function insertIntoTerminal(text: string): boolean {
  if (!setter) return false;
  setter(text);
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
