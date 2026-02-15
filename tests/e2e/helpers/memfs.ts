// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import * as fs from 'fs';
import type { Page } from '@playwright/test';
import { runCommand } from './run-command';

/**
 * Download files from the emulator's memfs via the shell `download` command.
 *
 * Issues the `download <path>` shell command for each path, which triggers a
 * browser file-save. Playwright intercepts the download event, saves the file
 * to a temp location, and we read it back as UTF-8 text.
 *
 * Returns an object mapping each path to its content (UTF-8 decoded).
 * Missing or errored files will have empty string values.
 *
 * Usage:
 * ```
 * const files = await readMemfsFiles(page, ['/tmp/log1.txt', '/tmp/log2.txt']);
 * console.log(files['/tmp/log1.txt']);
 * ```
 */
export async function readMemfsFiles(
  page: Page,
  paths: string[]
): Promise<Record<string, string>> {
  const result: Record<string, string> = {};

  for (const memfsPath of paths) {
    try {
      // start listening before triggering the download
      const downloadPromise = page.waitForEvent('download', { timeout: 10_000 });
      await runCommand(page, `download "${memfsPath}"`);
      const download = await downloadPromise;

      // read the saved temp file
      const tempPath = await download.path();
      if (tempPath) {
        result[memfsPath] = fs.readFileSync(tempPath, 'utf-8');
      } else {
        result[memfsPath] = '';
      }
    } catch {
      result[memfsPath] = '';
    }
  }

  return result;
}
