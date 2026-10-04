import { describe, it, expect } from 'vitest';
import {
  canonicalParamName,
  planMediaFetch,
  findMember,
  splitContainer,
  MediaUrlError,
  interleaveHalves,
} from '@/lib/mediaUrl';

const PAGE = 'https://pappadf.github.io/gs-pages/staging/';
const ROMS =
  'https://archive.org/download/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip';

// A metadata fetch that must not be reached.
const noFetch = async (): Promise<unknown> => {
  throw new Error('unexpected metadata fetch');
};

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

describe('planMediaFetch — generic hosts', () => {
  it('a plain file is fetched as is, relative values resolving against the page', async () => {
    const p = await planMediaFetch('roms/Plus.rom', PAGE, noFetch);
    expect(p.fetchUrl).toBe('https://pappadf.github.io/gs-pages/staging/roms/Plus.rom');
    expect(p.member).toBeNull();
    expect(p.fileName).toBe('Plus.rom');
  });
  it('a zip member: the container is fetched, the member decoded', async () => {
    const p = await planMediaFetch('https://h/roms.zip/Mac%20IIci/iici%26x.rom', PAGE, noFetch);
    expect(p.fetchUrl).toBe('https://h/roms.zip');
    expect(p.member).toBe('Mac IIci/iici&x.rom');
    expect(p.container).toBe('zip');
    expect(p.fileName).toBe('iici&x.rom');
    expect(p.containerName).toBe('roms.zip');
  });
  it('a Mac-archive member keeps the container query string', async () => {
    const p = await planMediaFetch('https://h/Games.sit/Dark%20Castle.img?dl=1', PAGE, noFetch);
    expect(p.fetchUrl).toBe('https://h/Games.sit?dl=1');
    expect(p.container).toBe('mac');
    expect(p.member).toBe('Dark Castle.img');
  });
  it('an http value on an https page is refused as mixed content', async () => {
    await expect(planMediaFetch('http://h/a.img', PAGE, noFetch)).rejects.toThrow(/mixed content/);
  });
  it('http is fine on an http page', async () => {
    const p = await planMediaFetch('http://h/a.img', 'http://localhost:8080/', noFetch);
    expect(p.fetchUrl).toBe('http://h/a.img');
  });
  it('refuses other schemes', async () => {
    await expect(planMediaFetch('ftp://h/a.img', PAGE, noFetch)).rejects.toBeInstanceOf(
      MediaUrlError,
    );
  });
});

