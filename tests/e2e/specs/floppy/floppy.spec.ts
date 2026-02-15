// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test } from '../../fixtures';
import { matchScreenFast } from '../../helpers/screen';
import { bootWithMedia, bootWithUploadedMedia } from '../../helpers/boot';
import { mouseClick, mouseDoubleClick, mouseDrag } from '../../helpers/mouse';
import { runCommand } from '../../helpers/run-command';
import { expect } from '@playwright/test';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const FD_REL = 'systems/System_6_0_8.dsk';

// Helper options
const SHORT_MATCH = { initialWaitMs: 2000, timeoutMs: 10_000 } as const;
const LONG_MATCH = { initialWaitMs: 2000, timeoutMs: 180_000 } as const; // up to 3 minutes

test.describe('Floppy', () => {
  test('floppy disk interactions', async ({ page, log }) => {
    test.setTimeout(360_000);
    log('[floppy] booting with System 6.0.8 floppy');
    await bootWithMedia(page, ROM_REL, FD_REL);

    await runCommand(page, 'schedule max');

    // Start with a baseline match.
    await matchScreenFast(page, 'floppy-1', { initialWaitMs: 2000, waitBeforeUpdateMs: 60_000, timeoutMs: 60_000 });

    // Create a new empty floppy via command.
    await runCommand(page, 'new-fd /tmp/my-empty-fd');

    // Progressively interact with the UI per steps provided.
    // Note: With proper TACH timing for motor speed measurement, disk operations take longer
    await matchScreenFast(page, 'floppy-2', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

    await mouseClick(page, 342, 159);
    await matchScreenFast(page, 'floppy-3', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

    await mouseClick(page, 342, 159);
    await matchScreenFast(page, 'floppy-4', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

    await mouseClick(page, 254, 159);
    await matchScreenFast(page, 'floppy-5', { initialWaitMs: 2000, waitBeforeUpdateMs: 180_000, timeoutMs: 120_000 });

    await mouseDrag(page, 42, 102, 355, 213); // select files on system disk
    await matchScreenFast(page, 'floppy-6', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

    await mouseDrag(page, 315, 184, 472, 113); // copy files to new disk
    await matchScreenFast(page, 'floppy-7', { initialWaitMs: 2000, waitBeforeUpdateMs: 180_000, timeoutMs: 180_000 });

    await mouseClick(page, 472, 113); // open new disk
    await mouseDrag(page, 50, 10, 50, 45); // select "open"
    await matchScreenFast(page, 'floppy-8', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

    await mouseDrag(page, 472, 113, 472, 310); // unmount disk
    await matchScreenFast(page, 'floppy-9', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 60_000 });

  });

  test('insert-fd probe command', async ({ page, log }) => {
    test.setTimeout(60_000);

    log('[insert-fd probe] setting up ROM (emulator not running)');
    await bootWithUploadedMedia(page, ROM_REL, undefined, undefined, { hideOverlay: true });

    await runCommand(page, 'schedule max');

    log('[insert-fd probe] creating valid floppy image');

    // Create a valid 800KB floppy image
    await runCommand(page, 'new-fd /tmp/valid-floppy.dsk');
    await page.waitForTimeout(500);

    log('[insert-fd probe] testing positive case - valid floppy image');
    // Test positive case: valid floppy should be recognized (returns 0 for success)
    const positiveResult = await runCommand(page, 'insert-fd --probe /tmp/valid-floppy.dsk');

    expect(positiveResult).toBe(0);
    log('[insert-fd probe] positive case passed - floppy recognized');

    log('[insert-fd probe] testing negative case - ROM file (wrong size)');
    // Test negative case: ROM file should not be recognized as floppy (returns 1 for failure)
    // Use the ROM file already loaded in memfs at /persist/boot/rom
    const romResult = await runCommand(page, 'insert-fd --probe /persist/boot/rom');

    expect(romResult).toBe(1);
    log('[insert-fd probe] negative case passed - ROM file not recognized as floppy');

    log('[insert-fd probe] testing non-existent file');
    // Test non-existent file (returns 1 for failure)
    const nonExistentResult = await runCommand(page, 'insert-fd --probe /tmp/does-not-exist.dsk');

    expect(nonExistentResult).toBe(1);
    log('[insert-fd probe] negative case passed - non-existent file handled');

    log('[insert-fd probe] test complete');
  });
});
