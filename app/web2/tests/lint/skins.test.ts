import { describe, it, expect } from 'vitest';
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { skins } from '@/skins/registry';
import { MANIFESTS } from '@/skins/manifests';

// Lint guard for the skins themselves: every folder is registered, every
// manifest is well formed, every asset URL is relative, and every webfont's
// licence is listed in THIRD_PARTY_NOTICES.md.  The token values are checked
// by tokens.test.ts (L-2) and contrast.test.ts; the sprites by
// sprite.test.ts.

const here = dirname(fileURLToPath(import.meta.url));
const WEB2 = join(here, '..', '..');
const SKINS_DIR = join(WEB2, 'src', 'skins');
const NOTICES = readFileSync(join(WEB2, '..', '..', 'THIRD_PARTY_NOTICES.md'), 'utf8');

const folders = readdirSync(SKINS_DIR).filter((e) => statSync(join(SKINS_DIR, e)).isDirectory());

describe('skins', () => {
  it('the registry lists exactly the skin folders, once each', () => {
    expect(skins).toBe(MANIFESTS);
    expect(skins.map((s) => s.id).sort()).toEqual([...folders].sort());
  });

  // A skin's token rules apply to that skin alone.  A bare :root would also
  // match every other skin, and each token another skin leaves undefined
  // would keep this skin's value instead of the neutral default (#241).
  it('no skin declares its tokens on the bare :root', () => {
    const bad: string[] = [];
    for (const f of folders) {
      const css = readFileSync(join(SKINS_DIR, f, 'tokens.css'), 'utf8').replace(
        /\/\*[\s\S]*?\*\//g,
        '',
      );
      for (const m of css.matchAll(/([^{}]+)\{/g))
        for (const sel of m[1].split(','))
          if (sel.trim() === ':root') bad.push(`${f}/tokens.css: ${m[1].trim()}`);
    }
    expect(bad).toEqual([]);
  });

  for (const s of MANIFESTS) {
    it(`${s.id}: a well-formed manifest`, () => {
      expect(s.id).toMatch(/^[a-z][a-z0-9]*(-[a-z0-9]+)*$/);
      expect(s.name.trim()).not.toBe('');
    });

    it(`${s.id}: its token stylesheet selects only its own skin`, () => {
      const css = readFileSync(join(SKINS_DIR, s.id, 'tokens.css'), 'utf8').replace(
        /\/\*[\s\S]*?\*\//g,
        '',
      );
      const others = [...css.matchAll(/data-skin='([^']+)'/g)].map((m) => m[1]);
      expect(others.filter((id) => id !== s.id)).toEqual([]);
      expect(css).toContain(`[data-skin='${s.id}']`);
      expect(css).toMatch(/--gs-color-scheme:\s*(light|dark);/);
    });

    it(`${s.id}: relative font URLs, and font licences listed`, () => {
      for (const u of (s.fonts ?? []).map((f) => f.src)) {
        expect(u, u).not.toMatch(/^(\/|[a-z]+:)/);
        expect(existsSync(join(WEB2, 'public', u)), u).toBe(true);
      }
      for (const f of s.fonts ?? []) expect(NOTICES, f.license).toContain(f.license);
    });
  }
});