describe('planMediaFetch — archive.org', () => {
  it('a plain download goes through /cors/ (the file servers send no CORS)', async () => {
    const p = await planMediaFetch(
      'https://archive.org/download/AppleMacintoshSystem753/System7_5_3.img',
      PAGE,
      noFetch,
    );
    expect(p.fetchUrl).toBe('https://archive.org/cors/AppleMacintoshSystem753/System7_5_3.img');
    expect(p.member).toBeNull();
    expect(p.fileName).toBe('System7_5_3.img');
  });
  it('a zip member is left to archive.org to extract', async () => {
    const p = await planMediaFetch(`${ROMS}/368CADFE%20-%20Mac%20IIci.ROM`, PAGE, noFetch);
    expect(p.fetchUrl).toBe(`${ROMS}/368CADFE%20-%20Mac%20IIci.ROM`);
    expect(p.member).toBeNull();
    expect(p.fileName).toBe('368CADFE - Mac IIci.ROM');
    expect(p.containerName).toBe('mac_rom_archive_-_as_of_8-19-2011.zip');
  });
  it('a member name typed with spaces (decoded by URLSearchParams) still works', async () => {
    const q = new URLSearchParams(`ROM=${ROMS}/368CADFE - Mac IIci.ROM`);
    const p = await planMediaFetch(q.get('ROM') as string, PAGE, noFetch);
    expect(p.fetchUrl).toBe(`${ROMS}/368CADFE%20-%20Mac%20IIci.ROM`);
  });
  it('an encoded & in a member survives; an unencoded one splits the query', async () => {
    const enc = new URLSearchParams(`ROM=${ROMS}/420DBFF3%20-%20Quadra%20700%26900.ROM&x=1`);
    const meta = async (): Promise<unknown> => ({
      server: 'ia1.us.archive.org',
      dir: '/1/items/R',
    });
    const p = await planMediaFetch(enc.get('ROM') as string, PAGE, meta);
    expect(p.fileName).toBe('420DBFF3 - Quadra 700&900.ROM');
    const raw = new URLSearchParams(`ROM=${ROMS}/420DBFF3 - Quadra 700&900.ROM`);
    expect(raw.get('ROM')).toBe(`${ROMS}/420DBFF3 - Quadra 700`);
  });
  it("a zip member named with & goes to the file server's view_archive.php directly", async () => {
    let asked = '';
    const meta = async (u: string): Promise<unknown> => {
      asked = u;
      return {
        server: 'ia800908.us.archive.org',
        dir: '/12/items/mac_rom_archive_-_as_of_8-19-2011',
      };
    };
    const p = await planMediaFetch(
      `${ROMS}/9FEB69B3%20-%20Power%20Mac%206100%20%26%207100%20%26%208100.ROM`,
      PAGE,
      meta,
    );
    expect(asked).toBe('https://archive.org/metadata/mac_rom_archive_-_as_of_8-19-2011');
    expect(p.fetchUrl).toBe(
      'https://ia800908.us.archive.org/view_archive.php?archive=' +
        '/12/items/mac_rom_archive_-_as_of_8-19-2011/mac_rom_archive_-_as_of_8-19-2011.zip' +
        '&file=9FEB69B3%20-%20Power%20Mac%206100%20%26%207100%20%26%208100.ROM',
    );
    expect(p.member).toBeNull();
    expect(p.fileName).toBe('9FEB69B3 - Power Mac 6100 & 7100 & 8100.ROM');
    expect(p.containerName).toBe('mac_rom_archive_-_as_of_8-19-2011.zip');
  });
  it('a nested member path with & keeps its folders in file=', async () => {
    const meta = async (): Promise<unknown> => ({
      server: 'ia1.us.archive.org',
      dir: '/1/items/X',
    });
    const p = await planMediaFetch(
      'https://archive.org/download/X/My%20Disks.zip/A%20%26%20B/c%2Bd.img',
      PAGE,
      meta,
    );
    expect(p.fetchUrl).toBe(
      'https://ia1.us.archive.org/view_archive.php?archive=/1/items/X/My%20Disks.zip' +
        '&file=A%20%26%20B%2Fc%2Bd.img',
    );
    expect(p.fileName).toBe('c+d.img');
  });
  it('a member with & in an item whose metadata has no server is refused', async () => {
    const meta = async (): Promise<unknown> => ({});
    await expect(planMediaFetch(`${ROMS}/A%20%26%20B.ROM`, PAGE, meta)).rejects.toThrow(
      MediaUrlError,
    );
  });
  it('an http archive.org URL is upgraded to https', async () => {
    const p = await planMediaFetch('http://archive.org/download/X/a.img', PAGE, noFetch);
    expect(p.fetchUrl).toBe('https://archive.org/cors/X/a.img');
  });
  it('/details/<item>/<file> is that file', async () => {
    const p = await planMediaFetch(
      'https://archive.org/details/AppleMacintoshSystem701/System7_0_1.img',
      PAGE,
      noFetch,
    );
    expect(p.fetchUrl).toBe('https://archive.org/cors/AppleMacintoshSystem701/System7_0_1.img');
  });
  it('/details/<item> resolves to the single media file in the item', async () => {
    let asked = '';
    const meta = async (u: string) => {
      asked = u;
      return {
        files: [
          { name: 'System7_5_3.img', source: 'original' },
          { name: '00_screenshot.png', source: 'original' },
          { name: 'AppleMacintoshSystem753_meta.xml', source: 'original' },
          { name: 'System7_5_3.img.torrent', source: 'metadata' },
        ],
      };
    };
    const p = await planMediaFetch(
      'https://archive.org/details/AppleMacintoshSystem753',
      PAGE,
      meta,
    );
    expect(asked).toBe('https://archive.org/metadata/AppleMacintoshSystem753');
    expect(p.fetchUrl).toBe('https://archive.org/cors/AppleMacintoshSystem753/System7_5_3.img');
  });
  it('/details/<item> with several candidates names them', async () => {
    const meta = async () => ({
      files: [
        { name: 'a.img', source: 'original' },
        { name: 'b.dsk', source: 'original' },
      ],
    });
    await expect(planMediaFetch('https://archive.org/details/X', PAGE, meta)).rejects.toThrow(
      /2 candidate files.*a\.img, b\.dsk/,
    );
  });
  it('/download/<item> without a file is refused', async () => {
    await expect(
      planMediaFetch('https://archive.org/download/X/', PAGE, noFetch),
    ).rejects.toBeInstanceOf(MediaUrlError);
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
