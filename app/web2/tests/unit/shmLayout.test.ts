// @vitest-environment node
//
// The shared-heap layouts are written twice: once in a C header the core
// compiles against and once in a TS module the page reads the heap with.
// Nothing ties the two but this test, which parses every `#define` in each
// header and compares it with the TS export of the same name (the header's
// prefix dropped).  A word renumbered on one side only would otherwise read
// the wrong field, silently.

import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import * as Shm from '@/bus/shmLayout';
import * as V2 from '@/gpu/voodoo2Protocol';
import * as R128 from '@/gpu/rage128Protocol';
import * as Takeover from '@/gpu/takeoverProtocol';
import * as Lw from '@/printer/platenProtocol';
import * as Mbx from '@/bus/mailbox';
import * as MbxRing from '@/bus/mailboxRing';

const repoRoot = fileURLToPath(new URL('../../../../', import.meta.url));

// Object-like `#define NAME value` lines with a numeric value, evaluated
// (earlier names substituted, `u` suffixes and casts dropped).  Include
// guards and function-like macros are skipped.
function headerDefines(rel: string): Map<string, number> {
  const text = readFileSync(join(repoRoot, rel), 'utf8');
  const out = new Map<string, number>();
  for (const line of text.split('\n')) {
    const m = /^#define\s+([A-Z0-9_]+)\s+(.+?)\s*(?:\/\/.*)?$/.exec(line);
    if (!m) continue;
    let expr = m[2]
      .replace(/\(\s*u?int\d+_t\s*\)/g, '')
      .replace(/\b(0x[0-9A-Fa-f]+|\d+)[uU]?[lL]*\b/g, '$1');
    expr = expr.replace(/\b[A-Z_][A-Z0-9_]*\b/g, (n) => (out.has(n) ? String(out.get(n)) : n));
    if (!/^[\s0-9A-Fa-fx()+\-*<>|&~]+$/.test(expr)) continue;
    out.set(m[1], Number(new Function(`return (${expr}) >>> 0;`)()));
  }
  return out;
}

// The module's numeric exports.
function tsNumbers(mod: Record<string, unknown>): Map<string, number> {
  const out = new Map<string, number>();
  for (const [k, v] of Object.entries(mod)) if (typeof v === 'number') out.set(k, v >>> 0);
  return out;
}

// How one TS module mirrors one header: `prefix` is dropped from the
// header's names; `tsOnly` lists the TS numbers that have no twin;
// `aliases` maps a TS name to a header name outside the prefix scheme;
// with `complete`, every prefixed header number must be mirrored too.
interface Mirror {
  prefix: string;
  tsOnly?: string[];
  aliases?: Record<string, string>;
  complete?: boolean;
  minMatched?: number;
}

// Every TS number must equal its header twin (see Mirror).
function compare(
  header: Map<string, number>,
  ts: Map<string, number>,
  { prefix, tsOnly = [], aliases = {}, complete = false, minMatched = 9 }: Mirror,
): void {
  let matched = 0;
  for (const [name, value] of ts) {
    const cName = aliases[name] ?? prefix + name;
    const twin = header.get(cName);
    if (twin === undefined) {
      expect(tsOnly, `${name} has no ${cName} in the header`).toContain(name);
      continue;
    }
    expect(value, `${name} vs ${cName}`).toBe(twin);
    matched++;
  }
  expect(matched).toBeGreaterThanOrEqual(minMatched);
  if (!complete) return;
  for (const name of header.keys()) {
    if (!name.startsWith(prefix)) continue;
    expect(ts.has(name.slice(prefix.length)), `${name} is not mirrored`).toBe(true);
  }
}

// The names of a module's numeric exports that start with `p`.
function keysWith(mod: Record<string, unknown>, p: string): string[] {
  return [...tsNumbers(mod).keys()].filter((k) => k.startsWith(p));
}

// The registry: every C header the page mirrors, and its TS twin.  A new
// shared-heap layout adds a row here.
const MIRRORS: { header: string; ts: string; mod: Record<string, unknown>; mirror: Mirror }[] = [
  {
    header: 'src/platform/wasm/em_shm_layout.h',
    ts: 'bus/shmLayout.ts',
    mod: Shm,
    mirror: { prefix: 'GS_', complete: true },
  },
  {
    header: 'src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h',
    ts: 'gpu/voodoo2Protocol.ts',
    mod: V2,
    // The draw-header words and the vertex sizing are struct-derived on
    // the C side.
    mirror: { prefix: 'V2GPU_', tsOnly: [...keysWith(V2, 'DH_'), 'VERTEX_BYTES', 'MAX_VERTS'] },
  },
  {
    header: 'src/core/peripherals/pci/cards/rage128_gpu_protocol.h',
    ts: 'gpu/rage128Protocol.ts',
    mod: R128,
    // The draw-header words are struct-derived on the C side.
    mirror: { prefix: 'R128GPU_', tsOnly: keysWith(R128, 'DH_') },
  },
  {
    header: 'src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h',
    ts: 'gpu/takeoverProtocol.ts (the control block both cards share)',
    mod: Takeover,
    mirror: { prefix: 'V2GPU_', tsOnly: keysWith(Takeover, 'DH_') },
  },
  {
    header: 'src/core/peripherals/pci/cards/rage128_gpu_protocol.h',
    ts: 'gpu/takeoverProtocol.ts (the control block both cards share)',
    mod: Takeover,
    mirror: { prefix: 'R128GPU_', tsOnly: keysWith(Takeover, 'DH_') },
  },
  {
    header: 'src/core/network/laserwriter_ring_protocol.h',
    ts: 'printer/platenProtocol.ts',
    mod: Lw,
    mirror: { prefix: 'LWRING_' },
  },
  {
    header: 'src/core/mailbox/mailbox.h',
    ts: 'bus/mailbox.ts',
    mod: Mbx,
    // The magic and version keep the header's older GS_MAILBOX_ names.
    mirror: {
      prefix: 'GS_MBX_',
      aliases: { MAGIC: 'GS_MAILBOX_MAGIC', VERSION: 'GS_MAILBOX_VERSION' },
    },
  },
  {
    header: 'src/core/mailbox/mailbox_ring.h',
    ts: 'bus/mailboxRing.ts',
    mod: MbxRing,
    mirror: { prefix: 'MBX_', complete: true, minMatched: 2 },
  },
];

describe('shared-heap layout mirrors', () => {
  it.each(MIRRORS)('$header and $ts agree', ({ header, mod, mirror }) => {
    compare(headerDefines(header), tsNumbers(mod), mirror);
  });

  it('the Voodoo2 vertex size is the header float count', () => {
    const h = headerDefines('src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h');
    expect(V2.VERTEX_BYTES).toBe(h.get('V2GPU_VERTEX_FLOATS')! * 4);
  });

  it('a header value the TS side does not share is caught', () => {
    const h = new Map([['GS_X', 1]]);
    expect(() => compare(h, new Map([['X', 2]]), { prefix: 'GS_', minMatched: 0 })).toThrow();
  });

  it('a TS number with no header twin is caught', () => {
    const h = new Map([['GS_X', 1]]);
    expect(() => compare(h, new Map([['Y', 1]]), { prefix: 'GS_', minMatched: 0 })).toThrow();
  });
});
