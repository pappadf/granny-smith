// Single OPFS mount at /opfs (mounted C-side in main()). All persistent
// images live under /opfs/images/<category>; user-uploaded staging at
// /opfs/upload; checkpoints at /opfs/checkpoints/<machine_id>-<created>/.

export const ROMS_DIR = '/opfs/images/rom';
export const VROMS_DIR = '/opfs/images/vrom';
export const FD_DIR = '/opfs/images/fd';
export const FDHD_DIR = '/opfs/images/fdhd';
export const HD_DIR = '/opfs/images/hd';
export const CD_DIR = '/opfs/images/cd';
export const CHECKPOINT_DIR = '/opfs/checkpoints';
export const UPLOAD_DIR = '/opfs/upload';
export const CONFIG_DIR = '/opfs/config';

export const RECENTS_PATH = '/opfs/config/recent.json';

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
