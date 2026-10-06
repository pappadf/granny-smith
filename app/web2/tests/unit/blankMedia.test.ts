import { describe, it, expect } from 'vitest';
import {
  parseBlankValue,
  blankUrlHash,
  blankDiskName,
  floppyBlankPlan,
  hardDiskBlankPlan,
} from '@/lib/blankMedia';
import type { BlankDisk } from '@/bus/profile';

// A SCSI bus's blank disks as the core lists them (machine_config.c).
const SCSI: BlankDisk[] = [
  {
    label: '20 MB (HD20SC)',
    method: 'files.hd_create',
    arg: '21411840',
    name: 'blank_20MB',
    ext: '.dmg',
  },
  {
    label: '80 MB (HD80SC)',
    method: 'files.hd_create',
    arg: '80061440',
    name: 'blank_80MB',
    ext: '.dmg',
  },
];
// The Lisa's ProFile port's.
const PROFILE: BlankDisk[] = [
  {
    label: '5 MB ProFile (9728 blocks)',
    method: 'files.profile_create',
    arg: '9728',
    name: 'blank_profile_5MB',
    ext: '.image',
  },
  {
    label: '10 MB Widget (19448 blocks)',
    method: 'files.profile_create',
    arg: '19448',
    name: 'blank_profile_10MB',
    ext: '.image',
  },
];

describe('parseBlankValue', () => {
  it('is null for a URL to fetch', () => {
    expect(parseBlankValue('https://host/disk.img')).toBeNull();
    expect(parseBlankValue('disks/blank.img')).toBeNull();
    expect(parseBlankValue('')).toBeNull();
  });

  it('takes blank: and blank!: in any case, trimming the spec', () => {
    expect(parseBlankValue('blank:HD230SC')).toEqual({ spec: 'HD230SC', fresh: false });
    expect(parseBlankValue(' BLANK: 80mb ')).toEqual({ spec: '80mb', fresh: false });
    expect(parseBlankValue('blank!:100m')).toEqual({ spec: '100m', fresh: true });
    expect(parseBlankValue('blank:')).toEqual({ spec: '', fresh: false });
  });
});

describe('blankUrlHash', () => {
  const media: Array<[string, string]> = [
    ['rom', 'plus.rom'],
    ['hd0', 'blank:20mb'],
  ];

  it('is stable: 8 hex digits, the same for the same parameters', () => {
    const h = blankUrlHash(media);
    expect(h).toMatch(/^[0-9a-f]{8}$/);
    expect(blankUrlHash([...media])).toBe(h);
  });

  it('hashes blank!: as blank:, so the forced disk replaces the reused one', () => {
    expect(
      blankUrlHash([
        ['rom', 'plus.rom'],
        ['hd0', 'blank!:20mb'],
      ]),
    ).toBe(blankUrlHash(media));
  });

  it('differs when a media parameter differs', () => {
    expect(
      blankUrlHash([
        ['rom', 'se.rom'],
        ['hd0', 'blank:20mb'],
      ]),
    ).not.toBe(blankUrlHash(media));
    expect(
      blankUrlHash([
        ['rom', 'plus.rom'],
        ['hd0', 'blank:40mb'],
      ]),
    ).not.toBe(blankUrlHash(media));
  });
});

describe('blankDiskName', () => {
  it('is blank_<spec>_<slot>_<hash><ext>', () => {
    expect(blankDiskName('hd0', 'HD230SC', '1a2b3c4d', '.dmg')).toBe(
      'blank_HD230SC_hd0_1a2b3c4d.dmg',
    );
    expect(blankDiskName('fd1', '800k', '00000000', '.dsk')).toBe('blank_800k_fd1_00000000.dsk');
  });

  it('keeps only safe characters of the spec', () => {
    expect(blankDiskName('hd1', '1.5 g/b', 'ff', '')).toBe('blank_1.5_g_b_hd1_ff');
    expect(blankDiskName('hd0', '', 'ff', '.dmg')).toBe('blank_disk_hd0_ff.dmg');
  });
});

describe('floppyBlankPlan', () => {
  it('creates 800K and 1440K floppies with files.fd_create', () => {
    expect(floppyBlankPlan('800k')).toEqual({
      ok: true,
      method: 'files.fd_create',
      arg: false,
      ext: '.dsk',
    });
    expect(floppyBlankPlan('800KB')).toMatchObject({ ok: true, arg: false });
    for (const s of ['1440k', '1440KB', '1.44mb', '1.4MB'])
      expect(floppyBlankPlan(s)).toMatchObject({ ok: true, arg: true });
  });

  it('refuses 400K and anything else, saying what is offered', () => {
    expect(floppyBlankPlan('400k')).toMatchObject({
      ok: false,
      reason: expect.stringMatching(/400K/),
    });
    expect(floppyBlankPlan('720k')).toMatchObject({
      ok: false,
      reason: expect.stringMatching(/800k or 1440k/),
    });
    expect(floppyBlankPlan('')).toMatchObject({ ok: false });
  });
});

describe('hardDiskBlankPlan', () => {
  it('hands the spec unchanged to files.hd_create on a SCSI bus', () => {
    for (const spec of ['HD230SC', '80mb', '100m', 'bogus'])
      expect(hardDiskBlankPlan(spec, SCSI)).toEqual({
        ok: true,
        method: 'files.hd_create',
        arg: spec,
        ext: '.dmg',
      });
  });

  it('takes the ProFile sizes on the Lisa', () => {
    expect(hardDiskBlankPlan('5mb', PROFILE)).toEqual({
      ok: true,
      method: 'files.profile_create',
      arg: '9728',
      ext: '.image',
    });
    expect(hardDiskBlankPlan('10MB', PROFILE)).toMatchObject({ ok: true, arg: '19448' });
    expect(hardDiskBlankPlan('20mb', PROFILE)).toMatchObject({
      ok: false,
      reason: expect.stringMatching(/5mb or 10mb/),
    });
  });

  it('refuses an empty spec, options, and a drive that takes no blank disk', () => {
    expect(hardDiskBlankPlan('', SCSI)).toMatchObject({ ok: false });
    expect(hardDiskBlankPlan('HD80SC,hfs', SCSI)).toMatchObject({
      ok: false,
      reason: expect.stringMatching(/options/),
    });
    expect(hardDiskBlankPlan('80mb', [])).toMatchObject({ ok: false });
  });
});
