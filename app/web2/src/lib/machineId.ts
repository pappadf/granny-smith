// Per-machine identity for the checkpoint directory layout
// (/opfs/checkpoints/<id>-<created>/). Mirrors app/web/js/checkpoint-machine.js.
// The id+timestamp pair lives in localStorage; the C side reads it via
// `machine.register(id, created)` so it can root delta/journal files
// underneath the right directory.

import { utcCompactStamp } from './time';
import { KEYS, readLS, writeLS } from './storage';

const KEY = KEYS.checkpointMachine;

export interface MachineIdentity {
  id: string;
  created: string;
}

// 8 random bytes encoded as 16 hex characters.
function randomId(): string {
  const bytes = new Uint8Array(8);
  crypto.getRandomValues(bytes);
  return [...bytes].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// Compact ISO 8601 UTC: "20260430T153045Z".
export function nowStamp(): string {
  return utcCompactStamp();
}

function read(): MachineIdentity | null {
  try {
    const raw = readLS(KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as Partial<MachineIdentity>;
    if (parsed && typeof parsed.id === 'string' && typeof parsed.created === 'string') {
      return { id: parsed.id, created: parsed.created };
    }
  } catch {
    // Malformed JSON or localStorage unavailable.
  }
  return null;
}

function write(m: MachineIdentity): void {
  writeLS(KEY, JSON.stringify(m));
}

// Return the existing machine record, minting one (and persisting it) if none.
export function getOrCreateMachine(): MachineIdentity {
  const existing = read();
  if (existing) return existing;
  const fresh: MachineIdentity = { id: randomId(), created: nowStamp() };
  write(fresh);
  return fresh;
}

// Replace the persisted machine record with a fresh one and return it.
// The C side does not support in-process rotation; callers typically
// follow this with location.reload().
export function rotateMachine(): MachineIdentity {
  const fresh: MachineIdentity = { id: randomId(), created: nowStamp() };
  write(fresh);
  return fresh;
}
