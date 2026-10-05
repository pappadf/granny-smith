import { describe, it, expect } from 'vitest';
import {
  canonicalParamName,
  planMediaFetch,
  findMember,
  splitContainer,
  MediaUrlError,
  interleaveHalves,
  urlMediaName,
} from '@/lib/mediaUrl';

const PAGE = 'https://pappadf.github.io/gs-pages/staging/';
const ROMS =
  'https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip';

describe('canonicalParamName', () => {
  it('matches names case-insensitively', () => {
    expect(canonicalParamName('ROM')).toBe('rom');
    expect(canonicalParamName('Rom')).toBe('rom');
    expect(canonicalParamName('HD0')).toBe('hd0');
    expect(canonicalParamName('Fd1')).toBe('fd1');
    expect(canonicalParamName('CD')).toBe('cd');
    expect(canonicalParamName('Model')).toBe('model');
  });
  it('maps bare hd / fd to the first bay / drive', () => {
    expect(canonicalParamName('HD')).toBe('hd0');
    expect(canonicalParamName('fd')).toBe('fd0');
  });
  it('rejects unrelated names', () => {
    expect(canonicalParamName('foo')).toBeNull();
    expect(canonicalParamName('hdx')).toBeNull();
  });
});

describe('splitContainer', () => {
  const split = (u: string) => splitContainer(new URL(u));
  it('splits at a container segment followed by more path', () => {
    const s = split('https://h/p/roms.zip/Mac%20IIci/iici.rom');
    expect(s?.containerPath).toEqual(['p', 'roms.zip']);
    expect(s?.memberPath).toEqual(['Mac%20IIci', 'iici.rom']);
  });
  it('a container as the last segment is just the file', () => {
    expect(split('https://h/p/roms.zip')).toBeNull();
    expect(split('https://h/p/roms.zip/')).toBeNull();
    expect(split('https://h/p/rom.bin')).toBeNull();
  });
  it('ignores the query and the fragment', () => {
    expect(split('https://h/p/a.img?x=roms.zip/y#b.zip/c')).toBeNull();
  });
  it('recognises the Mac archive extensions', () => {
    expect(split('https://h/Games.sit/Dark%20Castle.img')?.containerPath).toEqual(['Games.sit']);
    expect(split('https://h/Disk.hqx/x')?.containerPath).toEqual(['Disk.hqx']);
  });
});

describe('planMediaFetch', () => {
  it('a plain file is fetched as is, relative values resolving against the page', () => {
    const p = planMediaFetch('roms/Plus.rom', PAGE);
    expect(p.fetchUrl).toBe('https://pappadf.github.io/gs-pages/staging/roms/Plus.rom');
    expect(p.member).toBeNull();
    expect(p.fileName).toBe('Plus.rom');
  });
  it('a zip member: the container is fetched, the member decoded', () => {
    const p = planMediaFetch('https://h/roms.zip/Mac%20IIci/iici%26x.rom', PAGE);
    expect(p.fetchUrl).toBe('https://h/roms.zip');
    expect(p.member).toBe('Mac IIci/iici&x.rom');
    expect(p.container).toBe('zip');
    expect(p.fileName).toBe('iici&x.rom');
    expect(p.containerName).toBe('roms.zip');
  });
  it('a Mac-archive member keeps the container query string', () => {
    const p = planMediaFetch('https://h/Games.sit/Dark%20Castle.img?dl=1', PAGE);
    expect(p.fetchUrl).toBe('https://h/Games.sit?dl=1');
    expect(p.container).toBe('mac');
    expect(p.member).toBe('Dark Castle.img');
  });
  it('an http value on an https page is refused as mixed content', () => {
    expect(() => planMediaFetch('http://h/a.img', PAGE)).toThrow(/mixed content/);
  });
  it('http is fine on an http page', () => {
    const p = planMediaFetch('http://h/a.img', 'http://localhost:8080/');
    expect(p.fetchUrl).toBe('http://h/a.img');
  });
  it('refuses other schemes', () => {
    expect(() => planMediaFetch('ftp://h/a.img', PAGE)).toThrow(MediaUrlError);
  });
});

