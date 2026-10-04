// URL-media addressing: how a `?rom=` / `?hd0=` / ... value becomes the
// request to make and, when the value continues through a container file,
// the member to take out of it.  Pure apart from the metadata lookup (whose
// fetch is injected), so it is unit-tested directly
// (tests/unit/mediaUrl.test.ts).  bus/urlMedia.ts does the fetching.
//
// A value is a URL whose PATH may continue through a container:
//
//   https://host/roms.zip/Mac%20IIci.ROM          zip member
//   https://host/disks/Games.sit/Dark%20Castle.img Mac-archive member
//
// Splitting rule, applied to the path only (never the query or fragment):
// the first segment with a container extension that is NOT the last segment
// ends the container URL; the rest, percent-decoded, is the member path.  A
// container as the last segment is just the file.
//
// archive.org gets its own routing, because its file servers do not send
// CORS headers (a page on another origin cannot read them):
//   /download/<item>/<file>             -> /cors/<item>/<file> (CORS, whole file)
//   /download/<item>/<x.zip>/<member>   -> unchanged: archive.org extracts the
//                                          member server-side, with CORS;
//                                          a member named with & # + % goes
//                                          to the file server's
//                                          view_archive.php directly (via
//                                          the metadata API)
//   /details/<item>[/<file>]            -> the item's single media file (via
//                                          the metadata API), or <file>

// Extensions that end a container segment when a member path follows.
const CONTAINER_EXT = /\.(zip|sit|sea|cpt|hqx|bin)$/i;
const ZIP_EXT = /\.zip$/i;

// Extensions an archive.org item's original files carry when they are media
// (what /details/<item> may resolve to).
const MEDIA_EXT =
  /\.(img|image|dsk|disk|hfv|dc42|dmg|smi|iso|toast|cdr|rom|bin|zip|sit|sea|cpt|hqx)$/i;

const ARCHIVE_ORG_HOSTS = new Set(['archive.org', 'www.archive.org']);

// A value that cannot be turned into a request (the message is user-facing).
export class MediaUrlError extends Error {}

// How to fetch one URL-media value.
export interface MediaFetchPlan {
  // The URL to GET.
  fetchUrl: string;
  // Member to take out of the fetched container, or null when the response
  // is the file itself (including an archive.org server-side extraction).
  member: string | null;
  // The container's kind when `member` is set.
  container: 'zip' | 'mac' | null;
  // The file's own name (the member's base name, or the URL's last segment),
  // decoded: what it is stored under and what messages call it.
  fileName: string;
  // The container's file name when the value named a member, for messages.
  containerName: string | null;
}

// The metadata lookup /details/<item> needs: GET a URL, return parsed JSON.
export type JsonFetch = (url: string) => Promise<unknown>;

// Canonical parameter names: case-insensitive, `hd` / `fd` meaning the
// first bay / drive.  Returns null for names that are not URL-media keys.
export function canonicalParamName(name: string): string | null {
  const n = name.toLowerCase();
  if (n === 'hd') return 'hd0';
  if (n === 'fd') return 'fd0';
  if (/^(rom|vrom|model|speed|cd)$/.test(n) || /^(fd|hd)\d+$/.test(n)) return n;
  return null;
}

// Decode one path segment, keeping it as typed when it is not valid
// percent-encoding (a literal '%' a user left unencoded).
function decodeSegment(seg: string): string {
  try {
    return decodeURIComponent(seg);
  } catch {
    return seg;
  }
}

// Split a URL's path at the first container segment that has more path
// after it.  Returns null when the URL names a plain file.
export function splitContainer(url: URL): { containerPath: string[]; memberPath: string[] } | null {
  const segs = url.pathname.split('/').slice(1);
  for (let i = 0; i < segs.length - 1; i++) {
    if (CONTAINER_EXT.test(decodeSegment(segs[i])) && segs.slice(i + 1).some((s) => s !== '')) {
      return { containerPath: segs.slice(0, i + 1), memberPath: segs.slice(i + 1) };
    }
  }
  return null;
}

