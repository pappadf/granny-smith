// ZIP archive helpers. JSZip is dynamic-imported so it's a code-split chunk
// (only fetched the first time a user touches a .zip), per plan-doc §5.

// JSZip ships an awkward ESM shim: the namespace itself is the class, but
// TS sees both the namespace and a default export. We treat the module as
// an opaque object with `loadAsync` and let runtime work it out.
interface JSZipLike {
  loadAsync(data: Uint8Array): Promise<{
    files: Record<string, { dir: boolean; async(kind: 'uint8array'): Promise<Uint8Array> }>;
  }>;
}

let jszipPromise: Promise<JSZipLike> | null = null;

async function loadJSZip(): Promise<JSZipLike> {
  if (!jszipPromise) {
    jszipPromise = import('jszip').then((m) => {
      const mod = m as unknown as { default?: JSZipLike } & JSZipLike;
      return mod.default ?? mod;
    });
  }
  return jszipPromise;
}

const ARCHIVE_EXT = /\.(sit|hqx|cpt|bin|sea)$/i;
const ZIP_EXT = /\.zip$/i;

export function isZipFile(name: string): boolean {
  return ZIP_EXT.test(name || '');
}

// Mac-archive extensions handled by the C-side archive module (not JSZip).
export function isMacArchive(name: string): boolean {
  return ARCHIVE_EXT.test(name || '');
}

export function isArchiveFile(name: string): boolean {
  return isZipFile(name) || isMacArchive(name);
}

// Filename sanitiser — keep alphanumerics, ., _, -; collapse the rest to _.
export function sanitizeName(n: string): string {
  return n.replace(/[^A-Za-z0-9._-]+/g, '_');
}

// First-4-byte check for the local file header magic of a ZIP file.
export function isZipMagic(buf: Uint8Array): boolean {
  // PK\x03\x04
  return (
    buf.length >= 4 && buf[0] === 0x50 && buf[1] === 0x4b && buf[2] === 0x03 && buf[3] === 0x04
  );
}

export interface UnzippedFile {
  name: string;
  data: Uint8Array;
}

// Extract every file in `data`. Skips directory entries. Returns them in
// archive order (preserving the order the upstream zip was authored in).
export async function unzipAll(data: Uint8Array): Promise<UnzippedFile[]> {
  const JSZip = await loadJSZip();
  const zip = await JSZip.loadAsync(data);
  const out: UnzippedFile[] = [];
  const names = Object.keys(zip.files);
  for (const name of names) {
    const entry = zip.files[name];
    if (entry.dir) continue;
    const bytes = await entry.async('uint8array');
    out.push({ name, data: bytes });
  }
  return out;
}

// Extract just the first non-directory entry; useful when the URL-media path
// expects a single image inside a zip wrapper.
export async function unzipFirstFile(data: Uint8Array): Promise<UnzippedFile | null> {
  const all = await unzipAll(data);
  return all[0] ?? null;
}
