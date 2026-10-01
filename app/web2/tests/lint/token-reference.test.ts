import { describe, it, expect } from 'vitest';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { TOKENS, type TokenSpec } from '@/styles/contract';
import { parseRules } from '../helpers/tokenCss';

// src/skins/TOKENS.md, the skin author's token reference, is generated from
// the contract and the default stylesheets; this test fails when it is
// stale.  Regenerate with:  GS_WRITE_TOKEN_REFERENCE=1 npx vitest run
// tests/lint/token-reference.test.ts

const here = dirname(fileURLToPath(import.meta.url));
const SRC = join(here, '..', '..', 'src');
const OUT = join(SRC, 'skins', 'TOKENS.md');

// The :root defaults of scale.css and components.css.
function defaults(): Map<string, string> {
  const out = new Map<string, string>();
  for (const f of ['styles/scale.css', 'styles/components.css'])
    for (const r of parseRules(readFileSync(join(SRC, f), 'utf8')))
      if (r.selectors.includes(':root')) for (const [k, v] of r.decls) out.set(k, v);
  return out;
}

// One table cell: pipes escaped, whitespace collapsed.
const cell = (s: string) => s.replace(/\s+/g, ' ').replace(/\|/g, '\\|');

function render(): string {
  const dflt = defaults();
  const value = (t: TokenSpec) =>
    t.derived
      ? `\`${t.derived}\``
      : t.layer === 'semantic'
        ? '(the skin)'
        : `\`${dflt.get(t.name) ?? ''}\``;
  const section = (title: string, intro: string, layer: TokenSpec['layer']) => {
    const rows = TOKENS.filter((t) => t.layer === layer).map(
      (t) =>
        `| \`${t.name}\` | ${t.kind} | ${t.perScheme ? 'yes' : ''} | ${cell(value(t))} | ${cell(t.doc)} |`,
    );
    return [
      `## ${title}`,
      '',
      intro,
      '',
      '| Token | Kind | Per scheme | Default | Purpose |',
      '|---|---|---|---|---|',
      ...rows,
      '',
    ].join('\n');
  };
  return [
    '# Token reference',
    '',
    'Generated from `src/styles/contract.ts`, `src/styles/scale.css` and',
    '`src/styles/components.css` by `tests/lint/token-reference.test.ts`;',
    'do not edit by hand. See [README.md](README.md) for how a skin uses them.',
    '',
    section(
      'Semantic tokens',
      'Defined by every skin in every scheme it has (`skins/<id>/tokens.css`). A token with a default (derived) may be left out.',
      'semantic',
    ),
    section(
      'Scale tokens',
      'Scheme-independent; a skin may override any of them for its `[data-skin]` scope.',
      'scale',
    ),
    section(
      'Component tokens',
      "One component's knobs, defaulting to semantic or scale tokens; a skin may override any of them.",
      'component',
    ),
  ].join('\n');
}

describe('token reference', () => {
  it('src/skins/TOKENS.md is up to date', () => {
    const text = render();
    if (process.env.GS_WRITE_TOKEN_REFERENCE) writeFileSync(OUT, text);
    expect(existsSync(OUT)).toBe(true);
    expect(readFileSync(OUT, 'utf8')).toBe(text);
  });
});
