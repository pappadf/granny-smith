// URL-parameter media provisioning. Port of app/web/js/url-media.js.
//
// Usage: visit `?rom=/path/to/Plus.rom&fd0=/path/to/system.dsk&model=plus`
// and the page boots into a running machine without going through Welcome.
// Parameter names are case-insensitive (`ROM=`, `HD0=`); `hd` / `fd` mean the
// first bay / drive.  A value is fetched as given; it may continue through a
// container (`…/roms.zip/Mac%20IIci.ROM`) — see lib/mediaUrl.ts for the
// addressing rules.  What a file is stored and shown as comes from its slot
// and the time it was fetched (state/urlBoot urlMediaName), never from the
// URL; a ROM is stored under its own content id.
//
// Each parameter value is fetched, one at a time (relative paths resolve
// against the page origin), streamed to a scratch file of its own
// (/opfs/upload/.scratch/<nonce>-url_<slot>), the named member (or, for a
// bare archive, the first file / the found medium) taken out of a container,
// then persisted into /opfs/images/<category>/ the way an upload of that
// kind is (upload.ts persistAs) and mounted from there.  The frontend owns
// where media lives; the core no longer copies volatile paths into OPFS
// behind the caller's back.  A file that does not validate as its slot's
// category is rejected exactly as the same file dropped on its category is,
// with the validator's reason: a ROM that fails does not boot, a disk that
// fails is left out of the boot, and the run says so.  Nothing is attached
// from the scratch area, and every exit discards the scratch file.
//
// `hdN=blank:<model or size>` and `fdN=blank:<size>` create a new blank disk
// in that slot instead (lib/blankMedia.ts has the syntax): a floppy with the
// downloads, a hard disk once the ROM has chosen the model, since which bus
// the disk goes on (SCSI, ATA, the Lisa's ProFile) decides what it is.  It is
// named for the link, so a reload reuses it (`blank!:` replaces it), and a
// spec that cannot be created is reported like a failed download: the boot
// goes ahead without that disk.
//
// `config=` is the configuration document, base64url-encoded JSON (the
// dialog's document; absent, the model's default configuration).  `hdN`,
// `cd` and `fdN` keep meaning the Nth hard disk / the CD-ROM drive of the
// default configuration / floppy drive N; a floppy named for a position the
// default configuration leaves empty puts a drive there.  Every other
// parameter is a configuration edit by name -- an option of the model's tree
// (`addressing=32`, `memory=32768`) or the screen (`monitor=`, `mode=`,
// `display=`) -- applied over that document (lib/urlConfig.ts).
//
// The ROM and a `vrom=` go into the one machine.boot document: the URL's
// declaration ROM is the ROM of the slot whose card it provides, ahead of any
// other revision of it already stored.  (Storing it also offers it to the
// core's ROM catalog, for later boots; that is not how this boot gets it.)
//
// While a ROM-led boot runs (state/urlBoot: the page was opened to boot),
// every file is listed up front and its download progress reported for the
// progress view that stands in for Welcome (components/display/UrlBootView).

import { gsEval, gsErrorText, gsOk, isModuleReady } from './emulator';
import { xferReadAll } from './xfer';
import { reconcileUiWithMachine, prepareFreshMachine } from './boot';
import { showNotification } from '@/state/toasts.svelte';
import { startActivity, endActivity } from '@/state/activity.svelte';
import type { SchedulerMode } from '@/state/machine.svelte';
import {
  urlBoot,
  queueUrlFile,
  urlMediaNameFor,
  updateUrlFile,
  setUrlBootStage,
  skipQueuedUrlFiles,
} from '@/state/urlBoot.svelte';
import { setMounted } from '@/state/images.svelte';
import { unzipAll } from '@/lib/archive';
import {
  canonicalParamName,
  planMediaFetch,
  findMember,
  MediaUrlError,
  interleaveHalves,
  decodeConfigParam,
  type MediaFetchPlan,
} from '@/lib/mediaUrl';
import { identifyRom, MEDIA_TYPES, type MediaTypeId } from '@/lib/media';
import {
  addUrlConfigParam,
  applyUrlConfig,
  emptyUrlConfigParams,
  isUrlConfigParam,
  type UrlConfigParams,
} from '@/lib/urlConfig';
import { persistAs, streamToOpfs, discardStaging, stagedArchiveFormat } from './upload';
import { scratchPath, FD_DIR, HD_DIR } from '@/lib/opfsPaths';
import {
  parseBlankValue,
  blankUrlHash,
  blankDiskName,
  floppyBlankPlan,
  hardDiskBlankPlan,
  hardDiskSpecRefused,
  type BlankPlan,
  type BlankSpec,
} from '@/lib/blankMedia';
import { getProfile, type ConfigDocument, type MachineProfile, type StorageBus } from './profile';
import {
  attachHardDisk,
  attachCdrom,
  detectFdDriveCount,
  insertFloppy,
  type MediaResult,
} from './media';
import { importImage, type DiskCategory, type ImportSource } from './importImage';