// The plan for a URL on any host but archive.org.
function planGeneric(url: URL): MediaFetchPlan {
  const split = splitContainer(url);
  if (!split) {
    const last = url.pathname.split('/').pop() ?? '';
    return {
      fetchUrl: url.href,
      member: null,
      container: null,
      fileName: decodeSegment(last),
      containerName: null,
    };
  }
  const container = new URL(url.href);
  container.pathname = '/' + split.containerPath.join('/');
  const containerName = decodeSegment(split.containerPath[split.containerPath.length - 1]);
  const member = split.memberPath.filter((s) => s !== '').map(decodeSegment);
  return {
    fetchUrl: container.href,
    member: member.join('/'),
    container: ZIP_EXT.test(containerName) ? 'zip' : 'mac',
    fileName: member[member.length - 1],
    containerName,
  };
}

// Pick the one media file among an archive.org item's original files.
function pickItemFile(item: string, meta: unknown): string {
  const files = (meta as { files?: Array<{ name?: string; source?: string }> })?.files;
  if (!Array.isArray(files) || files.length === 0)
    throw new MediaUrlError(`archive.org item "${item}" was not found or has no files`);
  const media = files
    .filter((f) => f.source === 'original' && typeof f.name === 'string')
    .map((f) => f.name as string)
    .filter((n) => MEDIA_EXT.test(n));
  if (media.length === 1) return media[0];
  if (media.length === 0)
    throw new MediaUrlError(`archive.org item "${item}" has no disk image or ROM file`);
  throw new MediaUrlError(
    `archive.org item "${item}" has ${media.length} candidate files; name one: ` +
      media.slice(0, 5).join(', ') +
      (media.length > 5 ? ', ...' : ''),
  );
}

