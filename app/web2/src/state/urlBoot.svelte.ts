// Reactive state for a boot straight from URL parameters (?rom=…&hd0=…).
// When the page was opened to boot a machine, it shows the download progress
// view instead of Welcome and asks nothing: no "preview build" notice, no
// "resume the saved machine?" prompt.  The orchestration lives in
// bus/urlMedia.ts; this module owns the state the view renders.

// One file being fetched.  `total` is null when the server sends no length
// (a streamed zip member, a compressed response): the bar is then
// indeterminate and only `received` is shown.
export type UrlFileStatus = 'queued' | 'downloading' | 'unpacking' | 'done' | 'failed' | 'skipped';

export interface UrlFile {
  slot: string; // rom, rom2, vrom, fd0, hd0, cd …
  label: string; // "ROM", "Hard disk 1" …
  name: string; // the file's name, as the URL gives it
  received: number;
  total: number | null;
  status: UrlFileStatus;
  error: string | null;
}

// downloading: files are coming in; booting: all fetched, the machine is
// being set up; failed: the boot cannot go ahead (the view says why and
// offers the start screen).
export type UrlBootStage = 'downloading' | 'booting' | 'failed';

interface UrlBootState {
  // The page was opened to boot from URL parameters.  Set before mount and
  // never cleared: it is what keeps the start-up dialogs away.
  requested: boolean;
  // The progress view is up (until the machine runs, or the user leaves a
  // failed boot for the start screen).
  showProgress: boolean;
  model: string | null;
  stage: UrlBootStage;
  files: UrlFile[];
  error: string | null;
}

export const urlBoot: UrlBootState = $state({
  requested: false,
  showProgress: false,
  model: null,
  stage: 'downloading',
  files: [],
  error: null,
});

// The page is booting from its URL: show the progress view.
export function beginUrlBoot(model: string | null): void {
  urlBoot.requested = true;
  urlBoot.showProgress = true;
  urlBoot.model = model;
  urlBoot.stage = 'downloading';
  urlBoot.files = [];
  urlBoot.error = null;
}

// The human name of a URL slot.
export function slotLabel(slot: string): string {
  if (slot === 'rom') return 'ROM';
  if (slot === 'rom2') return 'ROM (second chip)';
  if (slot === 'vrom') return 'Video card ROM';
  if (slot === 'cd') return 'CD-ROM';
  const m = /^(fd|hd)(\d+)$/.exec(slot);
  if (m) return `${m[1] === 'fd' ? 'Floppy disk' : 'Hard disk'} ${Number(m[2]) + 1}`;
  return slot.toUpperCase();
}

// The file name a URL names: its last path segment, decoded.
export function urlFileName(url: string): string {
  const last = url.split(/[?#]/)[0].split('/').filter(Boolean).pop() ?? url;
  try {
    return decodeURIComponent(last);
  } catch {
    return last;
  }
}

// List a file the boot will fetch (in the order the view shows them).
export function queueUrlFile(slot: string, url: string): void {
  if (!urlBoot.requested) return;
  urlBoot.files.push({
    slot,
    label: slotLabel(slot),
    name: urlFileName(url),
    received: 0,
    total: null,
    status: 'queued',
    error: null,
  });
}

// Update the listed file for `slot` (a no-op when none is listed, i.e. when
// the page is not booting from its URL).
export function updateUrlFile(slot: string, patch: Partial<Omit<UrlFile, 'slot' | 'label'>>): void {
  const f = urlBoot.files.find((x) => x.slot === slot);
  if (f) Object.assign(f, patch);
}

// The boot cannot go ahead: the files still waiting are not fetched.
export function skipQueuedUrlFiles(): void {
  for (const f of urlBoot.files) if (f.status === 'queued') f.status = 'skipped';
}

export function setUrlBootStage(stage: UrlBootStage, error: string | null = null): void {
  urlBoot.stage = stage;
  urlBoot.error = error;
}

// Leave a failed URL boot for the ordinary start screen.
export function dismissUrlBoot(): void {
  urlBoot.showProgress = false;
}

// For tests: back to a page that was not opened to boot.
export function _resetUrlBootForTests(): void {
  urlBoot.requested = false;
  urlBoot.showProgress = false;
  urlBoot.model = null;
  urlBoot.stage = 'downloading';
  urlBoot.files = [];
  urlBoot.error = null;
}
