// Single OPFS mount at /opfs (mounted C-side in main()). All persistent
// images live under /opfs/images/<category>; files on their way there in
// /opfs/upload/.scratch; checkpoints at /opfs/checkpoints/<machine_id>-<created>/.

export const ROMS_DIR = '/opfs/images/rom';
export const VROMS_DIR = '/opfs/images/vrom';
// PCI expansion ROMs (*.prom).  A separate store from VROMS_DIR because
// the two are different objects with different identity rules — a vROM is
// keyed on a NuBus Format-Block CRC, a PROM on a PCI Data Structure — and
// the core offers them through separate registries.
export const PROMS_DIR = '/opfs/images/prom';
export const FD_DIR = '/opfs/images/fd';
export const FDHD_DIR = '/opfs/images/fdhd';
export const HD_DIR = '/opfs/images/hd';
export const CD_DIR = '/opfs/images/cd';
export const CHECKPOINT_DIR = '/opfs/checkpoints';
export const UPLOAD_DIR = '/opfs/upload';
// The scratch area: every file the page writes on its way somewhere else (an
// upload being probed, a URL download, a streamed import's .dmg.part, a Save
// State being downloaded) lives here, under a name no other operation uses,
// and is removed by the operation that wrote it on every exit.  Nothing is
// ever attached from it.  Files the user put in /opfs/upload themselves are
// not touched.
export const SCRATCH_DIR = `${UPLOAD_DIR}/.scratch`;

// This tab's part of it: a directory of its own, which the tab holds a Web
// Lock on (SCRATCH_LOCK_PREFIX + id) for as long as it lives.  A tab starting
// up removes only the parts no live tab holds -- what a tab closed
// mid-operation, or a crash, left behind (bus/scratch.ts) -- so a second tab
// never wipes the first one's files.
export const SCRATCH_ID = `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`;
export const SCRATCH_LOCK_PREFIX = 'gs-scratch-';
export const TAB_SCRATCH_DIR = `${SCRATCH_DIR}/${SCRATCH_ID}`;

let scratchSeq = 0;

// A scratch path for `name` that no other operation shares
// ("<nonce>-<name>" in this tab's part): two uploads of one name, or two URL
// boots of one slot, never write the same file.
export function scratchPath(name: string): string {
  const rand = Math.random().toString(36).slice(2, 6);
  const nonce = `${Date.now().toString(36)}${(scratchSeq++).toString(36)}${rand}`;
  return `${TAB_SCRATCH_DIR}/${nonce}-${name}`;
}

// Checkpoint file signatures (v2 = per-block RLE, v3 = whole-file RLE).
// First 7 bytes are shared; byte 7 is the version digit.
export const CHECKPOINT_MAGIC_PREFIX = 'GSCHKPT';
export const CHECKPOINT_MAGIC_PREFIX_BYTES = Array.from(CHECKPOINT_MAGIC_PREFIX).map((c) =>
  c.charCodeAt(0),
);

// Returns true if `buf` begins with a checkpoint signature (v2 or v3).
export function bufferHasCheckpointSignature(buf: Uint8Array): boolean {
  if (buf.length < 8) return false;
  for (let i = 0; i < 7; i++) {
    if (buf[i] !== CHECKPOINT_MAGIC_PREFIX_BYTES[i]) return false;
  }
  // Version digit '2' (0x32) or '3' (0x33).
  return buf[7] === 0x32 || buf[7] === 0x33;
}

export async function fileHasCheckpointSignature(file: File): Promise<boolean> {
  if (file.size < 8) return false;
  const head = new Uint8Array(await file.slice(0, 8).arrayBuffer());
  return bufferHasCheckpointSignature(head);
}
