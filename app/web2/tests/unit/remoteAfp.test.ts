import { describe, it, expect } from 'vitest';
import {
  BUCKET_OVER,
  collectionListing,
  finderName,
  iaFetchUrl,
  iaUri,
  itemListing,
  parseIaUri,
  parseZipView,
  toListing,
  uniqueNames,
  zipListing,
  type IaUri,
} from '@/lib/remoteAfp';

describe('ia URIs', () => {
  it('round-trip every kind, with any characters in a component', () => {
    const all: IaUri[] = [
      { kind: 'collection', collection: 'hypercardstacks' },
      { kind: 'collection', collection: 'hypercardstacks', bucket: '0-9' },
      { kind: 'item', item: 'x', folder: '' },
      { kind: 'item', item: 'x', folder: 'Disk 1/Games ƒ' },
      { kind: 'file', item: 'x', path: 'a b/c\td.sit' },
      { kind: 'zip', item: 'x', zip: 'big.zip', folder: 'g/h' },
      { kind: 'member', item: 'x', zip: 'dir/big.zip', member: 'g/GL Tron.sit' },
    ];
    for (const u of all) {
      const s = iaUri(u);
      expect(s).not.toMatch(/[\t\n]/);
      expect(parseIaUri(s)).toEqual(u);
    }
  });

  it('refuses what they do not name', () => {
    expect(parseIaUri('dir:///tmp')).toBeNull();
    expect(parseIaUri('ia:nope/x')).toBeNull();
    expect(parseIaUri('ia:file/x')).toBeNull();
    expect(parseIaUri('ia:item/%zz')).toBeNull();
  });

  it('fetch a file through /cors/ and a member through /download/', () => {
    expect(iaFetchUrl({ kind: 'file', item: 'it', path: 'A b/c.sit' })).toBe(
      'https://archive.org/cors/it/A%20b/c.sit',
    );
    expect(iaFetchUrl({ kind: 'member', item: 'it', zip: 'z.zip', member: 'g/GL Tron.sit' })).toBe(
      'https://archive.org/download/it/z.zip/g%2FGL%20Tron.sit',
    );
    expect(iaFetchUrl({ kind: 'item', item: 'it', folder: '' })).toBeNull();
  });
});

describe('listings', () => {
  it('are one tab-separated line per entry, after a comment', () => {
    const text = toListing([
      { kind: 'd', name: 'Games', size: 0, mtime: 0, uri: 'ia:item/g' },
      {
        kind: 'f',
        name: 'Read Me',
        size: 12.7,
        mtime: 99,
        type: 'TEXT',
        creator: 'ttxt',
        uri: 'ia:file/g/r',
      },
      { kind: 'f', name: 'x', size: 1, mtime: 0, type: 'TOOLONG', uri: 'ia:file/g/x' },
    ]);
    expect(text.split('\n')).toEqual([
      '# kind\tname\tsize\tmtime\ttype\tcreator\turi',
      'd\tGames\t0\t0\t\t\tia:item/g',
      'f\tRead Me\t12\t99\tTEXT\tttxt\tia:file/g/r',
      'f\tx\t1\t0\t\t\tia:file/g/x',
      '',
    ]);
  });

  it('give names the Finder can show, each once', () => {
    expect(finderName(' Disk: 1/2\tok ')).toBe('Disk- 1-2 ok');
    const names = uniqueNames([
      { kind: 'f', name: 'Game.sit', size: 0, mtime: 0, uri: 'a' },
      { kind: 'f', name: 'game.sit', size: 0, mtime: 0, uri: 'b' },
      { kind: 'd', name: 'Folder', size: 0, mtime: 0, uri: 'c' },
      { kind: 'd', name: 'Folder', size: 0, mtime: 0, uri: 'd' },
      { kind: 'd', name: '..', size: 0, mtime: 0, uri: 'e' },
    ]).map((e) => e.name);
    expect(names).toEqual(['Game.sit', 'game 2.sit', 'Folder', 'Folder 2']);
  });
});

describe('collections', () => {
  it('list their items as folders, by title', () => {
    const out = collectionListing('c', [
      { identifier: 'b', title: 'Beta', publicdate: '2017-12-20T19:00:00Z' },
      { identifier: 'a' },
    ]);
    expect(out.map((e) => [e.kind, e.name, e.uri])).toEqual([
      ['d', 'a', 'ia:item/a'],
      ['d', 'Beta', 'ia:item/b'],
    ]);
    expect(out[1].mtime).toBe(Date.UTC(2017, 11, 20, 19) / 1000);
  });

  it('leave out the words every title starts with', () => {
    const out = collectionListing('c', [
      { identifier: 'g', title: 'Vintage Shareware Games Collection G' },
      { identifier: 'n', title: 'Vintage Shareware Games Collection 0 to 9' },
    ]);
    expect(out.map((e) => e.name)).toEqual(['0 to 9', 'G']);
    // ... but never all of one
    const one = collectionListing('c', [
      { identifier: 'a', title: 'Games' },
      { identifier: 'b', title: 'Games Two' },
    ]);
    expect(one.map((e) => e.name)).toEqual(['Games', 'Games Two']);
  });

  it('show a large one as folders by first letter', () => {
    const docs = Array.from({ length: BUCKET_OVER + 1 }, (_, i) => ({
      identifier: `id${i}`,
      title: i % 3 === 0 ? `apple ${i}` : i % 3 === 1 ? `9 lives ${i}` : `"quoted" ${i}`,
    }));
    const top = collectionListing('big', docs);
    expect(top.map((e) => e.name)).toEqual(['0-9', 'A', 'Other']);
    expect(top[1].uri).toBe('ia:collection/big/A');
    const a = collectionListing('big', docs, 'A');
    expect(a.length).toBe(Math.ceil((BUCKET_OVER + 1) / 3));
    expect(a.every((e) => e.name.startsWith('apple'))).toBe(true);
  });
});

