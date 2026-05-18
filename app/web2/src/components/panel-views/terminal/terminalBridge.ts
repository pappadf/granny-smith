// Bridge between CommandBrowser (click-to-insert) and TerminalPane
// (the live xterm). TerminalPane registers a setter on mount and clears
// it on destroy; CommandBrowser calls insertIntoTerminal which forwards
// to the current setter (or is a no-op if the pane isn't mounted).

let setter: ((text: string) => void) | null = null;

export function registerTerminalInsert(fn: ((text: string) => void) | null): void {
  setter = fn;
}

export function insertIntoTerminal(text: string): boolean {
  if (!setter) return false;
  setter(text);
  return true;
}
