// Bridges between the Terminal's two panes.  Each side registers a function
// on mount and clears it on destroy; callers forward to the current one (a
// no-op, returning false, while that pane isn't mounted).
//   insertIntoTerminal   CommandBrowser → console input (click-to-insert)
//   revealInBrowser      console object link → CommandBrowser selection

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

// The path prefixes a node's path passes through, outermost first:
// `machine.scsi.device[3].image` → machine, machine.scsi,
// machine.scsi.device, machine.scsi.device[3], machine.scsi.device[3].image.
export function pathPrefixes(path: string): string[] {
  const out: string[] = [];
  let i = 0;
  let quoted = false;
  for (; i < path.length; i++) {
    const c = path[i];
    if (c === '"') quoted = !quoted;
    if (quoted) continue;
    if ((c === '.' || c === '[') && i > 0) out.push(path.slice(0, i));
  }
  if (path) out.push(path);
  return out;
}
