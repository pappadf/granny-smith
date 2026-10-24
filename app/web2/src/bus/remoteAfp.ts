// The page's file servers on the emulated AppleTalk network: "Internet
// Archive" and "Mac Hut", each its own Chooser entry beside the Shared
// Folders server, their volumes remote trees the core caches in OPFS
// (src/core/network/afp_remote.h).  The core asks for a folder's listing or
// a file's data when the guest needs it, with an `afp_remote_request` event;
// a provider here fetches it, writes it to the request's `dest`, and answers
// with appletalk.remote.complete.  What a URI names, and how each answer is
// built, is lib/remoteAfp.ts.

import { gsEval, gsErrorText, gsOk, setRemoteRequestHandler } from './emulator';
import { streamToOpfs } from './upload';
import { AFP_REMOTE_DIR } from '@/lib/opfsPaths';
import {
  collectionListing,
  iaFetchUrl,
  iaFilesUrl,
  iaSearchUrl,
  iaUri,
  iaZipViewUrl,
  itemListing,
  parseAutoindex,
  parseIaUri,
  parseZipView,
  toListing,
  webFetchUrl,
  zipListing,
  type IaDoc,
  type IaFile,
  type RemoteEntry,
  type ZipMember,
} from '@/lib/remoteAfp';

interface VolumeSpec {
  name: string; // as the Chooser lists it (max 27 characters)
  uri: string;
}

interface ServerSpec {
  name: string;
  volumes: VolumeSpec[];
}

// archive.org's Macintosh collections that hold software one can run, each a
// volume.
export const INTERNET_ARCHIVE: ServerSpec = {
  name: 'Internet Archive',
  volumes: [
    {
      name: 'Software Library',
      uri: iaUri({ kind: 'collection', collection: 'softwarelibrary_mac' }),
    },
    {
      name: 'Shareware Games',
      uri: iaUri({ kind: 'collection', collection: 'MacintoshSharewareGames' }),
    },
    { name: 'HyperCard Stacks', uri: iaUri({ kind: 'collection', collection: 'hypercardstacks' }) },
    {
      name: 'Macintosh Garden',
      uri: iaUri({ kind: 'collection', collection: 'Macintosh_Garden_Collection' }),
    },
  ],
};

const MAC_HUT_URL = 'https://machut.net/files/';

export const MAC_HUT: ServerSpec = {
  name: 'Mac Hut',
  volumes: [{ name: 'Mac Hut Files', uri: `web:${MAC_HUT_URL}` }],
};

// The user's settings, in localStorage under SETTINGS_KEY (JSON):
//   { "disabled": true }              no remote servers
//   { "proxy": "https://p/?u={url}" } Mac Hut through a CORS proxy (machut.net
//                                     sends no CORS headers)
export const SETTINGS_KEY = 'gs-remote-afp';

interface Settings {
  disabled?: boolean;
  proxy?: string;
}

function readSettings(): Settings {
  try {
    const raw = localStorage.getItem(SETTINGS_KEY);
    const s = raw ? (JSON.parse(raw) as unknown) : null;
    return s && typeof s === 'object' ? (s as Settings) : {};
  } catch {
    return {};
  }
}

let settings: Settings = {};
let unpackLimit = 64 << 20;
const inFlight = new Set<number>();