export interface UrlMediaParams {
  rom: string | null;
  // A second ROM= value: the other chip of a ROM dumped as two byte-wide
  // halves (the Lisa's 341-0175/341-0176).  The page interleaves the pair.
  romPair: string | null;
  vrom: string | null;
  model: string | null;
  speed: string | null;
  // The configuration document from ?config= (decoded), or null.
  config: Record<string, unknown> | null;
  // ?config= was given but is not base64url JSON.
  configInvalid: boolean;
  floppies: Array<{ slot: string; url: string }>;
  hardDisks: Array<{ slot: string; url: string }>;
  cd: string | null;
  // The configuration edits by name (lib/urlConfig.ts): every parameter
  // that is not one of the above or another the page reads.
  settings: UrlConfigParams;
}

// Parse a URLSearchParams (or compatible) into structured params.  Names
// match case-insensitively (`ROM`, `Rom`, `rom` are one parameter; `HD` is
// `hd0`); the first occurrence of a name wins, a later spelling of it is
// ignored with a console warning.  The one exception is a second `rom=`:
// the other half of a two-chip ROM.  Any other name is a configuration edit
// (settings), unless the page reads it for something else.
export function parseUrlMediaParams(params: URLSearchParams): UrlMediaParams {
  const seen = new Map<string, string>();
  const settings = emptyUrlConfigParams();
  let romPair: string | null = null;
  for (const [k, v] of params.entries()) {
    const name = canonicalParamName(k);
    if (!name) {
      if (isUrlConfigParam(k.toLowerCase())) addUrlConfigParam(settings, k, v);
      continue;
    }
    if (name === 'rom' && seen.has('rom') && romPair === null) {
      romPair = v;
      continue;
    }
    if (seen.has(name)) {
      console.warn(`[urlMedia] ignoring ${k}=: ${name} is already set`);
      continue;
    }
    seen.set(name, v);
  }
  const out: UrlMediaParams = {
    rom: seen.get('rom') ?? null,
    romPair,
    vrom: seen.get('vrom') ?? null,
    model: seen.get('model') ?? null,
    speed: seen.get('speed') ?? null,
    config: null,
    configInvalid: false,
    floppies: [],
    hardDisks: [],
    cd: seen.get('cd') ?? null,
    settings,
  };
  const config = seen.get('config');
  if (config !== undefined) {
    out.config = decodeConfigParam(config);
    out.configInvalid = out.config === null;
  }
  for (const [name, v] of seen) {
    if (/^fd\d+$/.test(name)) out.floppies.push({ slot: name, url: v });
    if (/^hd\d+$/.test(name)) out.hardDisks.push({ slot: name, url: v });
  }
  return out;
}

