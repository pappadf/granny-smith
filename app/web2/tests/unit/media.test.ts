import { describe, it, expect } from 'vitest';
import { MEDIA_TYPES, type GsEval } from '@/lib/media';
import { VROMS_DIR } from '@/lib/opfsPaths';

// The OPFS store is content-addressed for both media types: CPU ROMs are
// stored by content id, vROMs by the declaration ROM's Format-Block CRC. Discovery is
// content-based (the core's offer registry), so the on-disk name never
// matters — but the stable hash name is what keeps re-uploads idempotent.

// gsEval stub returning a fixed vrom.identify payload (the current shape:
// content facts only, no canonical_name).
const vromIdentify: GsEval = async (evalPath: string, args?: unknown[]) => {
  expect(evalPath).toBe('catalog.vroms.identify');
  expect(args).toEqual(['/opfs/upload/my_weird.bin']);
  return {
    recognised: true,
    card_id: 'mdc_8_24',
    compatible: ['mdc_8_24'],
    size: 32768,
    crc: '0xd1629664',
  };
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
    const unrecognised: GsEval = async () => ({ recognised: false, size: 32768 });
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

// rom.identify payloads (src/core/memory/rom.c): an intact supported ROM, a
// damaged dump of the same ROM, and a known ROM of an unemulated machine.
const TNT = {
  recognised: true,
  supported: true,
  compatible: ['pm7500', 'pm8500', 'pm9500'],
  name: 'Power Macintosh 7500/8500/9500 ROM (v1)',
  size: 4 * 1024 * 1024,
  kind: 'ppc',
  id: '96cd923d-c241cd82bf90797a',
  intact: true,
  reason: '',
};
const romIdentify =
  (payload: object): GsEval =>
  async (evalPath: string) => {
    expect(evalPath).toBe('machine.rom.identify');
    return payload;
  };

describe('rom media descriptor', () => {
  it('stores an intact, supported ROM under its content id', async () => {
    const result = await MEDIA_TYPES.rom.validate('/opfs/upload/x.rom', romIdentify(TNT));
    expect(result.valid).toBe(true);
    expect(result.reject).toBeUndefined();
    expect(MEDIA_TYPES.rom.nameFn?.('x.rom', result.info)).toBe('96cd923d-c241cd82bf90797a');
  });

  it('refuses a damaged dump of a known ROM, saying which part fails', async () => {
    const damaged = {
      ...TNT,
      intact: false,
      reason: 'PowerPC section does not verify (byte lanes 0)',
    };
    const result = await MEDIA_TYPES.rom.validate('/opfs/upload/x.rom', romIdentify(damaged));
    expect(result.valid).toBe(false);
    expect(result.reject).toContain('Power Macintosh 7500/8500/9500 ROM (v1)');
    expect(result.reject).toContain('PowerPC section does not verify');
  });

  it('refuses a known ROM of a machine that is not emulated, by name', async () => {
    const other = { ...TNT, supported: false, compatible: [], name: 'Power Mac 6500 ROM' };
    const result = await MEDIA_TYPES.rom.validate('/opfs/upload/x.rom', romIdentify(other));
    expect(result.valid).toBe(false);
    expect(result.reject).toContain('Power Mac 6500 ROM');
    expect(result.reject).toContain('does not emulate');
  });

  it('lets an unrecognised file fall through to the next media type', async () => {
    const result = await MEDIA_TYPES.rom.validate(
      '/opfs/upload/x.bin',
      romIdentify({ recognised: false }),
    );
    expect(result.valid).toBe(false);
    expect(result.reject).toBeUndefined();
  });
});

describe('fd media descriptor', () => {
  const sized = (size: number) =>
    (async (path: string) => (path === 'files.path_size' ? size : null)) as unknown as GsEval;

  it('accepts raw and Disk Copy 4.2 floppy sizes, with and without tag data', async () => {
    for (const size of [
      409600,
      819200,
      1474560, // raw
      409684,
      819284,
      1474644, // Disk Copy 4.2, no tags
      419284,
      838484, // Disk Copy 4.2 with 12 tag bytes per sector (GCR disks)
    ])
      expect((await MEDIA_TYPES.fd.validate('/x', sized(size))).valid, String(size)).toBe(true);
  });

  it('rejects other sizes', async () => {
    for (const size of [0, 409601, 20971520])
      expect((await MEDIA_TYPES.fd.validate('/x', sized(size))).valid, String(size)).toBe(false);
  });
});

// catalog.proms.identify payloads (src/core/memory/prom.c).  A structurally
// valid expansion ROM the emulator cannot use is refused with its reason
// instead of falling through to the permissive hard-disk probe, which stored
// two 128 KB Rage 128 ROMs as "hard disks".
describe('prom media descriptor', () => {
  const promIdentify =
    (payload: object): GsEval =>
    async (evalPath: string) => {
      expect(evalPath).toBe('catalog.proms.identify');
      return payload;
    };

  it('refuses a valid ROM of a card it does not emulate', async () => {
    const result = await MEDIA_TYPES.prom.validate(
      '/opfs/upload/rage128.prom',
      promIdentify({
        recognised: false,
        id: '1002-5245-7935',
        intact: true,
        size: 131072,
        reason: 'a valid Open Firmware expansion ROM, but no catalog row claims it',
      }),
    );
    expect(result.valid).toBe(false);
    expect(result.reject).toBe(
      'is a PCI expansion ROM (1002-5245-7935) for a card Granny Smith does not emulate',
    );
  });

  it('refuses a PC option ROM with the reason', async () => {
    const reason =
      'an expansion ROM, but its code type is not Open Firmware (a PC/x86 option ROM cannot drive a Macintosh card)';
    const result = await MEDIA_TYPES.prom.validate(
      '/opfs/upload/pc.rom',
      promIdentify({ recognised: false, size: 32768, reason }),
    );
    expect(result.reject).toBe(`is ${reason}`);
  });

  it('lets anything else go on to the other probes', async () => {
    const result = await MEDIA_TYPES.prom.validate(
      '/opfs/upload/disk.img',
      promIdentify({ recognised: false, size: 131072, reason: 'not a PCI expansion ROM' }),
    );
    expect(result.valid).toBe(false);
    expect(result.reject).toBeUndefined();
  });
});
