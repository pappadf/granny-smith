import { describe, it, expect } from 'vitest';
import {
  ROMS_DIR,
  VROMS_DIR,
  FD_DIR,
  HD_DIR,
  CD_DIR,
  CHECKPOINT_DIR,
  UPLOAD_DIR,
  CONFIG_DIR,
  RECENTS_PATH,
  bufferHasCheckpointSignature,
  fileHasCheckpointSignature,
} from '@/lib/opfsPaths';

describe('OPFS path constants', () => {
  it('match the C-side OPFS layout', () => {
    expect(ROMS_DIR).toBe('/opfs/images/rom');
    expect(VROMS_DIR).toBe('/opfs/images/vrom');
    expect(FD_DIR).toBe('/opfs/images/fd');
    expect(HD_DIR).toBe('/opfs/images/hd');
    expect(CD_DIR).toBe('/opfs/images/cd');
    expect(CHECKPOINT_DIR).toBe('/opfs/checkpoints');
    expect(UPLOAD_DIR).toBe('/opfs/upload');
    expect(CONFIG_DIR).toBe('/opfs/config');
    expect(RECENTS_PATH).toBe('/opfs/config/recent.json');
  });
});

describe('checkpoint signature detection', () => {
  function makeBuf(prefix: string, versionByte: number): Uint8Array {
    const buf = new Uint8Array(16);
    for (let i = 0; i < prefix.length; i++) buf[i] = prefix.charCodeAt(i);
    buf[7] = versionByte;
    return buf;
  }

  it('matches v2 signature (GSCHKPT2)', () => {
    expect(bufferHasCheckpointSignature(makeBuf('GSCHKPT', 0x32))).toBe(true);
  });
  it('matches v3 signature (GSCHKPT3)', () => {
    expect(bufferHasCheckpointSignature(makeBuf('GSCHKPT', 0x33))).toBe(true);
  });
  it('rejects unknown version byte', () => {
    expect(bufferHasCheckpointSignature(makeBuf('GSCHKPT', 0x34))).toBe(false);
  });
  it('rejects wrong prefix', () => {
    expect(bufferHasCheckpointSignature(makeBuf('OTHERPK', 0x32))).toBe(false);
  });
  it('rejects too-short buffer', () => {
    expect(bufferHasCheckpointSignature(new Uint8Array([0x47, 0x53]))).toBe(false);
  });

  // Exercising the File→signature path requires Blob.slice().arrayBuffer(),
  // which jsdom does not implement; assert the early-out for tiny files
  // instead. Real-File coverage lives in the e2e suite.
  it('fileHasCheckpointSignature returns false for tiny files', async () => {
    const file = new File([new Uint8Array([0x47, 0x53])], 'tiny.bin');
    expect(await fileHasCheckpointSignature(file)).toBe(false);
  });
});