describe('planMediaFetch — no host gets routing of its own', () => {
  it('an archive.org /cors/ file is fetched exactly as given', () => {
    const u = 'https://archive.org/cors/AppleMacintoshSystem753/System7_5_3.img';
    expect(planMediaFetch(u, PAGE).fetchUrl).toBe(u);
  });
  it('an archive.org /download/ file is not rewritten', () => {
    const u = 'https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img';
    expect(planMediaFetch(u, PAGE).fetchUrl).toBe(u);
  });
  it("a file server's view_archive.php URL, query and all, is the request", () => {
    const u =
      'https://ia800908.us.archive.org/view_archive.php?archive=/12/items/R/R.zip' +
      '&file=9FEB69B3%20-%20Power%20Mac%206100%20%26%207100%20%26%208100.ROM';
    // As it arrives from ?ROM=…: percent-encoded as a whole, decoded once.
    const q = new URLSearchParams(`ROM=${encodeURIComponent(u)}&model=pm6100`);
    const p = planMediaFetch(q.get('ROM') as string, PAGE);
    expect(p.fetchUrl).toBe(u);
    expect(p.member).toBeNull();
  });
  it("an archive.org zip member is split like any other host's", () => {
    const p = planMediaFetch(`${ROMS}/368CADFE%20-%20Mac%20IIci.ROM`, PAGE);
    expect(p.fetchUrl).toBe(ROMS);
    expect(p.member).toBe('368CADFE - Mac IIci.ROM');
    expect(p.container).toBe('zip');
  });
  it('http archive.org is not upgraded (mixed content, like any host)', () => {
    expect(() => planMediaFetch('http://archive.org/download/X/a.img', PAGE)).toThrow(
      /mixed content/,
    );
  });
});

describe('urlMediaName', () => {
  it('is the slot and the local date and time, nothing from the URL', () => {
    expect(urlMediaName('hd0', new Date(2026, 9, 4, 7, 5, 9))).toBe('hd0_2026-10-04_07-05-09');
    expect(urlMediaName('fd1', new Date(2026, 0, 31, 23, 59, 0))).toBe('fd1_2026-01-31_23-59-00');
  });
  it('is a safe stored file name as it is', () => {
    expect(urlMediaName('cd', new Date(2026, 9, 4, 17, 42, 5))).toMatch(/^[A-Za-z0-9._-]+$/);
  });
});

describe('findMember', () => {
  const names = ['Mac IIci/iici.rom', 'Plus/Plus v3.ROM', 'readme.txt', 'Other/readme.txt'];
  it('exact path', () => expect(findMember(names, 'Mac IIci/iici.rom')).toBe('Mac IIci/iici.rom'));
  it('ignoring case', () => expect(findMember(names, 'plus/plus V3.rom')).toBe('Plus/Plus v3.ROM'));
  it('a unique base name', () => expect(findMember(names, 'iici.rom')).toBe('Mac IIci/iici.rom'));
  it('an ambiguous base name is not guessed', () =>
    expect(findMember(names, 'readme.txt')).toBe('readme.txt'));
  it('an ambiguous base name without an exact hit', () =>
    expect(findMember(names, 'Elsewhere/README.TXT')).toBeNull());
  it('absent', () => expect(findMember(names, 'nope.rom')).toBeNull());
});

describe('interleaveHalves', () => {
  it('takes even bytes from the first chip and odd bytes from the second', () => {
    const out = interleaveHalves(
      new Uint8Array([0x00, 0x02, 0x04]),
      new Uint8Array([0x01, 0x03, 0x05]),
    );
    expect(Array.from(out)).toEqual([0, 1, 2, 3, 4, 5]);
  });
});