// ?speed= as the toolbar's pacing mode, or null when absent or unknown.
// Accepts the core's three names only (paced, accelerated, turbo), as the
// scheduler.mode attribute and the headless --speed flag do.
export function urlSchedulerMode(speed: string | null): SchedulerMode | null {
  switch ((speed ?? '').toLowerCase()) {
    case 'paced':
      return 'live';
    case 'accelerated':
      return 'accel';
    case 'turbo':
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
    setUrlBootStage('failed', 'The emulator was not ready to take the URL’s media.');
    return false;
  }
  const params = parseUrlMediaParams(rawParams);
  if (!hasUrlMedia(params)) return false;

  // Slot -> the path to attach it from: its persisted path, or undefined
  // when the fetch failed or the file was rejected.  One download at a time:
  // in parallel, every large image was in flight at once.
  const paths = new Map<string, string | undefined>();
  if (params.rom) {
    queueUrlFile('rom');
    if (params.romPair) queueUrlFile('rom2');
  }
  if (params.vrom) queueUrlFile('vrom');
  for (const fd of params.floppies) queueUrlFile(fd.slot);
  for (const hd of params.hardDisks) queueUrlFile(hd.slot);
  if (params.cd) queueUrlFile('cd');
  // A blank disk is listed under the name it will have (its extension is
  // the bus's, known once the model is).
  const hash = blankUrlHash(urlMediaEntries(params));
  for (const d of [...params.floppies, ...params.hardDisks]) {
    const blank = parseBlankValue(d.url);
    if (blank)
      updateUrlFile(d.slot, { name: blankDiskName(d.slot, blank.spec, hash, ''), blank: true });
  }

  // The ROM first: without it nothing boots, so the disks (which can be
  // hundreds of megabytes) are not fetched for nothing.
  if (params.rom && params.romPair)
    paths.set('rom', await fetchRomPair(params.rom, params.romPair));
  else if (params.rom) paths.set('rom', await fetchAndPersist('rom', params.rom, 'rom'));
  if (params.rom && !paths.get('rom')) {
    skipQueuedUrlFiles();
    setUrlBootStage(
      'failed',
      'There is no ROM to boot: it could not be downloaded, or is not a ROM.',
    );
    return false;
  }

  const wanted: Array<[string, string, MediaTypeId]> = [];
  if (params.vrom) wanted.push(['vrom', params.vrom, 'vrom']);
  for (const fd of params.floppies) wanted.push([fd.slot, fd.url, 'fd']);
  for (const hd of params.hardDisks) wanted.push([hd.slot, hd.url, 'hd']);
  if (params.cd) wanted.push(['cd', params.cd, 'cdrom']);
  for (const [slot, url, category] of wanted) {
    const blank = parseBlankValue(url);
    // A blank hard disk waits for the model (below).
    if (blank && category === 'hd') continue;
    if (blank && category === 'fd')
      paths.set(slot, await provideBlank(slot, blank, floppyBlankPlan(blank.spec), FD_DIR, hash));
    else paths.set(slot, await fetchAndPersist(slot, url, category));
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
  if (!romPath) {
    setUrlBootStage(
      'failed',
      'There is no ROM to boot: it could not be downloaded, or is not a ROM.',
    );
    return false;
  }
  const info = await identifyRom(gsEval, romPath);
  if (!info || !info.compatible.length) {
    showNotification('Unrecognised ROM in URL params', 'error');
    setUrlBootStage(
      'failed',
      'The ROM was downloaded, but it is not one of a machine Granny Smith emulates.',
    );
    return false;
  }
  setUrlBootStage('booting');
  const chosen =
    params.model && info.compatible.includes(params.model) ? params.model : info.compatible[0];

  // One boot document: the core validates model/rom together, installs the
  // ROM itself and boots the model's own default RAM (there was a 4096 KB
  // fallback here, which two models cannot boot).  The URL's vROM is part of
  // it -- vrom= is the ROM of its card's slot -- unless it could not be
  // had (not downloaded, or not a vROM): then the boot goes ahead without
  // it, and says so.
  const vrom = paths.get('vrom');
  if (params.vrom && !vrom)
    showNotification(`Booting ${chosen} without the URL's video ROM`, 'warning');
  if (params.configInvalid)
    showNotification('The URL’s config= is not a configuration; booting the default', 'warning');
  const profile = await getProfile(chosen);
  for (const hd of params.hardDisks) {
    const blank = parseBlankValue(hd.url);
    if (!blank) continue;
    const n = parseInt(hd.slot.replace('hd', ''), 10);
    const bus = profile ? hardDiskBus(profile, n) : null;
    const plan: BlankPlan = bus
      ? hardDiskBlankPlan(blank.spec, bus.blank_disks)
      : { ok: false, reason: `${chosen} has no hard disk ${n + 1} to create a blank disk for` };
    paths.set(hd.slot, await provideBlank(hd.slot, blank, plan, HD_DIR, hash));
  }
  let config = urlConfig(profile, params);
  if (profile) {
    const applied = applyUrlConfig(profile, config, params.settings);
    config = applied.config;
    for (const w of applied.warnings) showNotification(`${w}; booting without it`, 'warning');
  }
  const booted = await gsEval('machine.boot', {
    model: chosen,
    rom: romPath,
    ...(vrom ? { vrom } : {}),
    ...(config ? { config: JSON.stringify(config) } : {}),
  });
  if (booted !== true) {
    // A rejected document leaves the previous machine (or none) in place:
    // do not attach media to it or report a boot.
    showNotification(
      `Could not boot ${chosen} from URL parameters: ${gsErrorText(booted)}`,
      'error',
    );
    setUrlBootStage('failed', `Could not boot ${chosen}: ${gsErrorText(booted)}`);
    return false;
  }

  await insertUrlFloppies(params, paths);
  // ?hdN= is the default configuration's N-th hard disk (hd0 is the startup
  // disk, which the configuration names), on whatever bus it is -- not SCSI
  // id N on the first bus.
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

// The URL's media parameters as [canonical name, value] pairs, in a fixed
// order: what a blank disk's name is hashed from (lib/blankMedia.ts), so the
// same link names the same disks however its parameters are ordered.
function urlMediaEntries(params: UrlMediaParams): Array<[string, string]> {
  const out: Array<[string, string]> = [];
  if (params.rom) out.push(['rom', params.rom]);
  if (params.romPair) out.push(['rom2', params.romPair]);
  if (params.vrom) out.push(['vrom', params.vrom]);
  const bySlot = (a: { slot: string }, b: { slot: string }) =>
    parseInt(a.slot.slice(2), 10) - parseInt(b.slot.slice(2), 10);
  for (const fd of [...params.floppies].sort(bySlot)) out.push([fd.slot, fd.url]);
  for (const hd of [...params.hardDisks].sort(bySlot)) out.push([hd.slot, hd.url]);
  if (params.cd) out.push(['cd', params.cd]);
  return out;
}

// The bus the default configuration's `n`-th hard disk is on -- the one
// machine.attach_hd(path, n) attaches to -- or null when there is none.
function hardDiskBus(profile: MachineProfile, n: number): StorageBus | null {
  const dev = profile.defaults.storage.filter((d) => d.type === 'hd')[n];
  return dev ? (profile.storage.find((b) => b.id === dev.bus) ?? null) : null;
}

// `slot`'s blank disk, in `dir`, by `plan`: the one an earlier load of this
// URL created when it is there (blank: -- reloading the link does not pile
// up blank images), else a new one (and always a new one for blank!:,
// which removes the old).  Created the way the New Machine dialog's Create
// blank image does (CreateImageDialog.svelte), plain blank for the guest to
// format.  Returns the path to attach from, or undefined -- the boot goes
// ahead without it -- with the reason in the progress view and a toast,
// as for a failed download.
async function provideBlank(
  slot: string,
  blank: BlankSpec,
  plan: BlankPlan,
  dir: string,
  hash: string,
): Promise<string | undefined> {
  const label = slot.toUpperCase();
  const fail = (why: string): undefined => {
    showNotification(`${label}: ${why}`, 'error');
    updateUrlFile(slot, { status: 'failed', error: why });
    return undefined;
  };
  if (!plan.ok) return fail(plan.reason);
  const name = blankDiskName(slot, blank.spec, hash, plan.ext);
  const path = `${dir}/${name}`;
  updateUrlFile(slot, { name, blank: true });
  if ((await gsEval('files.path_exists', [path])) === true) {
    if (!blank.fresh) {
      updateUrlFile(slot, { status: 'done', reused: true });
      if (!urlBoot.requested) showNotification(`${label}: ${name} (blank, already stored)`, 'info');
      return path;
    }
    const rm = await gsEval('files.rm', [path]);
    if (!gsOk(rm)) return fail(`could not replace ${name}: ${gsErrorText(rm)}`);
  }
  const made = await gsEval(plan.method, [path, plan.arg]);
  if (made !== true) {
    // The core parses an hd_create spec; its refusal is the spec's fault.
    if (plan.method === 'files.hd_create') return fail(hardDiskSpecRefused(blank.spec));
    return fail(`could not create ${name}: ${gsErrorText(made)}`);
  }
  updateUrlFile(slot, { status: 'done' });
  if (!urlBoot.requested) showNotification(`${label}: ${name} (new blank disk)`, 'info');
  return path;
}

// The document to boot: ?config= as given, else -- when an ?fdN= names a
// floppy position the default configuration leaves empty -- the default
// configuration with a drive there (the position's first drive type), else
// null for the default configuration itself.
export function urlConfig(
  profile: MachineProfile | null,
  params: Pick<UrlMediaParams, 'config' | 'floppies'>,
): Record<string, unknown> | null {
  if (params.config) return params.config;
  if (!profile) return null;
  let doc: ConfigDocument | null = null;
  for (const fd of params.floppies) {
    const pos = profile.floppies.find((f) => f.id === fd.slot);
    if (!pos || (profile.defaults.floppies[pos.id] ?? pos.default) !== 'none') continue;
    const type = pos.types.find((t) => t.id !== 'none');
    if (!type) continue;
    doc ??= JSON.parse(JSON.stringify(profile.defaults)) as ConfigDocument;
    doc.floppies[pos.id] = type.id;
  }
  // A drive at a later position needs the ones before it.
  if (doc) {
    for (const pos of profile.floppies) {
      if (doc.floppies[pos.id] !== 'none') continue;
      const later = profile.floppies.slice(profile.floppies.indexOf(pos) + 1);
      const type = pos.types.find((t) => t.id !== 'none');
      if (type && later.some((l) => doc!.floppies[l.id] && doc!.floppies[l.id] !== 'none'))
        doc.floppies[pos.id] = type.id;
    }
  }
  return doc as Record<string, unknown> | null;
}

// ?fdN= goes into drive N -- it always went into drive 0, so ?fd0=a&fd1=b
// left b refused and dropped -- and only a drive the machine has.
async function insertUrlFloppies(
  params: UrlMediaParams,
  paths: Map<string, string | undefined>,
): Promise<void> {
  const drives = await detectFdDriveCount(true);
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

// An image of `category` an earlier download of `url` stored: one whose
// UDIF records that exact URL as its origin (gs-origin, written by the
// import; files.udif_info reads it back).  The URL is compared as given --
// two spellings of one file are two URLs.  A stored image is never written
// to (a machine's writes go to a delta of its own), so it is still what was
// downloaded.  Answers its path, the progress view and the run told, or
// null: then it is downloaded.  Only UDIF images carry an origin, which is
// why every hard disk or CD a URL brings is stored as one, whatever its size
// and however it arrived (fetchAndImport, storeCompact).
async function storedFromUrl(
  slot: string,
  url: string,
  category: MediaTypeId,
): Promise<string | null> {
  const origin = url.trim();
  const dir = MEDIA_TYPES[category].persistDir;
  const entries = await gsEval('files.list', [dir]);
  if (!Array.isArray(entries)) return null;
  for (const e of entries as { name?: unknown; kind?: unknown }[]) {
    if (typeof e?.name !== 'string' || e.kind === 'directory' || !/\.dmg$/i.test(e.name)) continue;
    const path = `${dir}/${e.name}`;
    const info = (await gsEval('files.udif_info', [path])) as { origin?: unknown } | null;
    if (!info || typeof info !== 'object' || info.origin !== origin) continue;
    const size = await gsEval('files.path_size', [path]);
    const bytes = typeof size === 'number' ? size : 0;
    updateUrlFile(slot, {
      name: e.name,
      status: 'done',
      reused: true,
      received: bytes,
      total: bytes,
    });
    if (!urlBoot.requested)
      showNotification(`${slot.toUpperCase()}: ${e.name} (already stored, not downloaded)`, 'info');
    return path;
  }
  return null;
}

// Fetch a URL, stage it, and persist it as `category`.  Returns the
// persisted /opfs/images/<category>/ path to attach from, or undefined when
// the fetch failed or the file is not valid as that category (rejected,
// with the validator's reason).  The staging copy is discarded either way.
async function fetchAndPersist(
  slot: string,
  url: string,
  category: MediaTypeId,
): Promise<string | undefined> {
  if (category === 'hd' || category === 'cdrom') {
    const stored = await storedFromUrl(slot, url, category);
    if (stored) return stored;
    const imported = await fetchAndImport(slot, url, category);
    if (imported !== false) return imported;
  }
  const staged = await fetchAndStage(slot, url);
  if (!staged) return undefined;
  try {
    if (category === 'hd' || category === 'cdrom') {
      const stored = await storeCompact(slot, staged, url, category);
      if (stored !== false) return stored;
    }
    const stored = await persistAs(staged.path, staged.name, category);
    if (stored.ok) return stored.path;
    rejectDownload(slot, staged.name, stored.reason);
    return undefined;
  } finally {
    await discardStaging(staged.path);
  }
}

// Store a staged hard disk or CD as a compact UDIF recording `url` as its
// origin, so the next boot of the same link finds it (storedFromUrl) instead
// of downloading it again.  For the disks the streamed import leaves to the
// staged flow: a small one (onSmall), and a Mac archive member that import
// refuses (a Disk Copy 6 image -- NDIF -- unpacked, then converted).  Returns the
// stored path, undefined when the image was rejected, or false when the core
// cannot read the staged file as a disk (the caller stores it as it is, and
// the validator has its say).
async function storeCompact(
  slot: string,
  staged: { path: string; name: string },
  url: string,
  category: DiskCategory,
): Promise<string | undefined | false> {
  const part = scratchPath(`url_${slot}.dmg.part`);
  try {
    const r = await gsEval('files.convert', [
      staged.path,
      part,
      64,
      1,
      'udif',
      staged.name,
      url.trim(),
    ]);
    if (!r || typeof r !== 'object' || 'error' in (r as object)) return false;
    const name = `${staged.name}.dmg`;
    const stored = await persistAs(part, name, category);
    if (stored.ok) {
      updateUrlFile(slot, { name: stored.path.split('/').pop() ?? name });
      return stored.path;
    }
    rejectDownload(slot, staged.name, stored.reason);
    return undefined;
  } finally {
    await discardStaging(part);
  }
}

// A download its slot's category refuses: rejected as the same file dropped
// on that category is, with the validator's reason, and never attached.
function rejectDownload(slot: string, name: string, reason: string): void {
  const why = `'${name}' ${reason}`;
  showNotification(`${slot.toUpperCase()}: ${why}`, 'error');
  updateUrlFile(slot, { status: 'failed', error: why });
}

// Fetch a hard-disk or CD value and stream it into a compact UDIF
// (bus/importImage.ts): the body -- or the named member of a zip body, or a
// gzip body's content -- goes into the core's writer as it arrives, so a
// 45 MB download of a 2 GB disk never needs 2 GB.  A small file is staged,
// then stored as a UDIF too (storeCompact): only a UDIF records the URL it
// came from, which is what lets the next boot of the link reuse it.  Returns the path to attach from, undefined when
// nothing was stored, or false when the body is a Mac archive, which the
// staged flow unpacks.
async function fetchAndImport(
  slot: string,
  url: string,
  category: DiskCategory,
): Promise<string | undefined | false> {
  const label = slot.toUpperCase();
  let plan: MediaFetchPlan;
  try {
    plan = planMediaFetch(url, window.location.href);
  } catch (e) {
    const msg = fetchFailureText(e, url);
    showNotification(`${label}: ${msg}`, 'error');
    updateUrlFile(slot, { status: 'failed', error: msg });
    return undefined;
  }
  const storeAs = urlMediaNameFor(slot);
  updateUrlFile(slot, { name: storeAs, status: 'downloading' });
  let res: Response;
  try {
    try {
      res = await fetch(plan.fetchUrl);
    } catch (e) {
      throw new MediaUrlError(fetchFailureText(e, plan.fetchUrl));
    }
    if (!res.ok) {
      const what = res.status === 404 ? 'not found' : `${res.status} ${res.statusText}`;
      throw new MediaUrlError(`${plan.containerName ?? plan.fileName}: ${what}`);
    }
  } catch (e) {
    const msg = fetchFailureText(e, plan.fetchUrl);
    showNotification(`${label}: ${msg}`, 'error');
    updateUrlFile(slot, { status: 'failed', error: msg });
    return undefined;
  }
  const length = Number(res.headers.get('Content-Length'));
  const total = Number.isFinite(length) && length > 0 ? length : null;
  updateUrlFile(slot, { total });
  const source: ImportSource = res.body
    ? { kind: 'stream', stream: res.body, total }
    : { kind: 'blob', blob: await res.blob() };
  const progress = progressReporter(slot);
  let rejected = false;
  // In the status bar like an upload, with its Cancel button (importImage
  // sets it while the import can be cancelled).
  startActivity(storeAs, 'Downloading');
  const out = await importImage(
    source,
    plan.container === 'zip' ? plan.fileName : (plan.containerName ?? plan.fileName),
    {
      categories: [category],
      member: plan.container ? plan.member : null,
      storeAs,
      origin: url.trim(),
      onProgress: (read) => progress(read),
      // importImage discards the staged file when this returns.
      onSmall: async (path) => {
        const compact = await storeCompact(slot, { path, name: storeAs }, url, category);
        if (compact !== false) {
          rejected = compact === undefined;
          return compact ?? null;
        }
        const stored = await persistAs(path, storeAs, category);
        if (stored.ok) return stored.path;
        rejectDownload(slot, storeAs, stored.reason);
        rejected = true;
        return null;
      },
    },
  ).finally(endActivity);
  if (!out.handled) return false;
  if (!out.path) {
    if (!rejected) updateUrlFile(slot, { status: 'failed', error: 'not stored' });
    return undefined;
  }
  const size = await gsEval('files.path_size', [out.path]);
  const name = out.path.split('/').pop() ?? plan.fileName;
  if (!urlBoot.requested)
    showNotification(
      `${label}: ${name}${typeof size === 'number' ? ` (${sizeText(size)} stored)` : ''}`,
      'info',
    );
  updateUrlFile(slot, { name, status: 'done' });
  return out.path;
}

// Fetch the two chips of a ROM dumped as byte-wide halves, interleave them
// into one image, and persist it as a ROM.  Which chip holds the even bytes
// is not for the URL to say: both orders are tried and the one whose own
// checksum verifies (machine.rom.identify) is kept.  Returns the persisted
// path, or undefined (with a message) when the pair is not a ROM.  Both
// halves' scratch files are discarded on every exit.
async function fetchRomPair(urlA: string, urlB: string): Promise<string | undefined> {
  const a = await fetchAndStage('rom', urlA);
  if (!a) return undefined;
  let b: { path: string; name: string } | null = null;
  try {
    b = await fetchAndStage('rom2', urlB);
    if (!b) return undefined;
    const bytesA = await xferReadAll(a.path);
    const bytesB = await xferReadAll(b.path);
    if (bytesA.length !== bytesB.length) {
      showNotification(
        `ROM: the two halves differ in size (${bytesA.length} and ${bytesB.length} bytes)`,
        'error',
      );
      return undefined;
    }
    for (const [even, odd] of [
      [bytesA, bytesB],
      [bytesB, bytesA],
    ]) {
      if (!(await streamToOpfs(a.path, interleaveHalves(even, odd)))) return undefined;
      const id = (await gsEval('machine.rom.identify', [a.path])) as { intact?: boolean } | null;
      if (id?.intact) {
        const name = `${a.name}+${b.name}`;
        const stored = await persistAs(a.path, name, 'rom');
        if (stored.ok) return stored.path;
        rejectDownload('rom', name, stored.reason);
        return undefined;
      }
    }
    showNotification(
      `ROM: ${a.name} and ${b.name} do not interleave into a ROM whose checksum verifies`,
      'error',
    );
    return undefined;
  } finally {
    await discardStaging(a.path);
    if (b) await discardStaging(b.path);
  }
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

// Fetch a URL-media value and stage its bytes at a scratch path of its own
// (scratchPath, so a later URL boot of the same slot never writes the file a
// running machine may still be reading), streamed through the one chunked writer (upload.ts streamToOpfs).  When
// the value names a member of a container, the container is fetched whole
// and the member taken out of it (a zip in JS — unzipping needs the whole
// archive in memory — a Mac archive by the C side); archive.org extracts zip
// members itself.  A container fetched without a member path keeps the old
// behaviour: a zip's first file, a Mac archive's found medium.  Returns the
// staged path and the name to store it under -- the caller discards it --
// or null, with nothing left behind.
async function fetchAndStage(
  slot: string,
  url: string,
): Promise<{ path: string; name: string } | null> {
  const label = slot.toUpperCase();
  let plan: MediaFetchPlan;
  try {
    plan = planMediaFetch(url, window.location.href);
  } catch (e) {
    const msg = fetchFailureText(e, url);
    showNotification(`${label}: ${msg}`, 'error');
    updateUrlFile(slot, { status: 'failed', error: msg });
    return null;
  }
  const storeAs = urlMediaNameFor(slot);
  updateUrlFile(slot, { name: storeAs, status: 'downloading' });
  const staged = scratchPath(`url_${slot}`);
  let handedOver = false;
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
    const length = Number(res.headers.get('Content-Length'));
    const total = Number.isFinite(length) && length > 0 ? length : null;
    updateUrlFile(slot, { total });
    const body = res.body ?? (await res.blob());
    if (!(await streamToOpfs(staged, body, progressReporter(slot)))) {
      updateUrlFile(slot, { status: 'failed', error: 'could not store the download' });
      return null;
    }
    // Whether the download is an archive is the core's call, from its content.
    const archive = await stagedArchiveFormat(staged);
    if (plan.member !== null || archive) updateUrlFile(slot, { status: 'unpacking' });

    const ct = res.headers.get('Content-Type') ?? '';
    if (plan.member !== null) {
      // The value named a member: take exactly that one out.
      if (!(await extractMember(slot, staged, plan))) return null;
    } else if (archive === 'zip' || /zip/i.test(ct)) {
      // A bare zip: its first file, as before member paths existed.
      updateUrlFile(slot, { status: 'unpacking' });
      const first = (await unzipAll(await xferReadAll(staged)))[0];
      if (!first) throw new MediaUrlError(`${plan.fileName}: the zip is empty`);
      if (!(await streamToOpfs(staged, first.data))) return null;
    } else if (archive) {
      await unpackMacArchive(slot, staged, null);
    }

    const size = await gsEval('files.path_size', [staged]);
    const sz = typeof size === 'number' ? ` (${sizeText(size)})` : '';
    // The progress view lists each file as it lands; a toast per file is
    // for a page that is not showing it.
    if (!urlBoot.requested) showNotification(`${label}: ${storeAs}${sz}`, 'info');
    updateUrlFile(slot, {
      status: 'done',
      ...(typeof size === 'number' ? { received: size, total: size } : {}),
    });
    handedOver = true;
    return { path: staged, name: storeAs };
  } catch (e) {
    console.error(`[urlMedia] fetch ${slot} failed`, e);
    const msg = fetchFailureText(e, plan.fetchUrl);
    showNotification(`${label}: ${msg}`, 'error');
    updateUrlFile(slot, { status: 'failed', error: msg });
    return null;
  } finally {
    // Every exit but the one handing the file to the caller discards it.
    if (!handedOver) await discardStaging(staged);
  }
}

// A streamToOpfs progress callback for `slot`'s row in the progress view, at
// most ten updates a second (a large download reports every network chunk).
function progressReporter(slot: string): (bytes: number) => void {
  let last = 0;
  return (bytes) => {
    const now = performance.now();
    if (now - last < 100) return;
    last = now;
    updateUrlFile(slot, { received: bytes });
  };
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
// MacBinary) and put `member` — or, with none, the medium files.find_media
// picks — in its place.
async function unpackMacArchive(
  slot: string,
  staged: string,
  member: string | null,
  plan?: MediaFetchPlan,
): Promise<boolean> {
  const fmt = await gsEval('files.archive.identify', [staged]);
  if (typeof fmt !== 'string' || fmt.length === 0) {
    if (member !== null && plan) throw new MediaUrlError(`${plan.containerName}: not an archive`);
    return true;
  }
  const extractDir = scratchPath(`url_${slot}_unpacked`);
  try {
    if ((await gsEval('files.archive.extract', [staged, extractDir])) !== true)
      throw new MediaUrlError(`could not unpack ${plan?.containerName ?? 'the archive'}`);
    if (member === null) {
      await gsEval('files.find_media', [extractDir, staged]);
      return true;
    }
    const names = await listTree(extractDir);
    const hit = findMember(names, member);
    if (!hit) throw memberNotFound(plan as MediaFetchPlan, names);
    return (await gsEval('files.cp', [`${extractDir}/${hit}`, staged])) === true;
  } finally {
    await discardStaging(extractDir);
  }
}

// Every file under `dir`, as paths relative to it.  Each entry says what it is
// (files.list), so a subdirectory that cannot be listed is an error -- the
// URL's media is rejected -- never taken for a file.
async function listTree(dir: string, prefix = ''): Promise<string[]> {
  const at = prefix ? `${dir}/${prefix}` : dir;
  const entries = await gsEval('files.list', [at]);
  // An unreadable or oversized listing is the core's error: said, not taken
  // for an empty one.
  if (!Array.isArray(entries))
    throw new MediaUrlError(`cannot list ${at}: ${gsErrorText(entries)}`);
  const out: string[] = [];
  for (const e of entries as { name?: unknown; kind?: unknown }[]) {
    if (typeof e?.name !== 'string' || e.name === '.' || e.name === '..') continue;
    const rel = prefix ? `${prefix}/${e.name}` : e.name;
    if (e.kind === 'directory') out.push(...(await listTree(dir, rel)));
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
