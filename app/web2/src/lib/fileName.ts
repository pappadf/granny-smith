// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Names the page makes for files and folders, one function per intent.
// Several end up as stored names (blank disks, checkpoint downloads, the
// AFP cache), so each rule is fixed: change one only with what it renames.
// (The LaserWriter's document name, printer/platenRing.ts documentName,
// mirrors the core's printer rule and stays beside it.)

// OPFS-safe, keeping the user's name: replace the characters OPFS or
// Windows reject (notably ':' which the HFS reader produces from an
// in-name '/') and trim trailing dots/spaces.  Spaces and leading dots
// are kept; `fallback` when nothing is left.
export function safeFileName(name: string, fallback = 'untitled'): string {
  return name.replace(/[\\/:*?"<>|]/g, '_').replace(/[ .]+$/g, '') || fallback;
}

// Conservative ASCII: keep alphanumerics, '.', '_' and '-'; everything else
// becomes '_', a run of it one '_' (or, with collapse false, one per
// character).  May return "".
export function sanitizeName(name: string, opts: { collapse?: boolean } = {}): string {
  return name.replace(opts.collapse === false ? /[^A-Za-z0-9._-]/g : /[^A-Za-z0-9._-]+/g, '_');
}

// A folder-name slug: runs of anything but letters and digits become one
// '-', none at either end; `fallback` when nothing is left.
export function slug(s: string, fallback: string): string {
  return s.replace(/[^A-Za-z0-9]+/g, '-').replace(/^-|-$/g, '') || fallback;
}
