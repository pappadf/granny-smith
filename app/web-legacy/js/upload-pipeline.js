// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// Unified upload pipeline: stage -> validate -> extract -> persist -> cleanup.
// All media types (ROM, VROM, FD, HD, CD-ROM) follow the same flow,
// parameterized by the media type descriptor from media-types.js.

import { UPLOAD_DIR } from './config.js';
import { sanitizeName, isZipFile, isMacArchive, probeMacArchive,
         extractZipToDir, extractMacArchiveToDir } from './media.js';
import { writeToOPFS, removeFromOPFS, listDir } from './fs.js';
import { toast } from './ui.js';
import { showUploadDialog } from './dialogs.js';

// Run the unified upload pipeline for a given media type.
// Returns { path, name, info } on success, null on failure.
export async function runUploadPipeline(file, mediaType) {
  const safeName = sanitizeName(file.name) || 'image.img';

  // Step 1: Stage to /opfs/upload/ (bypasses /tmp, no WASM heap pressure)
  const stagingPath = `${UPLOAD_DIR}/${safeName}`;
  try {
    await writeToOPFS(stagingPath, file);
  } catch (err) {
    console.error('upload-pipeline: direct OPFS write failed', err);
    toast('Upload failed');
    return null;
  }

  // Step 2: Validate against expected media type
  const result = await mediaType.validate(stagingPath);

  if (result.valid) {
    return await persistAndCleanup(stagingPath, safeName, mediaType, result.info);
  }

  // Step 3: Not directly valid — try archive extraction
  const extractedFile = await tryArchiveExtraction(stagingPath, safeName, file, mediaType);

  if (extractedFile) {
    return await persistAndCleanup(extractedFile.path, extractedFile.name, mediaType, extractedFile.info);
  }

  // Step 4: Nothing worked — show error and clean up
  showUploadDialog(
    `"${file.name}" does not appear to be a valid ${mediaType.label}. ` +
    `If it was an archive, no valid ${mediaType.label} was found inside it.`
  );
  await cleanup(stagingPath);
  return null;
}

// Attempt to extract an archive and find a valid media file inside.
async function tryArchiveExtraction(filePath, displayName, file, mediaType) {
  const isArchive = isZipFile(displayName) || isMacArchive(displayName)
                    || await probeMacArchive(filePath);

  if (!isArchive) return null;

  toast(`Extracting ${displayName}...`);

  const extractDir = `${UPLOAD_DIR}/${sanitizeName(displayName)}_unpacked`;

  let extractedPaths = [];

  if (isZipFile(displayName)) {
    try {
      // ZIP extraction needs data as Uint8Array — read from File object
      const data = new Uint8Array(await file.arrayBuffer());
      extractedPaths = await extractZipToDir(data, extractDir);
    } catch (err) {
      console.error('ZIP extraction failed:', err);
      toast(`Failed to extract ZIP: ${displayName}`);
      return null;
    }
  } else {
    // Mac-archive extraction (sit, hqx, cpt, bin, sea)
    const ok = await extractMacArchiveToDir(filePath, extractDir);
    if (!ok) {
      toast(`Failed to extract archive: ${displayName}`);
      return null;
    }
    extractedPaths = await listExtractedFiles(extractDir);
  }

  if (extractedPaths.length === 0) {
    toast(`Archive was empty: ${displayName}`);
    return null;
  }

  // Validate each extracted file against the expected media type
  for (const extractedPath of extractedPaths) {
    const result = await mediaType.validate(extractedPath);
    if (result.valid) {
      // Sanitize the extracted filename for persistence (may contain spaces)
      const rawName = extractedPath.split('/').pop();
      const safeName = sanitizeName(rawName) || rawName;
      toast(`Found valid ${mediaType.label} in archive: ${rawName}`);
      return { path: extractedPath, name: safeName, info: result.info };
    }
  }

  // No valid media found in archive
  return null;
}

// Move the validated file to its final /opfs/images/<type>/ location.
async function persistAndCleanup(sourcePath, fileName, mediaType, info) {
  const finalName = mediaType.nameFn
    ? mediaType.nameFn(fileName, info)
    : fileName;
  // Allow validator to override persistDir (e.g. floppy FD vs FDHD)
  const targetDir = info?.persistDir || mediaType.persistDir;
  const finalPath = `${targetDir}/${finalName}`;

  const ok = await window.gsEval('storage.cp', [sourcePath, finalPath]);
  if (ok !== true) {
    console.error(`upload-pipeline: failed to persist ${sourcePath} -> ${finalPath}`);
    toast(`Failed to save ${fileName}`);
    return null;
  }

  // Best-effort cleanup of staging area
  await cleanup(sourcePath);

  toast(`${fileName} uploaded`);
  return { path: finalPath, name: finalName, info };
}

// Clean up staging files (best-effort, non-fatal).
async function cleanup(path) {
  await removeFromOPFS(path);
  // Also clean up extraction directory if it exists
  const unpackedDir = path.replace(/\.[^.]+$/, '') + '_unpacked';
  await removeFromOPFS(unpackedDir);
}

// Recursively list all files in an /opfs/ directory tree.
async function listExtractedFiles(dirPath) {
  const results = [];
  const entries = await listDir(dirPath);
  for (const entry of entries) {
    const fullPath = `${dirPath}/${entry.name}`;
    if (entry.kind === 'file') {
      // Skip AppleDouble sidecar files (resource forks)
      if (!entry.name.startsWith('._')) {
        results.push(fullPath);
      }
    } else if (entry.kind === 'directory') {
      // Recurse into subdirectories (some archives create folders, e.g. SIT)
      const subFiles = await listExtractedFiles(fullPath);
      results.push(...subFiles);
    }
  }
  return results;
}
