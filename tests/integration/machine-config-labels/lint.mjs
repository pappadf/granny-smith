// Lint the labels of every catalog.profile tree read from stdin (one JSON
// object per line, other lines ignored).  Exit 1 with one line per offence.
import { readFileSync } from 'node:fs';

// Words that keep their capitals inside a sentence-case label: proper nouns
// and product names (Apple's own spelling).
const PROPER = new Set([
  'Apple', 'Macintosh', 'AppleColor', 'AppleTalk', 'NuBus', 'PCI', 'SCSI', 'ATA', 'CD-ROM',
  'SuperDrive', 'ProFile', 'PowerPC', 'VGA', 'SVGA', 'NTSC', 'PAL', 'RGB', 'ID', 'MB', 'KB',
  'GB', 'MHz', 'L2', 'Voodoo2', 'Rage', 'Pro', 'ATI',
]);
// Chip and firmware names: detail text, never a label of their own (L2).
const CHIPS = new Set([
  'Control', 'Chaos', 'Ariel', 'Ariel II', 'DAFB', 'CIVIC', 'RBV', 'VCI', 'Valkyrie', 'MESH',
  'Curio', 'Cirrus Logic 54M30', 'ATI Rage Pro', 'Mach64', 'Heathrow', 'Grand Central',
]);
const FORBIDDEN = [
  [/\b(FD|HD)\d\b/, 'a drive index (FD0, HD1)'],
  [/SLOT\d_|_PCI\d/, 'a firmware slot name'],
  [/\d+x\d+/, 'an "x" for "×"'],
  [/Colour/i, 'British spelling'],
  [/on-?board/i, '"on-board": an emulator artefact or a chip'],
  [/\(generic/i, 'a "(generic …)" stand-in'],
];

const problems = [];

// A word that may keep a capital: proper noun, acronym, number-like, a
// single letter (slot C), or Apple's 8•24-style names.
function mayCapitalise(w) {
  const bare = w.replace(/^[(“"]+|[)”",.:;]+$/g, '');
  return (
    !bare || PROPER.has(bare) || /\d/.test(bare) || /^[A-Z]$/.test(bare) ||
    /^[A-Z0-9-]+$/.test(bare) || /[•″]/.test(bare)
  );
}

// Sentence case: each part (split at " · ") starts with a capital or a
// number; a clause after ", " may start either way ("Rear bracket, top",
// "640 × 480, Thousands of colors"); no other word is capitalised unless it
// may be.
function sentenceCase(label) {
  for (const part of label.split(' · ')) {
    const clauses = part.split(', ');
    for (const [i, clause] of clauses.entries()) {
      const words = clause.split(/\s+/).filter(Boolean);
      if (!words.length) continue;
      if (i === 0 && /^[a-z]/.test(words[0])) return false;
      for (const w of words.slice(1)) if (/^[(“"]?[A-Z]/.test(w) && !mayCapitalise(w)) return false;
    }
  }
  return true;
}

function check(model, where, label, { sentence = true } = {}) {
  if (typeof label !== 'string' || !label) {
    problems.push(`${model}: ${where}: no label`);
    return;
  }
  for (const [re, why] of FORBIDDEN) if (re.test(label)) problems.push(`${model}: ${where}: "${label}" — ${why}`);
  if (CHIPS.has(label)) problems.push(`${model}: ${where}: "${label}" — a chip name as a label`);
  if (sentence && !sentenceCase(label)) problems.push(`${model}: ${where}: "${label}" — not sentence case`);
}

function unique(model, where, labels) {
  const seen = new Set();
  for (const l of labels) {
    if (seen.has(l)) problems.push(`${model}: ${where}: "${l}" is used twice`);
    seen.add(l);
  }
}

function option(model, where, o) {
  check(model, `${where} ${o.id}`, o.label);
  for (const v of o.values ?? []) check(model, `${where} ${o.id}=${v.id}`, v.label);
  unique(model, `${where} ${o.id} values`, (o.values ?? []).map((v) => v.label));
}

let models = 0;
for (const line of readFileSync(0, 'utf8').split('\n')) {
  if (!line.startsWith('{')) continue;
  const p = JSON.parse(line);
  const m = p.id;
  models++;
  // Model names are Apple's marketing names, product capitals and all.
  check(m, 'name', p.name, { sentence: false });
  for (const o of p.options ?? []) option(m, 'option', o);
  unique(m, 'options', (p.options ?? []).map((o) => o.label));
  for (const f of p.floppies ?? []) {
    check(m, `floppy ${f.id}`, f.label);
    for (const t of f.types ?? []) check(m, `floppy ${f.id} type ${t.id}`, t.label);
  }
  for (const b of p.storage ?? []) {
    check(m, `bus ${b.id}`, b.label);
    for (const u of b.units ?? []) check(m, `bus ${b.id} unit ${u.unit}`, u.label);
    for (const a of b.accepts ?? []) check(m, `bus ${b.id} accepts ${a.id}`, a.label);
    unique(m, `bus ${b.id} units`, (b.units ?? []).map((u) => u.label));
  }
  for (const s of p.slots ?? []) check(m, `slot ${s.id}`, s.label);
  for (const c of p.cards ?? []) {
    // A card is its product name as sold.
    check(m, `card ${c.id}`, c.label, { sentence: false });
    for (const o of c.options ?? []) option(m, `card ${c.id} option`, o);
    for (const [mon, modes] of Object.entries(c.modes ?? {}))
      for (const md of modes) check(m, `card ${c.id} ${mon} mode ${md.id}`, md.label);
  }
  unique(m, 'cards', (p.cards ?? []).map((c) => c.label));
  const b = p.displays?.builtin;
  if (b) {
    check(m, 'built-in video', b.label);
    for (const o of b.options ?? []) option(m, 'built-in video option', o);
  }
  // Monitors are product names too.
  for (const mon of p.monitors ?? []) check(m, `monitor ${mon.id}`, mon.label, { sentence: false });
  // L9: no two devices or slots of one machine share a label.
  unique(m, 'devices and slots', [
    ...(p.floppies ?? []).map((f) => f.label),
    ...(p.storage ?? []).map((x) => x.label),
    ...(p.slots ?? []).map((s) => s.label),
    ...(b ? [b.label] : []),
  ]);
}

if (models < 20) problems.push(`only ${models} models read: the dump failed`);
if (problems.length) {
  for (const p of problems) console.log(`FAIL: ${p}`);
  process.exit(1);
}
console.log(`machine-config-labels: ${models} models clean`);
