// URL-parameter media provisioning. Port of app/web/js/url-media.js.
//
// Usage: visit `?rom=/path/to/Plus.rom&fd0=/path/to/system.dsk&model=plus`
// and the page boots into a running machine without going through Welcome.
// Parameter names are case-insensitive (`ROM=`, `HD0=`); `hd` / `fd` mean the
// first bay / drive.  A value may continue through a container
// (`…/roms.zip/Mac%20IIci.ROM`) and archive.org URLs are routed to endpoints
// a page may read — see lib/mediaUrl.ts for the addressing rules.
//
// Each parameter value is fetched, one at a time (relative paths resolve
// against the page origin), streamed to /opfs/upload/url_<slot>, the named
// member (or, for a bare archive, the first file / the found medium) taken
// out of a container, then persisted into
// /opfs/images/<category>/ the way an upload of that kind is (upload.ts
// persistAs) and mounted from there; the staging copy is then removed.  The
// frontend owns where media lives; the core no longer copies volatile paths
// into OPFS behind the caller's back.  A file that does not
// validate as its slot's category is attached from its staging copy, with a
// warning.

import { gsEval, gsErrorText, isModuleReady } from './emulator';
import { xferReadAll } from './xfer';
import { reconcileUiWithMachine, prepareFreshMachine, setBootDevice } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import type { SchedulerMode } from '@/state/machine.svelte';
import { setMounted } from '@/state/images.svelte';
import { sanitizeName, unzipAll, isMacArchive } from '@/lib/archive';
import {
  canonicalParamName,
  planMediaFetch,
  findMember,
  MediaUrlError,
  type MediaFetchPlan,
} from '@/lib/mediaUrl';
import { identifyRom, type MediaTypeId } from '@/lib/media';
import { persistAs, streamToOpfs, discardStaging, stagedIsZip } from './upload';
import { UPLOAD_DIR } from '@/lib/opfsPaths';
import { getProfile } from './profile';
import { attachHardDisk, attachCdrom, insertFloppy, type MediaResult } from './media';

export interface UrlMediaParams {
  rom: string | null;
  vrom: string | null;
  model: string | null;
  speed: string | null;
  floppies: Array<{ slot: string; url: string }>;
  hardDisks: Array<{ slot: string; url: string }>;
  cd: string | null;
}

// Parse a URLSearchParams (or compatible) into structured params.  Names
// match case-insensitively (`ROM`, `Rom`, `rom` are one parameter; `HD` is
// `hd0`); the first occurrence of a name wins, a later spelling of it is
// ignored with a console warning.
export function parseUrlMediaParams(params: URLSearchParams): UrlMediaParams {
  const seen = new Map<string, string>();
  for (const [k, v] of params.entries()) {
    const name = canonicalParamName(k);
    if (!name) continue;
    if (seen.has(name)) {
      console.warn(`[urlMedia] ignoring ${k}=: ${name} is already set`);
      continue;
    }
    seen.set(name, v);
  }
  const out: UrlMediaParams = {
    rom: seen.get('rom') ?? null,
    vrom: seen.get('vrom') ?? null,
    model: seen.get('model') ?? null,
    speed: seen.get('speed') ?? null,
    floppies: [],
    hardDisks: [],
    cd: seen.get('cd') ?? null,
  };
  for (const [name, v] of seen) {
    if (/^fd\d+$/.test(name)) out.floppies.push({ slot: name, url: v });
    if (/^hd\d+$/.test(name)) out.hardDisks.push({ slot: name, url: v });
  }
  return out;
}

// ?speed= as the toolbar's pacing mode, or null when absent or unknown.
// Accepts the core's names and their legacy aliases (max, realtime,
// hardware).  It used to be documented as reaching the wasm module's
// --speed flag, which nothing ever passed.
export function urlSchedulerMode(speed: string | null): SchedulerMode | null {
  switch ((speed ?? '').toLowerCase()) {
    case 'paced':
    case 'realtime':
    case 'real':
    case 'hardware':
    case 'hw':
      return 'live';
    case 'accelerated':
    case 'accel':
      return 'accel';
    case 'turbo':
    case 'max':
      return 'turbo';
    default:
      return null;
  }
}

