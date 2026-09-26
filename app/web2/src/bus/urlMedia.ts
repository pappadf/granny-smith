// URL-parameter media provisioning. Port of app/web/js/url-media.js.
//
// Usage: visit `?rom=/path/to/Plus.rom&fd0=/path/to/system.dsk&model=plus`
// and the page boots into a running machine without going through Welcome.
//
// Each parameter value is fetched, one at a time (relative paths resolve
// against the page origin), streamed to /opfs/upload/url_<slot>, optionally
// archive-extracted via the C-side `archive.extract`, then persisted into
// /opfs/images/<category>/ the way an upload of that kind is (upload.ts
// persistAs) and mounted from there; the staging copy is then removed.  The
// frontend owns where media lives; the core no longer copies volatile paths
// into OPFS behind the caller's back (09-storage D-1).  A file that does not
// validate as its slot's category is attached from its staging copy, with a
// warning.

import { gsEval, gsErrorText, getModule, isModuleReady } from './emulator';
import { reconcileUiWithMachine, prepareFreshMachine } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import type { SchedulerMode } from '@/state/machine.svelte';
import { setMounted } from '@/state/images.svelte';
import { sanitizeName, isZipMagic, unzipFirstFile, isMacArchive } from '@/lib/archive';
import { identifyRom, type MediaTypeId } from '@/lib/media';
import { persistAs, streamToOpfs, discardStaging } from './upload';
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

// Parse a URLSearchParams (or compatible) into structured params.
export function parseUrlMediaParams(params: URLSearchParams): UrlMediaParams {
  const out: UrlMediaParams = {
    rom: params.get('rom'),
    vrom: params.get('vrom'),
    model: params.get('model'),
    speed: params.get('speed'),
    floppies: [],
    hardDisks: [],
    cd: params.get('cd'),
  };
  for (const [k, v] of params.entries()) {
    if (/^fd\d+$/.test(k)) out.floppies.push({ slot: k, url: v });
    if (/^hd\d+$/.test(k)) out.hardDisks.push({ slot: k, url: v });
  }
  return out;
}

// ?speed= as the toolbar's pacing mode, or null when absent or unknown.
// Accepts the core's names and their legacy aliases (max, realtime,
// hardware).  It used to be documented as reaching the wasm module's
// --speed flag, which nothing ever passed (F-22).
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
  // was in flight at once (R3).
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
  // fallback here, which two models cannot boot, F-01).
  const booted = await gsEval('machine.boot', { model: chosen, rom: romPath });
  if (booted !== true) {
    // A rejected document leaves the previous machine (or none) in place:
    // do not attach media to it or report a boot (N-08).
    showNotification(
      `Could not boot ${chosen} from URL parameters: ${gsErrorText(booted)}`,
      'error',
    );
    return false;
  }

  await insertUrlFloppies(params, paths);
  // ?hdN= is the N-th hard-disk bay in the model's own order (hd0 is the boot
  // bay), on whatever bus it is — not SCSI id N on the first bus (F-10).
  for (const hd of params.hardDisks) {
    const p = paths.get(hd.slot);
    if (!p) continue;
    const n = parseInt(hd.slot.replace('hd', ''), 10);
    report(hd.slot, p, await attachHardDisk(p, n));
  }
  const cdPath = paths.get('cd');
  if (cdPath) report('cd', cdPath, await attachCdrom(cdPath));

  await reconcileUiWithMachine('boot');
  await prepareFreshMachine();
  showNotification(`Booted ${chosen} from URL parameters`, 'info');
  return true;
}

// ?fdN= goes into drive N — it always went into drive 0, so ?fd0=a&fd1=b
// left b refused and dropped (F-09) — and only a drive the model has.
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

// Whether the file at `path` starts with the ZIP signature.
function stagedIsZip(path: string): boolean {
  const mod = getModule();
  if (!mod) return false;
  let stream: unknown;
  try {
    stream = mod.FS.open(path, 'r');
    const head = new Uint8Array(4);
    mod.FS.read(stream, head, 0, 4, 0);
    return isZipMagic(head);
  } catch {
    return false;
  } finally {
    if (stream !== undefined) mod.FS.close(stream);
  }
}

// Fetch a URL and stage its bytes at /opfs/upload/url_<slot>, streamed
// through the one chunked writer (upload.ts streamToOpfs) — it was read whole
// into memory and written to the memory-backed /tmp.  A ZIP is the one
// exception: unzipping needs the whole archive in memory, so a response that
// turns out to be one is read back, unzipped, and its first file staged in
// its place.  For Mac-archive extensions (sit/hqx/cpt/bin/sea) the C side
// does the extraction.  Returns the staged path and the name to store it
// under, or null.
async function fetchAndStage(
  slot: string,
  url: string,
): Promise<{ path: string; name: string } | null> {
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    const fileName = (url.split('/').pop() ?? '').split('?')[0] || slot;
    const staged = `${UPLOAD_DIR}/url_${slot}`;
    const body = res.body ?? (await res.blob());
    if (!(await streamToOpfs(staged, body))) return null;

    const ct = res.headers.get('Content-Type') ?? '';
    const looksLikeZip = /\.zip($|[?#])/i.test(url) || /zip/i.test(ct) || stagedIsZip(staged);
    if (looksLikeZip) {
      const mod = getModule();
      if (!mod) return null;
      const first = await unzipFirstFile(mod.FS.readFile(staged));
      if (!first) {
        showNotification(`${slot}: zip is empty`, 'error');
        await discardStaging(staged);
        return null;
      }
      if (!(await streamToOpfs(staged, first.data))) return null;
    }

    if (isMacArchive(fileName)) {
      const fmt = await gsEval('archive.identify', [staged]);
      if (typeof fmt === 'string' && fmt.length > 0) {
        const extractDir = `${UPLOAD_DIR}/url_${slot}_unpacked`;
        const extracted = (await gsEval('archive.extract', [staged, extractDir])) === true;
        if (extracted) await gsEval('storage.find_media', [extractDir, staged]);
        await discardStaging(extractDir);
      }
    }

    showNotification(`${slot} downloaded${looksLikeZip ? ' (zip)' : ''}`, 'info');
    return { path: staged, name: sanitizeName(fileName) || slot };
  } catch (e) {
    console.error(`[urlMedia] fetch ${slot} failed`, e);
    showNotification(`${slot} download failed`, 'error');
    return null;
  }
}
