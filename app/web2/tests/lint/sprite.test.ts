import { describe, it, expect } from 'vitest';
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { skins } from '@/skins/registry';

// Lint guard: every icon sprite defines exactly the registry's ids.  An
// external <use href="sprite.svg#i-x"> cannot fall back symbol by symbol, so
// a skin's own sprite missing one id would leave that icon blank; an id no
// registry name reaches is dead weight.  The registry is the IconName union
// in src/lib/icons.ts.

const here = dirname(fileURLToPath(import.meta.url));
const WEB2 = join(here, '..', '..');

// The IconName union's members.
function registry(): string[] {
  const src = readFileSync(join(WEB2, 'src/lib/icons.ts'), 'utf8');
  const union = src.match(/export type IconName =([\s\S]*?);/);
  if (!union) throw new Error('no IconName union in lib/icons.ts');
  return [...union[1].matchAll(/'([a-z0-9-]+)'/g)].map((m) => m[1]).sort();
}

// The sprite's symbol ids, without the i- prefix.
function symbols(file: string): string[] {
  const svg = readFileSync(file, 'utf8');
  return [...svg.matchAll(/<symbol\s+id="i-([a-z0-9-]+)"/g)].map((m) => m[1]).sort();
}

describe('icon sprites', () => {
  const names = registry();

  it('the registry is not empty and has no duplicates', () => {
    expect(names.length).toBeGreaterThan(0);
    expect(new Set(names).size).toBe(names.length);
  });

  // The default sprite, and every skin's own (skins/<id>/sprite.svg).
  const sprites = [
    'src/icons/sprite.svg',
    ...skins.map((s) => `src/skins/${s.id}/sprite.svg`).filter((f) => existsSync(join(WEB2, f))),
  ];
  for (const rel of sprites) {
    it(`${rel} defines exactly the registry's ids`, () => {
      expect(symbols(join(WEB2, rel))).toEqual(names);
    });
  }
});