export function hasUrlMedia(params: UrlMediaParams): boolean {
  return (
    !!(params.rom || params.vrom || params.cd) ||
    params.floppies.length > 0 ||
    params.hardDisks.length > 0
  );
}

// Top-level entry. Returns true if a machine was successfully booted.
export async function processUrlMedia(rawParams: URLSearchParams): Promise<boolean> {
  if (!isModuleReady()) {
    showNotification('Emulator still starting; URL media skipped', 'warning');
    return false;
  }
  const params = parseUrlMediaParams(rawParams);
  if (!hasUrlMedia(params)) return false;

  // Slot -> the path to attach it from: its persisted path, or its staging
  // path when it did not validate as its category, or undefined when the
  // fetch failed.  One download at a time: in parallel, every large image
  // was in flight at once.
  const paths = new Map<string, string | undefined>();
  const wanted: Array<[string, string, MediaTypeId]> = [];
  if (params.rom) wanted.push(['rom', params.rom, 'rom']);
  if (params.vrom) wanted.push(['vrom', params.vrom, 'vrom']);
  for (const fd of params.floppies) wanted.push([fd.slot, fd.url, 'fd']);
  for (const hd of params.hardDisks) wanted.push([hd.slot, hd.url, 'hd']);
  if (params.cd) wanted.push(['cd', params.cd, 'cdrom']);
  for (const [slot, url, category] of wanted) {
    paths.set(slot, await fetchAndPersist(slot, url, category));
  }

  if (!params.rom) {
    // Without a ROM there's no machine to boot; insert floppies into the
    // existing machine if one is running (matches url-media.js:230-237).
    await insertUrlFloppies(params, paths);
    return false;
  }

  // ROM-led boot. rom.identify tells us which models the image lights up;
  // prefer the URL's `model=` if it's in the compatible list, else pick the
  // first compatible model.
  const romPath = paths.get('rom');
  const info = romPath ? await identifyRom(gsEval, romPath) : null;
  if (!info || !info.compatible.length) {
    showNotification('Unrecognised ROM in URL params', 'error');
    return false;
  }
  const chosen =
    params.model && info.compatible.includes(params.model) ? params.model : info.compatible[0];

  // One boot document: the core validates model/rom together, installs the
  // ROM itself and boots the model's own default RAM (there was a 4096 KB
  // fallback here, which two models cannot boot).
  const booted = await gsEval('machine.boot', { model: chosen, rom: romPath });
  if (booted !== true) {
    // A rejected document leaves the previous machine (or none) in place:
    // do not attach media to it or report a boot.
    showNotification(
      `Could not boot ${chosen} from URL parameters: ${gsErrorText(booted)}`,
      'error',
    );
    return false;
  }

  await insertUrlFloppies(params, paths);
  // ?hdN= is the N-th hard-disk bay in the model's own order (hd0 is the boot
  // bay), on whatever bus it is — not SCSI id N on the first bus.
  for (const hd of params.hardDisks) {
    const p = paths.get(hd.slot);
    if (!p) continue;
    const n = parseInt(hd.slot.replace('hd', ''), 10);
    const r = await attachHardDisk(p, n);
    report(hd.slot, p, r);
    // The boot bay's disk is the startup device, as the Configuration
    // dialog's boot names it.
    if (r.ok && n === 0 && r.mount.bus === 'scsi') await setBootDevice(r.mount.drive);
  }
  const cdPath = paths.get('cd');
  if (cdPath) report('cd', cdPath, await attachCdrom(cdPath));

  await reconcileUiWithMachine('boot');
  await prepareFreshMachine();
  showNotification(`Booted ${chosen} from URL parameters`, 'info');
  return true;
}

