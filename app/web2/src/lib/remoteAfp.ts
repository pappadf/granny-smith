// The page's side of remote AFP volumes (src/core/network/afp_remote.h): what
// the provider in bus/remoteAfp.ts turns archive.org's answers into.  Pure functions, no fetching.
//
// The core asks for a URI and passes the URIs in a listing back unread, so
// the URI scheme is the page's own:
//
//   ia:collection/<collection>[/<bucket>]   an archive.org collection's items
//   ia:item/<item>[/<folder path>]          an item's files (a folder of them)
//   ia:file/<item>/<path>                   one of an item's files
//   ia:zip/<item>/<zip path>[/<folder>]     the members of a zip in an item
//   ia:member/<item>/<zip path>/<member>    one of those members
//
// Every `ia:` component is URI-encoded, so none holds a '/', a tab or a
// newline (the listing's separators).

// One line of a listing (afp_remote.h): a folder or a file.
export interface RemoteEntry {
  kind: 'd' | 'f';
  name: string;
  size: number;
  mtime: number; // Unix seconds, 0 when unknown
  type?: string; // four characters, or left to the core
  creator?: string;
  uri: string;
}

// The listing file the core reads.
export function toListing(entries: RemoteEntry[]): string {
  const lines = ['# kind\tname\tsize\tmtime\ttype\tcreator\turi'];
  for (const e of uniqueNames(entries)) {
    const type = e.type && e.type.length === 4 ? e.type : '';
    const creator = e.creator && e.creator.length === 4 ? e.creator : '';
    lines.push(
      [
        e.kind,
        e.name,
        Math.max(0, Math.floor(e.size)),
        Math.max(0, Math.floor(e.mtime)),
        type,
        creator,
        e.uri,
      ].join('\t'),
    );
  }
  return lines.join('\n') + '\n';
}

// A name as the Finder can show it: no path separators (':' is the Mac's,
// '/' the host's), no control characters, no tabs or newlines.
export function finderName(raw: string): string {
  // eslint-disable-next-line no-control-regex
  const s = raw.replace(/[\u0000-\u001f\u007f]/g, ' ').replace(/[:/\\]/g, '-');
  return s.replace(/\s+/g, ' ').trim();
}

// Names in a folder are unique, compared as the Finder compares them
// (ignoring case): a repeated one gets " 2", " 3", ... before its ending.
export function uniqueNames(entries: RemoteEntry[]): RemoteEntry[] {
  const seen = new Set<string>();
  const out: RemoteEntry[] = [];
  for (const e of entries) {
    const base = finderName(e.name);
    if (!base || base === '.' || base === '..') continue;
    let name = base;
    for (let n = 2; seen.has(name.toLowerCase()); n++) {
      const dot = base.lastIndexOf('.');
      name = dot > 0 ? `${base.slice(0, dot)} ${n}${base.slice(dot)}` : `${base} ${n}`;
    }
    seen.add(name.toLowerCase());
    out.push({ ...e, name });
  }
  return out;
}

// --- URIs ----------------------------------------------------------------------

export type IaUri =
  | { kind: 'collection'; collection: string; bucket?: string }
  | { kind: 'item'; item: string; folder: string }
  | { kind: 'file'; item: string; path: string }
  | { kind: 'zip'; item: string; zip: string; folder: string }
  | { kind: 'member'; item: string; zip: string; member: string };

const enc = encodeURIComponent;

export function iaUri(u: IaUri): string {
  switch (u.kind) {
    case 'collection':
      return (
        `ia:collection/${enc(u.collection)}` + (u.bucket !== undefined ? `/${enc(u.bucket)}` : '')
      );
    case 'item':
      return `ia:item/${enc(u.item)}` + (u.folder ? `/${enc(u.folder)}` : '');
    case 'file':
      return `ia:file/${enc(u.item)}/${enc(u.path)}`;
    case 'zip':
      return `ia:zip/${enc(u.item)}/${enc(u.zip)}` + (u.folder ? `/${enc(u.folder)}` : '');
    case 'member':
      return `ia:member/${enc(u.item)}/${enc(u.zip)}/${enc(u.member)}`;
  }
}

