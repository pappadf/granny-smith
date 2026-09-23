// URL-parameter media provisioning. Port of app/web/js/url-media.js.
//
// Usage: visit `?rom=/path/to/Plus.rom&fd0=/path/to/system.dsk&model=Macintosh+Plus`
// and the page boots into a running machine without going through Welcome.
//
// Each parameter value is fetched (relative paths resolve against the page
// origin), staged to /tmp/url_<slot>, optionally archive-extracted via the
// C-side `archive.extract`, then persisted into /opfs/images/<category>/
// the way an upload of that kind is (upload.ts persistAs) and mounted from
// there.  The frontend owns where media lives; the core no longer copies
// volatile paths into OPFS behind the caller's back (09-storage D-1).  A
// file that does not validate as its slot's category is attached from /tmp
// as before, with a warning that it will not survive a reload.

import { gsEval, getModule, isModuleReady, applyCapabilities } from './emulator';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { sanitizeName, isZipMagic, unzipFirstFile, isMacArchive } from '@/lib/archive';
import type { MediaTypeId } from '@/lib/media';
import { persistAs } from './upload';

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

  // Slot -> the path to attach it from.  Fetches run in parallel; each
  // resolves to its persisted path (or its /tmp staging path, or undefined
  // when the fetch failed).
  const paths = new Map<string, string | undefined>();
  const provision = async (slot: string, url: string, category: MediaTypeId) => {
    paths.set(slot, await fetchAndPersist(slot, url, category));
  };
  const downloads: Array<Promise<void>> = [];
  if (params.rom) downloads.push(provision('rom', params.rom, 'rom'));
  if (params.vrom) downloads.push(provision('vrom', params.vrom, 'vrom'));
  for (const fd of params.floppies) downloads.push(provision(fd.slot, fd.url, 'fd'));
  for (const hd of params.hardDisks) downloads.push(provision(hd.slot, hd.url, 'hd'));
  if (params.cd) downloads.push(provision('cd', params.cd, 'cdrom'));
  await Promise.all(downloads);

  if (!params.rom) {
    // Without a ROM there's no machine to boot; insert floppies into the
    // existing machine if one is running (matches url-media.js:230-237).
    for (const fd of params.floppies) {
      const p = paths.get(fd.slot);
      if (p) await gsEval('machine.floppy.drive[0].insert', [p, true]);
    }
    return false;
  }

  // ROM-led boot. rom.identify tells us which models the image lights up;
  // prefer the URL's `model=` if it's in the compatible list, else pick the
  // first compatible model.
  const romPath = paths.get('rom');
  const info = romPath ? await romIdentify(romPath) : null;
  if (!info || !info.compatible?.length) {
    showNotification('Unrecognised ROM in URL params', 'error');
    return false;
  }
  const chosen =
    params.model && info.compatible.includes(params.model) ? params.model : info.compatible[0];
  const profile = await parseProfile(chosen);
  const ramKb = profile?.ram_default ?? 4096;

  // One boot document: the core validates model/ram/rom together and
  // installs the ROM itself (proposal-named-args-boot-config §4).
  await gsEval('machine.boot', { model: chosen, ram: ramKb, rom: romPath });

  for (const fd of params.floppies) {
    const p = paths.get(fd.slot);
    if (p) await gsEval('machine.floppy.drive[0].insert', [p, true]);
  }
  for (const hd of params.hardDisks) {
    const p = paths.get(hd.slot);
    if (!p) continue;
    if (profile?.hd_bus === 'profile') {
      // Lisa/XL: parallel-port ProFile, attached off the SCSI bus.
      await gsEval('machine.hd.attach', [p, true]);
    } else {
      const id = parseInt(hd.slot.replace('hd', ''), 10);
      await gsEval('machine.scsi.attach_hd', [p, id]);
    }
  }
  const cdPath = paths.get('cd');
  if (cdPath) {
    await gsEval('machine.scsi.attach_cdrom', [cdPath, 3]);
  }

  machine.model = chosen;
  machine.ram = `${ramKb / 1024} MB`;
  await applyCapabilities(chosen);

  await gsEval('scheduler.run');
  showNotification(`Booted ${chosen} from URL parameters`, 'info');
  return true;
}

