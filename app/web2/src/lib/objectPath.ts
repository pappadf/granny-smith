// Object-model paths as the shell writes them (`machine.scsi.device[3]`,
// `log.category["scsi"]`).

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