export function parseIaUri(uri: string): IaUri | null {
  const m = /^ia:([a-z]+)\/(.*)$/.exec(uri);
  if (!m) return null;
  let parts: string[];
  try {
    parts = m[2].split('/').map((p) => decodeURIComponent(p));
  } catch {
    return null;
  }
  if (!parts[0]) return null;
  switch (m[1]) {
    case 'collection':
      if (parts.length > 2) return null;
      return parts.length === 2
        ? { kind: 'collection', collection: parts[0], bucket: parts[1] }
        : { kind: 'collection', collection: parts[0] };
    case 'item':
      return parts.length <= 2 ? { kind: 'item', item: parts[0], folder: parts[1] ?? '' } : null;
    case 'file':
      return parts.length === 2 && parts[1]
        ? { kind: 'file', item: parts[0], path: parts[1] }
        : null;
    case 'zip':
      return parts.length >= 2 && parts.length <= 3 && parts[1]
        ? { kind: 'zip', item: parts[0], zip: parts[1], folder: parts[2] ?? '' }
        : null;
    case 'member':
      return parts.length === 3 && parts[1] && parts[2]
        ? { kind: 'member', item: parts[0], zip: parts[1], member: parts[2] }
        : null;
    default:
      return null;
  }
}

// Each component of a path URI-encoded, the '/'s kept.
function encPath(path: string): string {
  return path.split('/').map(enc).join('/');
}

const IA = 'https://archive.org';

// Where a fetch goes.  /cors/ serves an item's file with CORS headers (its
// /download/ redirect has none); a zip member is served by the datanode
// /download/ redirects to, which has them.
export function iaFetchUrl(u: IaUri): string | null {
  switch (u.kind) {
    case 'file':
      return `${IA}/cors/${enc(u.item)}/${encPath(u.path)}`;
    case 'member':
      return `${IA}/download/${enc(u.item)}/${encPath(u.zip)}/${enc(u.member)}`;
    default:
      return null;
  }
}

export function iaSearchUrl(collection: string, rows: number): string {
  const q = new URLSearchParams();
  q.set('q', `collection:(${collection})`);
  for (const f of ['identifier', 'title', 'publicdate']) q.append('fl[]', f);
  q.set('rows', String(rows));
  q.set('output', 'json');
  return `${IA}/advancedsearch.php?${q.toString()}`;
}

export function iaFilesUrl(item: string): string {
  return `${IA}/metadata/${enc(item)}/files`;
}

export function iaZipViewUrl(item: string, zip: string): string {
  return `${IA}/download/${enc(item)}/${encPath(zip)}/`;
}

// --- archive.org collections ----------------------------------------------------

export interface IaDoc {
  identifier: string;
  title?: string;
  publicdate?: string;
}

// A collection with more items than this is shown as folders by first letter.
export const BUCKET_OVER = 300;

function bucketOf(title: string): string {
  const c = title.trim().charAt(0).toUpperCase();
  if (c >= 'A' && c <= 'Z') return c;
  if (c >= '0' && c <= '9') return '0-9';
  return 'Other';
}

function docTitle(d: IaDoc): string {
  return (typeof d.title === 'string' && d.title.trim()) || d.identifier;
}

function docTime(d: IaDoc): number {
  const t = d.publicdate ? Date.parse(d.publicdate) : NaN;
  return Number.isFinite(t) ? Math.floor(t / 1000) : 0;
}