// A folder name for a cache directory.
function slug(s: string): string {
  return s.replace(/[^A-Za-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'volume';
}

async function addServer(spec: ServerSpec): Promise<void> {
  const servers = await gsEval('appletalk.remote.servers');
  if (!(Array.isArray(servers) && servers.includes(spec.name))) {
    const r = await gsEval('appletalk.remote.add_server', [spec.name]);
    if (!gsOk(r)) {
      console.warn(`remote AFP: cannot add ${spec.name}: ${gsErrorText(r)}`);
      return;
    }
  }
  for (const v of spec.volumes) {
    const cache = `${AFP_REMOTE_DIR}/${slug(spec.name)}/${slug(v.name)}`;
    const r = await gsEval('appletalk.remote.add_volume', [spec.name, v.name, v.uri, cache]);
    if (!gsOk(r))
      console.warn(`remote AFP: cannot add ${spec.name} / ${v.name}: ${gsErrorText(r)}`);
  }
}

// Whether the page can read machut.net (directly, or through the proxy).
async function canReachMacHut(): Promise<boolean> {
  const url = webFetchUrl(`web:${MAC_HUT_URL}`, settings.proxy);
  if (!url) return false;
  try {
    const res = await fetch(url, { method: 'GET', signal: AbortSignal.timeout(8000) });
    return res.ok;
  } catch {
    return false; // no CORS headers, or offline
  }
}

// Put the servers on the network, and answer whatever the core already asked
// (a request made before this page was listening).  Once, after the module is
// ready.
export async function installRemoteAfp(): Promise<void> {
  settings = readSettings();
  if (settings.disabled) return;
  setRemoteRequestHandler(onRemoteRequest);
  const limit = await gsEval('appletalk.remote.unpack_limit');
  if (typeof limit === 'number' && limit >= 0) unpackLimit = limit;
  await addServer(INTERNET_ARCHIVE);
  // Mac Hut is published only when its files can be fetched: a server whose
  // every folder fails would only confuse.
  if (await canReachMacHut()) await addServer(MAC_HUT);
  const pending = await gsEval('appletalk.remote.requests');
  if (Array.isArray(pending))
    for (const r of pending)
      if (r && typeof r === 'object') onRemoteRequest(r as Record<string, unknown>);
}

// The core's `afp_remote_request` event (bus/emulator.ts routes it here).
export function onRemoteRequest(d: Record<string, unknown>): void {
  const id = typeof d.id === 'number' ? d.id : Number(d.id);
  const op = d.op;
  const uri = d.uri;
  const dest = d.dest;
  if (!Number.isInteger(id) || typeof uri !== 'string' || typeof dest !== 'string') return;
  if (op !== 'list' && op !== 'fetch') return;
  if (inFlight.has(id)) return; // the event and the startup sweep both saw it
  inFlight.add(id);
  void serve(id, op, uri, dest).finally(() => inFlight.delete(id));
}

async function serve(id: number, op: 'list' | 'fetch', uri: string, dest: string): Promise<void> {
  let status = 1;
  let message = '';
  try {
    const ok =
      op === 'list'
        ? await streamToOpfs(dest, new TextEncoder().encode(toListing(await list(uri))))
        : await fetchTo(uri, dest);
    if (ok) status = 0;
    else message = 'could not write the result';
  } catch (err) {
    message = err instanceof Error ? err.message : String(err);
    console.warn(`remote AFP: ${op} ${uri} failed: ${message}`);
  }
  await gsEval('appletalk.remote.complete', [id, status, message]);
}

async function getOk(url: string): Promise<Response> {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${res.status} ${res.statusText} from ${new URL(url).host}`);
  return res;
}

// What has been read this session, for the folders below it: a collection's
// items, an item's files, a zip's members.
const docsCache = new Map<string, Promise<IaDoc[]>>();
const filesCache = new Map<string, Promise<IaFile[]>>();
const zipCache = new Map<string, Promise<ZipMember[]>>();

function once<T>(cache: Map<string, Promise<T>>, key: string, load: () => Promise<T>): Promise<T> {
  let p = cache.get(key);
  if (!p) {
    p = load();
    cache.set(key, p);
    p.catch(() => cache.delete(key)); // a failure is tried again next time
  }
  return p;
}

// The largest collection read whole (the search's row limit).
const MAX_ITEMS = 10000;

async function list(uri: string): Promise<RemoteEntry[]> {
  if (uri.startsWith('web:')) {
    const url = webFetchUrl(uri, settings.proxy);
    if (!url) throw new Error(`not a web URI: ${uri}`);
    return parseAutoindex(await (await getOk(url)).text(), uri.slice(4));
  }
  const u = parseIaUri(uri);
  if (!u) throw new Error(`not a URI this page serves: ${uri}`);
  switch (u.kind) {
    case 'collection': {
      const docs = await once(docsCache, u.collection, async () => {
        const json = (await (await getOk(iaSearchUrl(u.collection, MAX_ITEMS))).json()) as {
          response?: { docs?: IaDoc[] };
        };
        return json.response?.docs ?? [];
      });
      return collectionListing(u.collection, docs, u.bucket);
    }
    case 'item': {
      const files = await once(filesCache, u.item, async () => {
        const json = (await (await getOk(iaFilesUrl(u.item))).json()) as { result?: IaFile[] };
        return json.result ?? [];
      });
      return itemListing(u.item, files, u.folder, unpackLimit);
    }
    case 'zip': {
      const key = `${u.item}/${u.zip}`;
      const members = await once(zipCache, key, async () =>
        parseZipView(await (await getOk(iaZipViewUrl(u.item, u.zip))).text()),
      );
      return zipListing(u.item, u.zip, members, u.folder);
    }
    default:
      throw new Error(`not a folder: ${uri}`);
  }
}

async function fetchTo(uri: string, dest: string): Promise<boolean> {
  const url = uri.startsWith('web:') ? webFetchUrl(uri, settings.proxy) : iaFetchUrlOf(uri);
  if (!url) throw new Error(`not a file URI: ${uri}`);
  const res = await getOk(url);
  return streamToOpfs(dest, res.body ?? new Uint8Array(await res.arrayBuffer()));
}

function iaFetchUrlOf(uri: string): string | null {
  const u = parseIaUri(uri);
  return u ? iaFetchUrl(u) : null;
}