describe('items', () => {
  const files = [
    {
      name: 'disk.img',
      source: 'original',
      size: '3145728',
      mtime: '1513794850',
      format: 'ISO Image',
    },
    { name: 'Docs/Manual.txt', source: 'original', size: '10', format: 'Text' },
    { name: 'Docs/Deep/More.txt', source: 'original', size: '5' },
    { name: 'huge.zip', source: 'original', size: String(500 << 20), format: 'ZIP' },
    { name: 'small.zip', source: 'original', size: '1000', format: 'ZIP' },
    { name: 'x_meta.xml', source: 'original', format: 'Metadata' },
    { name: '__ia_thumb.jpg', source: 'original', format: 'Item Tile' },
    { name: 'x_archive.torrent', source: 'metadata', format: 'Archive BitTorrent' },
    { name: 'disk.img.png', source: 'derivative', format: 'PNG' },
  ];

  it('list what was uploaded, a folder level at a time', () => {
    const top = itemListing('x', files, '', 64 << 20);
    expect(top.map((e) => [e.kind, e.name, e.size, e.uri])).toEqual([
      ['d', 'Docs', 0, 'ia:item/x/Docs'],
      ['f', 'disk.img', 3145728, 'ia:file/x/disk.img'],
      ['d', 'huge.zip', 0, 'ia:zip/x/huge.zip'], // too big to unpack: browsed
      ['f', 'small.zip', 1000, 'ia:file/x/small.zip'], // the core unpacks it
    ]);
    expect(top[1].mtime).toBe(1513794850);
    const docs = itemListing('x', files, 'Docs', 64 << 20);
    expect(docs.map((e) => [e.kind, e.name, e.uri])).toEqual([
      ['d', 'Deep', 'ia:item/x/Docs%2FDeep'],
      ['f', 'Manual.txt', 'ia:file/x/Docs%2FManual.txt'],
    ]);
  });
});

describe('zip views', () => {
  const html =
    '<table><tr><th>file<th>as jpg<th>timestamp<th>size</tr>\n' +
    '<tr><td>__MACOSX/<td><td>2016-10-24 06:12<td id="size"></tr>\n' +
    '<tr><td><a href="//archive.org/download/I/z.zip/__MACOSX%2F._g">__MACOSX/._g</a><td><td>2016-10-17 19:23<td id="size">120</tr>\n' +
    '<tr><td>g/<td><td>2016-10-24 06:12<td id="size"></tr>\n' +
    '<tr><td><a href="//archive.org/download/I/z.zip/g%2FGL%20Tron.sit">g/GL Tron.sit</a><td><td>2016-10-17 19:19<td id="size">4096</tr>\n' +
    '<tr><td><a href="//archive.org/download/I/z.zip/g%2FM%26M.sit">g/M&amp;M.sit</a><td><td>2016-10-17 19:19<td id="size">7</tr>\n' +
    '<tr><td><a href="//archive.org/download/I/z.zip/g%2F.DS_Store">g/.DS_Store</a><td><td>2016-10-17 19:19<td id="size">7</tr>\n' +
    '</table>';

  it('are read into members, folders left implied', () => {
    const m = parseZipView(html);
    expect(m.map((x) => x.path)).toEqual([
      '__MACOSX/._g',
      'g/GL Tron.sit',
      'g/M&M.sit',
      'g/.DS_Store',
    ]);
    expect(m[1]).toEqual({
      path: 'g/GL Tron.sit',
      size: 4096,
      mtime: Date.UTC(2016, 9, 17, 19, 19) / 1000,
    });
  });

  it('list a folder of members, without macOS bookkeeping', () => {
    const m = parseZipView(html);
    const inG = [
      ['f', 'GL Tron.sit', 'ia:member/I/z.zip/g%2FGL%20Tron.sit'],
      ['f', 'M&M.sit', 'ia:member/I/z.zip/g%2FM%26M.sit'],
    ];
    // the folder that is all the zip holds is opened at the top
    expect(zipListing('I', 'z.zip', m, '').map((e) => [e.kind, e.name, e.uri])).toEqual(inG);
    const more = [...m, { path: 'h/x.sit', size: 1, mtime: 0 }];
    expect(zipListing('I', 'z.zip', more, '').map((e) => [e.kind, e.name, e.uri])).toEqual([
      ['d', 'g', 'ia:zip/I/z.zip/g'],
      ['d', 'h', 'ia:zip/I/z.zip/h'],
    ]);
    expect(zipListing('I', 'z.zip', m, 'g').map((e) => [e.kind, e.name, e.uri])).toEqual([
      ['f', 'GL Tron.sit', 'ia:member/I/z.zip/g%2FGL%20Tron.sit'],
      ['f', 'M&M.sit', 'ia:member/I/z.zip/g%2FM%26M.sit'],
    ]);
  });
});
