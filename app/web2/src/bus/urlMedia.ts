// URL-parameter media provisioning. Port of app/web/js/url-media.js.
//
// Usage: visit `?rom=/path/to/Plus.rom&fd0=/path/to/system.dsk&model=Macintosh+Plus`
// and the page boots into a running machine without going through Welcome.
//
// Each parameter value is fetched (relative paths resolve against the page
// origin), staged to /tmp/url_<slot>, optionally archive-extracted via the
// C-side `archive.extract`, then mounted into the machine.

import { gsEval, getModule, isModuleReady } from './emulator';
import { showNotification } from '@/state/toasts.svelte';
import { machine } from '@/state/machine.svelte';
import { sanitizeName, isZipMagic, unzipFirstFile, isMacArchive } from '@/lib/archive';

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

  const downloads: Array<Promise<void>> = [];
  if (params.rom) downloads.push(fetchAndStage('rom', params.rom));
  if (params.vrom) downloads.push(fetchAndStage('vrom', params.vrom));
  for (const fd of params.floppies) downloads.push(fetchAndStage(fd.slot, fd.url));
  for (const hd of params.hardDisks) downloads.push(fetchAndStage(hd.slot, hd.url));
  if (params.cd) downloads.push(fetchAndStage('cd', params.cd));
  await Promise.all(downloads);

  if (!params.rom) {
    // Without a ROM there's no machine to boot; insert floppies into the
    // existing machine if one is running (matches url-media.js:230-237).
    for (const fd of params.floppies) {
      await gsEval('floppy.drives[0].insert', [`/tmp/url_${fd.slot}`, true]);
    }
    return false;
  }

  // ROM-led boot. rom.identify tells us which models the image lights up;
  // prefer the URL's `model=` if it's in the compatible list, else pick the
  // first compatible model.
  const tmpRomPath = '/tmp/url_rom';
  const info = await romIdentify(tmpRomPath);
  if (!info || !info.compatible?.length) {
    showNotification('Unrecognised ROM in URL params', 'error');
    return false;
  }
  const chosen =
    params.model && info.compatible.includes(params.model) ? params.model : info.compatible[0];
  const profile = await parseProfile(chosen);
  const ramKb = profile?.ram_default ?? 4096;

  await gsEval('machine.boot', [chosen, ramKb]);
  await gsEval('rom.load', [tmpRomPath]);

  for (const fd of params.floppies) {
    await gsEval('floppy.drives[0].insert', [`/tmp/url_${fd.slot}`, true]);
  }
  for (const hd of params.hardDisks) {
    const id = parseInt(hd.slot.replace('hd', ''), 10);
    await gsEval('scsi.attach_hd', [`/tmp/url_${hd.slot}`, id]);
  }
  if (params.cd) {
    await gsEval('scsi.attach_cdrom', ['/tmp/url_cd', 3]);
  }

  machine.model = chosen;
  machine.ram = `${ramKb / 1024} MB`;
  machine.mmuEnabled = /SE\/30|II/i.test(chosen);

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
  const r = await gsEval('rom.identify', [path]);
  if (typeof r !== 'string') return null;
  try {
    const parsed = JSON.parse(r) as Partial<RomIdentifyResult>;
    if (parsed && parsed.recognised) return parsed as RomIdentifyResult;
  } catch {
    return null;
  }
  return null;
}

async function parseProfile(model: string): Promise<{ ram_default?: number } | null> {
  const r = await gsEval('machine.profile', [model]);
  if (typeof r !== 'string') return null;
  try {
    return JSON.parse(r) as { ram_default?: number };
  } catch {
    return null;
  }
}

// Fetch a URL and stage its bytes into /tmp/url_<slot>. Handles ZIP wrapping
// transparently (extract the first file inside). For Mac-archive extensions
// (sit/hqx/cpt/bin/sea) the C side does the extraction once the file is in
// /tmp.
async function fetchAndStage(slot: string, url: string): Promise<void> {
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
        return;
      }
      bytes = first.data;
    }

    const tmpPath = `/tmp/url_${slot}`;
    const mod = getModule();
    if (!mod) return;
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
    void sanitizeName(fileName); // placeholder hook for future renaming
  } catch (e) {
    console.error(`[urlMedia] fetch ${slot} failed`, e);
    showNotification(`${slot} download failed`, 'error');
  }
}