// ?fdN= goes into drive N — it always went into drive 0, so ?fd0=a&fd1=b
// left b refused and dropped — and only a drive the model has.
async function insertUrlFloppies(
  params: UrlMediaParams,
  paths: Map<string, string | undefined>,
): Promise<void> {
  const model = await gsEval('machine.id');
  const profile = typeof model === 'string' && model ? await getProfile(model) : null;
  const drives = profile?.floppy_slots.length ?? 0;
  for (const fd of params.floppies) {
    const p = paths.get(fd.slot);
    if (!p) continue;
    const n = parseInt(fd.slot.replace('fd', ''), 10);
    if (n >= drives) {
      showNotification(`${fd.slot}: this machine has no floppy drive ${n + 1}`, 'error');
      continue;
    }
    report(fd.slot, p, await insertFloppy(p, true, n));
  }
}

// Record where a URL medium went, or say that it did not go in.
function report(slot: string, path: string, r: MediaResult): void {
  if (r.ok) setMounted(path, r.mount);
  else showNotification(`${slot}: not attached: ${r.reason}`, 'error');
}

// Fetch a URL, stage it, and persist it as `category`.  Returns the path to
// attach from: the persisted /opfs/images/<category>/ path (the staging copy
// is then removed), or the staging path when the file does not validate as
// that category, or undefined when the fetch failed.
async function fetchAndPersist(
  slot: string,
  url: string,
  category: MediaTypeId,
): Promise<string | undefined> {
  const staged = await fetchAndStage(slot, url);
  if (!staged) return undefined;
  const persisted = await persistAs(staged.path, staged.name, category);
  if (persisted) {
    await discardStaging(staged.path);
    return persisted;
  }
  showNotification(
    `${slot}: not recognised as ${category}; attaching the downloaded copy`,
    'warning',
  );
  return staged.path;
}

// GET `url` for JSON (the archive.org metadata API).
async function fetchJson(url: string): Promise<unknown> {
  const res = await fetch(url);
  if (!res.ok) throw new MediaUrlError(`${url}: ${res.status} ${res.statusText}`);
  return res.json();
}

