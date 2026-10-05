// Machine-description trees for the configuration dialog's tests.
//
// tests/fixtures/profiles/*.json are catalog.profile() answers dumped from
// the core (build/headless/gs-headless, `echo "${catalog.profile("iix")}"`),
// so the dialog is tested against the shapes the core actually emits: a
// slotless machine (Plus), NuBus without built-in video (IIx), built-in
// video and sockets (IIci), PCI with built-in video (7500) and without
// (9500), two SCSI buses (Network Server), ATA (G3) and ProFile (Lisa).
// Regenerate them after a tree change.
import plus from '../fixtures/profiles/plus.json';
import iix from '../fixtures/profiles/iix.json';
import iici from '../fixtures/profiles/iici.json';
import pm7500 from '../fixtures/profiles/pm7500.json';
import pm9500 from '../fixtures/profiles/pm9500.json';
import ans500 from '../fixtures/profiles/ans500.json';
import pmg3dt from '../fixtures/profiles/pmg3dt.json';
import lisa from '../fixtures/profiles/lisa.json';

const TREES: Record<string, unknown> = { plus, iix, iici, pm7500, pm9500, ans500, pmg3dt, lisa };

// A fresh copy of model `id`'s tree, or undefined.
export function tree(id: string): Record<string, unknown> | undefined {
  const t = TREES[id];
  return t ? (JSON.parse(JSON.stringify(t)) as Record<string, unknown>) : undefined;
}

export const TREE_IDS = Object.keys(TREES);

// rom.identify's answer for a ROM that boots `compatible`.
export function romIdentity(id: string, compatible: string[], variant?: string) {
  return {
    recognised: true,
    supported: true,
    intact: true,
    id,
    name: `${id} ROM`,
    ...(variant ? { variant } : {}),
    compatible,
    size: 512 * 1024,
  };
}