// The items' titles as their folders show them: without the words every
// title starts with ("Vintage Apple Macintosh Shareware Games Collection A"
// is "A" in that collection), as long as none is left empty.
export function shownTitles(docs: IaDoc[]): Map<string, string> {
  const words = docs.map((d) => docTitle(d).split(/\s+/));
  let common = 0;
  if (words.length > 1)
    for (; ; common++) {
      const w = words[0][common];
      if (w === undefined || !words.every((t) => t[common] === w && t.length > common + 1)) break;
    }
  const out = new Map<string, string>();
  docs.forEach((d, i) => out.set(d.identifier, words[i].slice(common).join(' ')));
  return out;
}

// A collection's listing: its items as folders, or -- a large one, without a
// bucket -- the letters its items' titles start with.
export function collectionListing(
  collection: string,
  docs: IaDoc[],
  bucket?: string,
): RemoteEntry[] {
  const valid = docs.filter((d) => typeof d.identifier === 'string' && d.identifier);
  const titles = shownTitles(valid);
  const title = (d: IaDoc) => titles.get(d.identifier) ?? d.identifier;
  const items = valid.sort((a, b) => title(a).localeCompare(title(b)));
  if (bucket === undefined && items.length > BUCKET_OVER) {
    const buckets = new Set(items.map((d) => bucketOf(title(d))));
    return [...buckets].sort().map((b) => ({
      kind: 'd',
      name: b,
      size: 0,
      mtime: 0,
      uri: iaUri({ kind: 'collection', collection, bucket: b }),
    }));
  }
  return items
    .filter((d) => bucket === undefined || bucketOf(title(d)) === bucket)
    .map((d) => ({
      kind: 'd',
      name: title(d),
      size: 0,
      mtime: docTime(d),
      uri: iaUri({ kind: 'item', item: d.identifier, folder: '' }),
    }));
}

// --- archive.org items --------------------------------------------------------------

export interface IaFile {
  name: string;
  source?: string;
  format?: string;
  size?: string | number;
  mtime?: string | number;
}

// What an item holds besides what was uploaded to it: archive.org's own
// metadata, thumbnails and derivatives.
const SKIP_FORMATS = new Set([
  'Metadata',
  'Item Tile',
  'Thumbnail',
  'JPEG Thumb',
  'Archive BitTorrent',
  'Emulator Screenshot',
  'JSON',
  'Spectrogram',
  'PNG',
]);

export function isUploadedFile(f: IaFile): boolean {
  if (!f || typeof f.name !== 'string' || !f.name) return false;
  if (f.source && f.source !== 'original') return false;
  if (f.format && SKIP_FORMATS.has(f.format)) return false;
  const base = f.name.slice(f.name.lastIndexOf('/') + 1);
  return (
    !base.startsWith('__ia_thumb') && !/_(meta\.(xml|sqlite)|files\.xml|reviews\.xml)$/.test(base)
  );
}

function num(v: unknown): number {
  const n = typeof v === 'number' ? v : Number(v);
  return Number.isFinite(n) && n > 0 ? n : 0;
}

// The children of `folder` ("" for the top) in a tree of paths: the folders
// that paths below it pass through, and its own files.
function treeChildren<T extends { path: string }>(
  all: T[],
  folder: string,
): { dirs: Map<string, number>; files: T[] } {
  const prefix = folder ? folder + '/' : '';
  const dirs = new Map<string, number>();
  const files: T[] = [];
  for (const f of all) {
    if (!f.path.startsWith(prefix)) continue;
    const rest = f.path.slice(prefix.length);
    const slash = rest.indexOf('/');
    if (slash < 0) {
      if (rest) files.push(f);
    } else if (slash > 0) {
      dirs.set(rest.slice(0, slash), 1);
    }
  }
  return { dirs, files };
}

