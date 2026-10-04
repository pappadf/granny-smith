// URL-media addressing: how a `?rom=` / `?hd0=` / ... value becomes the
// request to make and, when the value continues through a container file,
// the member to take out of it.  Pure, so it is unit-tested directly
// (tests/unit/mediaUrl.test.ts).  bus/urlMedia.ts does the fetching.
//
// The URL is fetched as given, whatever the host: no host gets routing of
// its own.  A link that needs a particular endpoint (a CORS-enabled one, a
// server-side extraction) names that endpoint itself.
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

// Extensions that end a container segment when a member path follows.
const CONTAINER_EXT = /\.(zip|sit|sea|cpt|hqx|bin)$/i;
const ZIP_EXT = /\.zip$/i;

// A value that cannot be turned into a request (the message is user-facing).
export class MediaUrlError extends Error {}

// How to fetch one URL-media value.
export interface MediaFetchPlan {
  // The URL to GET.
  fetchUrl: string;
  // Member to take out of the fetched container, or null when the response
  // is the file itself.
  member: string | null;
  // The container's kind when `member` is set.
  container: 'zip' | 'mac' | null;
  // The file's own name (the member's base name, or the URL's last segment),
  // decoded: what error messages call it, and the import's hint for a
  // streamed UDIF (.dmg).  What the file is stored and shown as is the
  // caller's (bus/urlMedia.ts names URL media by slot and time).
  fileName: string;
  // The container's file name when the value named a member, for messages.
  containerName: string | null;
}

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

// The plan for a URL: the URL itself, or its container and the member path.
function planUrl(url: URL): MediaFetchPlan {
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

// Turn a URL-media value into its fetch plan.  `base` is the page's URL
// (relative values resolve against it).  Throws MediaUrlError when the value
// cannot work: not a URL, or http on an https page (the browser blocks mixed
// content).
export function planMediaFetch(value: string, base: string): MediaFetchPlan {
  let url: URL;
  try {
    url = new URL(value.trim(), base);
  } catch {
    throw new MediaUrlError(`not a valid URL: ${value}`);
  }
  if (url.protocol !== 'https:' && url.protocol !== 'http:')
    throw new MediaUrlError(`unsupported URL scheme ${url.protocol} in ${value}`);
  const page = new URL(base);
  if (url.protocol === 'http:' && page.protocol === 'https:')
    throw new MediaUrlError(
      `${url.href} is http:, and this page is https: — the browser blocks that download (mixed content); use an https: URL`,
    );
  return planUrl(url);
}

// The name a file fetched for URL slot `slot` is stored and shown under:
// the slot and the local date and time ("hd0_2026-10-04_17-42-05").  A URL
// says nothing reliable about what it serves (…/view_archive.php?file=…), so
// none of it goes into the name.  A ROM is stored under its own content id
// whatever it is called (lib/media.ts).
export function urlMediaName(slot: string, at: Date = new Date()): string {
  const p = (n: number) => String(n).padStart(2, '0');
  const date = `${at.getFullYear()}-${p(at.getMonth() + 1)}-${p(at.getDate())}`;
  const time = `${p(at.getHours())}-${p(at.getMinutes())}-${p(at.getSeconds())}`;
  return `${slot}_${date}_${time}`;
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
