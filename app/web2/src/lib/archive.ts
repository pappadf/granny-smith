// ZIP archive helpers. JSZip is dynamic-imported so it's a code-split chunk
// (only fetched the first time a user touches a .zip).

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

// Whether a file is an archive is not decided here, from its name: the
// core's format registry decides from its content (files.archive.identify,
// and the `expandable` flag files.list reports).

// Filename sanitiser — keep alphanumerics, ., _, -; collapse the rest to _.
export function sanitizeName(n: string): string {
  return n.replace(/[^A-Za-z0-9._-]+/g, '_');
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