// Human size for messages ("512 KB", "25.0 MB").
function sizeText(bytes: number): string {
  if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

// A failed fetch() of `url`: a TypeError is the browser refusing to hand the
// page the response (no network, or no CORS header on it).
function fetchFailureText(e: unknown, url: string): string {
  if (e instanceof MediaUrlError) return e.message;
  if (e instanceof TypeError)
    return `could not read ${new URL(url).host}: network error, or the server does not allow this page to download it (CORS)`;
  return e instanceof Error ? e.message : String(e);
}

// Fetch a URL-media value and stage its bytes at /opfs/upload/url_<slot>,
// streamed through the one chunked writer (upload.ts streamToOpfs).  When
// the value names a member of a container, the container is fetched whole
// and the member taken out of it (a zip in JS — unzipping needs the whole
// archive in memory — a Mac archive by the C side); archive.org extracts zip
// members itself.  A container fetched without a member path keeps the old
// behaviour: a zip's first file, a Mac archive's found medium.  Returns the
// staged path and the name to store it under, or null.
async function fetchAndStage(
  slot: string,
  url: string,
): Promise<{ path: string; name: string } | null> {
  const label = slot.toUpperCase();
  let plan: MediaFetchPlan;
  try {
    plan = await planMediaFetch(url, window.location.href, fetchJson);
  } catch (e) {
    showNotification(`${label}: ${fetchFailureText(e, url)}`, 'error');
    return null;
  }
  const staged = `${UPLOAD_DIR}/url_${slot}`;
  try {
    let res: Response;
    try {
      res = await fetch(plan.fetchUrl);
    } catch (e) {
      throw new MediaUrlError(fetchFailureText(e, plan.fetchUrl));
    }
    if (!res.ok) {
      const what = res.status === 404 ? 'not found' : `${res.status} ${res.statusText}`;
      throw new MediaUrlError(`${plan.containerName ?? plan.fileName}: ${what}`);
    }
    const body = res.body ?? (await res.blob());
    if (!(await streamToOpfs(staged, body))) return null;

    const ct = res.headers.get('Content-Type') ?? '';
    let name = plan.fileName;
    if (plan.member !== null) {
      // The value named a member: take exactly that one out.
      if (!(await extractMember(slot, staged, plan))) return null;
    } else if (/\.zip$/i.test(plan.fileName) || /zip/i.test(ct) || (await stagedIsZip(staged))) {
      // A bare zip: its first file, as before member paths existed.
      const first = (await unzipAll(await xferReadAll(staged)))[0];
      if (!first) throw new MediaUrlError(`${plan.fileName}: the zip is empty`);
      if (!(await streamToOpfs(staged, first.data))) return null;
      name = first.name.split('/').pop() || name;
    } else if (isMacArchive(plan.fileName)) {
      await unpackMacArchive(slot, staged, null);
    }

    const size = await gsEval('storage.path_size', [staged]);
    const from = plan.containerName ? ` from ${plan.containerName}` : '';
    const sz = typeof size === 'number' ? ` (${sizeText(size)})` : '';
    showNotification(`${label}: ${name}${from}${sz}`, 'info');
    return { path: staged, name: sanitizeName(name) || slot };
  } catch (e) {
    console.error(`[urlMedia] fetch ${slot} failed`, e);
    showNotification(`${label}: ${fetchFailureText(e, plan.fetchUrl)}`, 'error');
    await discardStaging(staged);
    return null;
  }
}

// Replace the container staged at `staged` with its member `plan.member`.
// Throws MediaUrlError (listing what is there) when the member is absent.
async function extractMember(slot: string, staged: string, plan: MediaFetchPlan): Promise<boolean> {
  const member = plan.member ?? '';
  if (plan.container === 'zip') {
    const entries = (await unzipAll(await xferReadAll(staged))).filter((f) => f.name);
    const hit = findMember(
      entries.map((f) => f.name),
      member,
    );
    if (!hit)
      throw memberNotFound(
        plan,
        entries.map((f) => f.name),
      );
    const data = entries.find((f) => f.name === hit)?.data;
    return !!data && (await streamToOpfs(staged, data));
  }
  return unpackMacArchive(slot, staged, member, plan);
}

// Unpack the Mac archive staged at `staged` (StuffIt, BinHex, Compact Pro,
// MacBinary) and put `member` — or, with none, the medium storage.find_media
// picks — in its place.
async function unpackMacArchive(
  slot: string,
  staged: string,
  member: string | null,
  plan?: MediaFetchPlan,
): Promise<boolean> {
  const fmt = await gsEval('archive.identify', [staged]);
  if (typeof fmt !== 'string' || fmt.length === 0) {
    if (member !== null && plan) throw new MediaUrlError(`${plan.containerName}: not an archive`);
    return true;
  }
  const extractDir = `${UPLOAD_DIR}/url_${slot}_unpacked`;
  try {
    if ((await gsEval('archive.extract', [staged, extractDir])) !== true)
      throw new MediaUrlError(`could not unpack ${plan?.containerName ?? 'the archive'}`);
    if (member === null) {
      await gsEval('storage.find_media', [extractDir, staged]);
      return true;
    }
    const names = await listTree(extractDir);
    const hit = findMember(names, member);
    if (!hit) throw memberNotFound(plan as MediaFetchPlan, names);
    return (await gsEval('storage.cp', [`${extractDir}/${hit}`, staged])) === true;
  } finally {
    await discardStaging(extractDir);
  }
}

// Every file under `dir`, as paths relative to it.
async function listTree(dir: string, prefix = ''): Promise<string[]> {
  const entries = await gsEval('storage.list_dir', [prefix ? `${dir}/${prefix}` : dir]);
  if (!Array.isArray(entries)) return [];
  const out: string[] = [];
  for (const e of entries) {
    if (typeof e !== 'string' || e === '.' || e === '..') continue;
    const rel = prefix ? `${prefix}/${e}` : e;
    const sub = await gsEval('storage.list_dir', [`${dir}/${rel}`]);
    if (Array.isArray(sub)) out.push(...(await listTree(dir, rel)));
    else out.push(rel);
  }
  return out;
}

// The error for a member path that is not in its container.
function memberNotFound(plan: MediaFetchPlan, names: string[]): MediaUrlError {
  const shown = names.slice(0, 5).join(', ');
  return new MediaUrlError(
    `"${plan.member}" is not in ${plan.containerName}` +
      (names.length ? ` (it has ${shown}${names.length > 5 ? ', ...' : ''})` : ''),
  );
}
