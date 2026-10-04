// The scratch area's housekeeping (lib/opfsPaths.ts): this tab's part of it,
// and the parts no live tab holds.

import { gsEval } from './emulator';
import { SCRATCH_DIR, SCRATCH_ID, SCRATCH_LOCK_PREFIX, TAB_SCRATCH_DIR } from '@/lib/opfsPaths';

// Take this tab's part of the scratch area, then remove what no live tab
// holds.  The lock is held before the directory exists, so a tab starting at
// the same moment never takes this one's part for an abandoned one.  Without
// Web Locks (an old browser) nothing is removed: a leftover costs quota,
// removing a live tab's file breaks its operation.
export async function claimScratch(): Promise<void> {
  const locks = typeof navigator !== 'undefined' ? navigator.locks : undefined;
  if (locks) {
    await new Promise<void>((granted) => {
      void locks.request(SCRATCH_LOCK_PREFIX + SCRATCH_ID, () => {
        granted();
        return new Promise<void>(() => {}); // held for the tab's life
      });
    });
  }
  await gsEval('files.mkdir', [TAB_SCRATCH_DIR]);
  if (!locks) return;

  const held = new Set(((await locks.query()).held ?? []).map((l) => l.name ?? ''));
  const others = [...held].some(
    (n) => n.startsWith(SCRATCH_LOCK_PREFIX) && n !== SCRATCH_LOCK_PREFIX + SCRATCH_ID,
  );
  const entries = await gsEval('files.list', [SCRATCH_DIR]);
  if (!Array.isArray(entries)) return;
  for (const e of entries as { name?: unknown; kind?: unknown }[]) {
    if (typeof e?.name !== 'string' || e.name === '.' || e.name === '..' || e.name === SCRATCH_ID)
      continue;
    // A tab's part goes when nobody holds its lock; a loose file (written
    // before parts existed) only when no other tab is alive to own it.
    const abandoned = e.kind === 'directory' ? !held.has(SCRATCH_LOCK_PREFIX + e.name) : !others;
    if (abandoned) await gsEval('files.rm', [`${SCRATCH_DIR}/${e.name}`]);
  }
}
