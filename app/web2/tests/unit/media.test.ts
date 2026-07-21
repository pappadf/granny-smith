import { describe, it, expect } from 'vitest';
import { MEDIA_TYPES, type GsEval } from '@/lib/media';
import { VROMS_DIR } from '@/lib/opfsPaths';

// The OPFS store is content-addressed for both media types
// (proposal-content-addressed-rom-provisioning.md §3.6a): CPU ROMs are stored
// by checksum, vROMs by the declaration ROM's Format-Block CRC. Discovery is
// content-based (the core's offer registry), so the on-disk name never
// matters — but the stable hash name is what keeps re-uploads idempotent.

// gsEval stub returning a fixed vrom.identify payload (the post-proposal
// shape: content facts only, no canonical_name).
const vromIdentify: GsEval = async (evalPath: string, args?: unknown[]) => {
  expect(evalPath).toBe('machine.vrom.identify');
  expect(args).toEqual(['/opfs/upload/my_weird.bin']);
  return JSON.stringify({
    recognised: true,
    card_id: 'mdc_8_24',
    compatible: ['mdc_8_24'],
    size: 32768,
    crc: '0xd1629664',
  });
};

describe('vrom media descriptor', () => {
  it('validate() maps the identify payload to cardId/compatible/checksum', async () => {
    const result = await MEDIA_TYPES.vrom.validate('/opfs/upload/my_weird.bin', vromIdentify);
    expect(result.valid).toBe(true);
    expect(result.info?.cardId).toBe('mdc_8_24');
    expect(result.info?.compatible).toEqual(['mdc_8_24']);
    expect(result.info?.checksum).toBe('0xd1629664');
  });

  it('validate() rejects unrecognised files', async () => {
    const unrecognised: GsEval = async () => JSON.stringify({ recognised: false, size: 32768 });
    const result = await MEDIA_TYPES.vrom.validate('/opfs/upload/junk.bin', unrecognised);
    expect(result.valid).toBe(false);
  });

  it('nameFn stores an uploaded vROM under its content hash (CRC, no 0x)', async () => {
    const result = await MEDIA_TYPES.vrom.validate('/opfs/upload/my_weird.bin', vromIdentify);
    const name = MEDIA_TYPES.vrom.nameFn!('my_weird.bin', result.info);
    expect(name).toBe('d1629664');
    // The persisted path is content-addressed — the user's upload name is
    // discarded, exactly like the CPU-ROM store.
    expect(`${MEDIA_TYPES.vrom.persistDir}/${name}`).toBe(`${VROMS_DIR}/d1629664`);
  });

  it('nameFn falls back to the original name without identify info', () => {
    expect(MEDIA_TYPES.vrom.nameFn!('fallback.vrom', undefined)).toBe('fallback.vrom');
  });
});
