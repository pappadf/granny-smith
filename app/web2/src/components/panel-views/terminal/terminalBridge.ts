// Bridges between the Terminal's two panes.  Each side registers itself on
// mount and clears it on destroy; callers forward to the current one (a
// no-op, returning false, while that pane isn't mounted).
//   console input   ← the command browser inserts at the cursor, hands focus
//   revealInBrowser ← a console object link selects its node

export interface ConsoleInputApi {
  // Replace the path token at the cursor with `text`.
  replaceToken(text: string): void;
  focusEnd(): void;
}

let input: ConsoleInputApi | null = null;
let revealer: ((path: string) => void) | null = null;

export function registerConsoleInput(api: ConsoleInputApi | null): void {
  input = api;
}

export function writeToConsole(text: string): boolean {
  if (!input) return false;
  input.replaceToken(text);
  return true;
}

export function focusConsole(): boolean {
  if (!input) return false;
  input.focusEnd();
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