interface RomIdentifyResult {
  recognised: boolean;
  compatible: string[];
  checksum: string;
  name: string;
  size: number;
}

async function romIdentify(path: string): Promise<RomIdentifyResult | null> {
  // rom.identify returns a native object (V_MAP) — no inner JSON.parse.
  const r = await gsEval('machine.rom.identify', [path]);
  if (!r || typeof r !== 'object') return null;
  const parsed = r as Partial<RomIdentifyResult>;
  if (parsed.recognised) return parsed as RomIdentifyResult;
  return null;
}

async function parseProfile(
  model: string,
): Promise<{ ram_default?: number; hd_bus?: string } | null> {
  const r = await gsEval('machine.profile', [model]);
  if (!r || typeof r !== 'object' || 'error' in r) return null;
  return r as { ram_default?: number; hd_bus?: string };
}

// Fetch a URL, stage it, and persist it as `category`.  Returns the path to
// attach from: the persisted /opfs/images/<category>/ path, or the /tmp
// staging path when the file does not validate as that category, or
// undefined when the fetch failed.
async function fetchAndPersist(
  slot: string,
  url: string,
  category: MediaTypeId,
): Promise<string | undefined> {
  const staged = await fetchAndStage(slot, url);
  if (!staged) return undefined;
  const persisted = await persistAs(staged.path, staged.name, category);
  if (persisted) return persisted;
  showNotification(`${slot}: not recognised as ${category}; using it unsaved`, 'warning');
  return staged.path;
}

// Fetch a URL and stage its bytes into /tmp/url_<slot>. Handles ZIP wrapping
// transparently (extract the first file inside). For Mac-archive extensions
// (sit/hqx/cpt/bin/sea) the C side does the extraction once the file is in
// /tmp.  Returns the staged path and the name to store it under, or null.
async function fetchAndStage(
  slot: string,
  url: string,
): Promise<{ path: string; name: string } | null> {
  try {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    let bytes: Uint8Array<ArrayBufferLike> = new Uint8Array(await res.arrayBuffer());
    const fileName = (url.split('/').pop() ?? '').split('?')[0] || slot;
    const ct = res.headers.get('Content-Type') ?? '';
    const looksLikeZip = isZipMagic(bytes) || /\.zip($|[?#])/i.test(url) || /zip/i.test(ct);
    if (looksLikeZip) {
      const first = await unzipFirstFile(bytes);
      if (!first) {
        showNotification(`${slot}: zip is empty`, 'error');
        return null;
      }
      bytes = first.data;
    }

    const tmpPath = `/tmp/url_${slot}`;
    const mod = getModule();
    if (!mod) return null;
    try {
      mod.FS.unlink(tmpPath);
    } catch {
      /* not present yet */
    }
    mod.FS.writeFile(tmpPath, bytes);

    if (isMacArchive(fileName)) {
      const fmt = await gsEval('archive.identify', [tmpPath]);
      if (typeof fmt === 'string' && fmt.length > 0) {
        const extractDir = `/tmp/url_${slot}_unpacked`;
        const extracted = (await gsEval('archive.extract', [tmpPath, extractDir])) === true;
        if (extracted) {
          await gsEval('storage.find_media', [extractDir, tmpPath]);
        }
      }
    }

    showNotification(`${slot} downloaded${looksLikeZip ? ' (zip)' : ''}`, 'info');
    return { path: tmpPath, name: sanitizeName(fileName) || slot };
  } catch (e) {
    console.error(`[urlMedia] fetch ${slot} failed`, e);
    showNotification(`${slot} download failed`, 'error');
    return null;
  }
}