// An item's folder: its sub-folders, and its files.  A zip larger than the
// core unpacks is shown as a folder of its members (archive.org lists them),
// so a 450 MB collection zip is browsed, not downloaded.
export function itemListing(
  item: string,
  files: IaFile[],
  folder: string,
  unpackLimit: number,
): RemoteEntry[] {
  const kept = files.filter(isUploadedFile).map((f) => ({ ...f, path: f.name }));
  const { dirs, files: own } = treeChildren(kept, folder);
  const prefix = folder ? folder + '/' : '';
  const out: RemoteEntry[] = [];
  for (const d of [...dirs.keys()].sort())
    out.push({
      kind: 'd',
      name: d,
      size: 0,
      mtime: 0,
      uri: iaUri({ kind: 'item', item, folder: prefix + d }),
    });
  for (const f of own.sort((a, b) => a.path.localeCompare(b.path))) {
    const name = f.path.slice(prefix.length);
    const size = num(f.size);
    const mtime = num(f.mtime);
    if (/\.zip$/i.test(name) && size > unpackLimit)
      out.push({
        kind: 'd',
        name,
        size: 0,
        mtime,
        uri: iaUri({ kind: 'zip', item, zip: f.path, folder: '' }),
      });
    else
      out.push({ kind: 'f', name, size, mtime, uri: iaUri({ kind: 'file', item, path: f.path }) });
  }
  return out;
}

// --- archive.org zip views ------------------------------------------------------------

export interface ZipMember {
  path: string;
  size: number;
  mtime: number;
}

const ENTITIES: Record<string, string> = {
  amp: '&',
  lt: '<',
  gt: '>',
  quot: '"',
  apos: "'",
  nbsp: ' ',
};

export function decodeEntities(s: string): string {
  return s.replace(/&(#x[0-9a-f]+|#[0-9]+|[a-z]+);/gi, (m, e: string) => {
    if (e[0] === '#') {
      const code =
        e[1] === 'x' || e[1] === 'X' ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      return Number.isFinite(code) ? String.fromCodePoint(code) : m;
    }
    return ENTITIES[e.toLowerCase()] ?? m;
  });
}

// archive.org's listing of a zip (view_archive.php): a table row per member,
// a link for each file, none for a folder.
export function parseZipView(html: string): ZipMember[] {
  const out: ZipMember[] = [];
  const row = /<tr><td><a href="[^"]*">([^<]*)<\/a><td>.*?<td>([^<]*)<td id="size">(\d*)/g;
  for (let m: RegExpExecArray | null; (m = row.exec(html));) {
    const path = decodeEntities(m[1]);
    if (!path || path.endsWith('/')) continue;
    const t = Date.parse(m[2].trim().replace(' ', 'T') + 'Z');
    out.push({ path, size: num(m[3]), mtime: Number.isFinite(t) ? Math.floor(t / 1000) : 0 });
  }
  return out;
}

// A zip's folder.  macOS's __MACOSX copies of the members' resource forks are
// left out (a member is fetched alone, without its fork).  At the top, a
// folder that is all the zip holds is opened: its contents are the listing.
export function zipListing(
  item: string,
  zip: string,
  members: ZipMember[],
  folder: string,
): RemoteEntry[] {
  const kept = members.filter(
    (m) => !/(^|\/)__MACOSX\//.test(m.path) && !/(^|\/)\.DS_Store$/.test(m.path),
  );
  let { dirs, files } = treeChildren(kept, folder);
  const top = !folder;
  while (top && dirs.size === 1 && files.length === 0) {
    const only = [...dirs.keys()][0];
    folder = folder ? `${folder}/${only}` : only;
    ({ dirs, files } = treeChildren(kept, folder));
  }
  const prefix = folder ? folder + '/' : '';
  const out: RemoteEntry[] = [];
  for (const d of [...dirs.keys()].sort())
    out.push({
      kind: 'd',
      name: d,
      size: 0,
      mtime: 0,
      uri: iaUri({ kind: 'zip', item, zip, folder: prefix + d }),
    });
  for (const f of files.sort((a, b) => a.path.localeCompare(b.path)))
    out.push({
      kind: 'f',
      name: f.path.slice(prefix.length),
      size: f.size,
      mtime: f.mtime,
      uri: iaUri({ kind: 'member', item, zip, member: f.path }),
    });
  return out;
}