// Characters that end or change a query-string value when left unescaped.
const QUERY_SPECIAL = /[&#+%]/;

// The view_archive.php URL that extracts `member` from the zip `zip` (a path
// inside archive.org item `item`), on the file server the item's metadata
// names, with both parameters escaped.
async function viewArchiveUrl(
  item: string,
  zip: string,
  member: string,
  fetchJson: JsonFetch,
): Promise<string> {
  const meta = (await fetchJson(`https://archive.org/metadata/${encodeURIComponent(item)}`)) as {
    server?: unknown;
    dir?: unknown;
  } | null;
  const server = meta?.server;
  const dir = meta?.dir;
  if (typeof server !== 'string' || typeof dir !== 'string' || !/^[\w.-]+$/.test(server))
    throw new MediaUrlError(`archive.org item "${item}" was not found or has no file server`);
  const archive = encodeURIComponent(`${dir}/${zip}`).replace(/%2F/g, '/');
  return `https://${server}/view_archive.php?archive=${archive}&file=${encodeURIComponent(member)}`;
}

// The plan for an archive.org URL.
async function planArchiveOrg(url: URL, fetchJson: JsonFetch): Promise<MediaFetchPlan> {
  const segs = url.pathname.split('/').slice(1);
  const kind = segs[0];
  const item = segs[1] ?? '';
  if ((kind !== 'details' && kind !== 'download') || !item) return planGeneric(url);
  let rest = segs.slice(2);
  if (rest.length > 0 && rest[rest.length - 1] === '') rest = rest.slice(0, -1);
  if (rest.length === 0) {
    if (kind === 'download')
      throw new MediaUrlError(`archive.org URL names the item "${item}" but no file in it`);
    const meta = await fetchJson(`https://archive.org/metadata/${encodeURIComponent(item)}`);
    rest = pickItemFile(item, meta)
      .split('/')
      .map((s) => encodeURIComponent(s));
  }
  const download = new URL(`https://archive.org/download/${item}/${rest.join('/')}`);
  const split = splitContainer(download);
  if (split && ZIP_EXT.test(decodeSegment(split.containerPath[split.containerPath.length - 1]))) {
    // archive.org extracts a zip member itself (view_archive.php, with CORS).
    const member = split.memberPath.filter((s) => s !== '').map(decodeSegment);
    const plan: MediaFetchPlan = {
      fetchUrl: download.href,
      member: null,
      container: null,
      fileName: member[member.length - 1],
      containerName: decodeSegment(split.containerPath[split.containerPath.length - 1]),
    };
    // /download/ answers a member request with a redirect to view_archive.php
    // on the item's file server; for a browser's request that redirect
    // carries the member name unescaped, so a name with `&` ("9FEB69B3 -
    // Power Mac 6100 & 7100 & 8100.ROM") is cut at the `&` and the
    // extraction fails (503).  Such a name goes to view_archive.php directly,
    // on the server and directory the item's metadata names.
    if (member.some((m) => QUERY_SPECIAL.test(m))) {
      const zip = split.containerPath.slice(2).map(decodeSegment).join('/');
      plan.fetchUrl = await viewArchiveUrl(item, zip, member.join('/'), fetchJson);
    }
    return plan;
  }
  // Anything else is fetched whole through /cors/, the one file path that
  // carries CORS headers; a Mac-archive member is then taken out locally.
  const plan = planGeneric(download);
  const cors = new URL(plan.fetchUrl);
  cors.pathname = cors.pathname.replace(/^\/download\//, '/cors/');
  return { ...plan, fetchUrl: cors.href };
}

// Turn a URL-media value into its fetch plan.  `base` is the page's URL
// (relative values resolve against it).  Throws MediaUrlError when the value
// cannot work: not a URL, http on an https page (the browser blocks mixed
// content), an archive.org item with no single media file.
export async function planMediaFetch(
  value: string,
  base: string,
  fetchJson: JsonFetch,
): Promise<MediaFetchPlan> {
  let url: URL;
  try {
    url = new URL(value.trim(), base);
  } catch {
    throw new MediaUrlError(`not a valid URL: ${value}`);
  }
  if (url.protocol !== 'https:' && url.protocol !== 'http:')
    throw new MediaUrlError(`unsupported URL scheme ${url.protocol} in ${value}`);
  const page = new URL(base);
  // archive.org is always reached over https (it redirects http anyway).
  if (ARCHIVE_ORG_HOSTS.has(url.hostname)) {
    url.protocol = 'https:';
    return planArchiveOrg(url, fetchJson);
  }
  if (url.protocol === 'http:' && page.protocol === 'https:')
    throw new MediaUrlError(
      `${url.href} is http:, and this page is https: — the browser blocks that download (mixed content); use an https: URL`,
    );
  return planGeneric(url);
}

// Find `member` among a container's entry names: the exact path, then the
// same path ignoring case, then a unique base-name match.  Returns the
// entry name or null.
export function findMember(names: string[], member: string): string | null {
  const want = member.replace(/^\/+/, '');
  if (names.includes(want)) return want;
  const lower = want.toLowerCase();
  const ci = names.filter((n) => n.toLowerCase() === lower);
  if (ci.length === 1) return ci[0];
  const base = lower.split('/').pop() ?? lower;
  const byBase = names.filter((n) => (n.split('/').pop() ?? '').toLowerCase() === base);
  return byBase.length === 1 ? byBase[0] : null;
}

// Interleave two byte-wide ROM chips into one 16-bit image: `even` supplies
// bytes 0, 2, 4, … and `odd` bytes 1, 3, 5, … (equal lengths).
export function interleaveHalves(even: Uint8Array, odd: Uint8Array): Uint8Array {
  const out = new Uint8Array(even.length * 2);
  for (let i = 0; i < even.length; i++) {
    out[2 * i] = even[i];
    out[2 * i + 1] = odd[i];
  }
  return out;
}
