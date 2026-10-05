// Helpers for the Checkpoints panel view. Per docs/checkpointing.md, every
// checkpoint lives in a per-machine dir whose name is `<id>-<created>`:
//   <id>      — 16 hex chars (8 random bytes)
//   <created> — compact ISO 8601 (YYYYMMDDTHHMMSSZ)
// The dir also contains state.checkpoint (the quick-save) and a manifest.json
// the core writes (`machine: {model, ram_bytes}`, the build, the images); the
// UI adds a `label` to it.

export interface ParsedCheckpointDir {
  id: string;
  created: string;
}

const DIR_RE = /^([0-9a-f]{16})-(\d{8}T\d{6}Z)$/;

export function parseCheckpointDirName(name: string): ParsedCheckpointDir | null {
  const m = DIR_RE.exec(name);
  if (!m) return null;
  return { id: m[1], created: m[2] };
}

// Format a compact ISO 8601 timestamp (YYYYMMDDTHHMMSSZ) into a human label
// suitable for the Checkpoints table when no manifest is present.
export function formatCheckpointLabel(created: string): string {
  const m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z$/.exec(created);
  if (!m) return `Checkpoint ${created}`;
  return `Checkpoint ${m[1]}-${m[2]}-${m[3]} ${m[4]}:${m[5]}`;
}

// Convert compact ISO 8601 back to a sortable Date.
export function checkpointCreatedToDate(created: string): Date | null {
  const m = /^(\d{4})(\d{2})(\d{2})T(\d{2})(\d{2})(\d{2})Z$/.exec(created);
  if (!m) return null;
  return new Date(
    Date.UTC(
      parseInt(m[1], 10),
      parseInt(m[2], 10) - 1,
      parseInt(m[3], 10),
      parseInt(m[4], 10),
      parseInt(m[5], 10),
      parseInt(m[6], 10),
    ),
  );
}

// Compact human size, e.g. "20 MB".
export function formatBytes(n: number): string {
  if (n <= 0 || !Number.isFinite(n)) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  let v = n;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v < 10 && i > 0 ? v.toFixed(1) : Math.round(v)} ${units[i]}`;
}

// The manifest's `machine`: the core writes `{model, ram_bytes}`; manifests
// written by older builds of this UI hold the model as a plain string.
export function manifestMachine(m: unknown): { model: string; ramBytes: number } | null {
  if (typeof m === 'string' && m) return { model: m, ramBytes: 0 };
  if (m && typeof m === 'object') {
    const o = m as { model?: unknown; ram_bytes?: unknown };
    if (typeof o.model === 'string' && o.model) {
      return { model: o.model, ramBytes: typeof o.ram_bytes === 'number' ? o.ram_bytes : 0 };
    }
  }
  return null;
}

// "Macintosh Plus · 4 MB" (the RAM left out when unknown).
export function describeMachine(name: string, ramBytes: number): string {
  if (ramBytes <= 0) return name;
  const mb = ramBytes / (1024 * 1024);
  return `${name} · ${Number.isInteger(mb) ? `${mb} MB` : formatBytes(ramBytes)}`;
}
