// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';
import { bootWithUploadedMedia } from '../../helpers/boot';
import { runCommand } from '../../helpers/run-command';
import { dispatchDropEvent } from '../../helpers/drop';
import * as fs from 'fs';
import * as path from 'path';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const ARCHIVE_REL = 'apps/MacTest_Disk.image_.sit_.hqx';

test.describe('Peeler', () => {
  test('unpack MacTest_Disk.image_.sit_.hqx', async ({ page, log }) => {
    test.setTimeout(60_000);

    log('[peeler] setting up ROM (emulator not running)');
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });

    // Read the .hqx file from disk
    const archivePath = path.join(process.cwd(), 'tests', 'data', ARCHIVE_REL);
    const archiveData = new Uint8Array(fs.readFileSync(archivePath));

    log('[peeler] dropping archive file onto emulator');
    // Drop the archive file - it will be uploaded to /tmp/upload and auto-extracted
    const dropResult = await dispatchDropEvent(page, '#screen', 'MacTest_Disk.image_.sit_.hqx', archiveData);
    expect(dropResult).toBe(true);

    // Wait for upload and auto-extraction to complete
    await page.waitForTimeout(5_000);

    log('[peeler] verifying auto-extracted file exists');
    // The UI automatically extracts archives to /tmp/upload/<basename>_unpacked/
    // Verify the unpacked file exists (note: peeler preserves original filename with spaces)
    const exitCode = await runCommand(page, 'exists "/tmp/upload/MacTest_Disk.image_.sit__unpacked/MacTest Disk.image"');
    const fileExists = (exitCode === 0);

    expect(fileExists).toBe(true);

    log('[peeler] testing manual peeler extraction to different location');
    // Now test peeler command directly on the uploaded archive (still at /tmp/upload)
    // Extract to a different location to verify manual extraction works
    await runCommand(page, 'peeler -o /tmp /tmp/upload/MacTest_Disk.image_.sit_.hqx');

    // Small delay to allow command to complete
    await page.waitForTimeout(1_000);

    log('[peeler] verifying manually extracted file exists');
    // Verify the manually extracted file exists at the specified output location
    const manualExitCode = await runCommand(page, 'exists "/tmp/MacTest Disk.image"');
    const manualFileExists = (manualExitCode === 0);

    expect(manualFileExists).toBe(true);
    log('[peeler] test complete - file successfully unpacked');
  });

  test('probe supported and unsupported formats', async ({ page, log }) => {
    test.setTimeout(60_000);

    log('[probe] setting up ROM (emulator not running)');
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });

    // Drop the archive file (positive case)
    const archivePath = path.join(process.cwd(), 'tests', 'data', ARCHIVE_REL);
    const archiveData = new Uint8Array(fs.readFileSync(archivePath));

    log('[probe] dropping archive file onto emulator');
    const dropResult = await dispatchDropEvent(page, '#screen', 'MacTest_Disk.image_.sit_.hqx', archiveData);
    expect(dropResult).toBe(true);

    // Wait for upload and auto-extraction to complete
    await page.waitForTimeout(3_000);

    log('[probe] testing positive case - supported archive format');
    // Test positive case: archive should be recognized (returns 0 for success)
    // The archive file is at /tmp/upload/ even after auto-extraction
    const positiveResult = await runCommand(page, 'peeler --probe /tmp/upload/MacTest_Disk.image_.sit_.hqx');
    
    expect(positiveResult).toBe(0);
    log('[probe] positive case passed - archive recognized');

    log('[probe] testing negative case - unsupported file format (using ROM from /persist/boot/rom)');
    // Test negative case: Use the ROM file from /persist/boot/rom (no need to upload)
    const negativeResult = await runCommand(page, 'peeler --probe /persist/boot/rom');
    
    expect(negativeResult).toBe(1);
    log('[probe] negative case passed - ROM file not recognized');
    log('[probe] test complete');
  });
});
