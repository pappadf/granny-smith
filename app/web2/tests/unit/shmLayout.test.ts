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
import * as Lw from '@/printer/platenProtocol';

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

// Every TS number must equal its header twin; `tsOnly` lists the ones that
// have none.  With `complete`, every header number must be mirrored too.
function compare(
  header: Map<string, number>,
  prefix: string,
  ts: Map<string, number>,
  tsOnly: string[] = [],
  complete = false,
): void {
  let matched = 0;
  for (const [name, value] of ts) {
    const twin = header.get(prefix + name);
    if (twin === undefined) {
      expect(tsOnly, `${name} has no ${prefix}${name} in the header`).toContain(name);
      continue;
    }
    expect(value, `${name} vs ${prefix}${name}`).toBe(twin);
    matched++;
  }
  expect(matched).toBeGreaterThan(8);
  if (!complete) return;
  for (const name of header.keys()) {
    if (!name.startsWith(prefix)) continue;
    expect(ts.has(name.slice(prefix.length)), `${name} is not mirrored`).toBe(true);
  }
}

describe('shared-heap layout mirrors', () => {
  it('em_shm_layout.h and bus/shmLayout.ts agree, both ways', () => {
    const h = headerDefines('src/platform/wasm/em_shm_layout.h');
    compare(h, 'GS_', tsNumbers(Shm), [], true);
  });

  it('voodoo2_gpu_protocol.h and gpu/voodoo2Protocol.ts agree', () => {
    const h = headerDefines('src/core/peripherals/pci/cards/voodoo2_gpu_protocol.h');
    // PROTOCOL_VERSION is V2GPU_PROTOCOL_VERSION; the draw-header words and
    // the vertex sizing are struct-derived on the C side.
    const tsOnly = [
      ...[...tsNumbers(V2).keys()].filter((k) => k.startsWith('DH_')),
      'VERTEX_BYTES',
      'MAX_VERTS',
    ];
    compare(h, 'V2GPU_', tsNumbers(V2), tsOnly);
    expect(V2.VERTEX_BYTES).toBe(h.get('V2GPU_VERTEX_FLOATS')! * 4);
  });

  it('laserwriter_ring_protocol.h and printer/platenProtocol.ts agree', () => {
    const h = headerDefines('src/core/network/laserwriter_ring_protocol.h');
    compare(h, 'LWRING_', tsNumbers(Lw));
  });

  it('a header value the TS side does not share is caught', () => {
    const h = new Map([['GS_X', 1]]);
    expect(() => compare(h, 'GS_', new Map([['X', 2]]))).toThrow();
  });
});
