// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';
import { matchScreenFast } from '../../helpers/screen';
import { mouseClick, mouseDrag } from '../../helpers/mouse';
import { runCommand, waitForPrompt } from '../../helpers/run-command';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const MACTEST_ARCHIVE_REL = 'apps/MacTest_Disk.image_.sit_.hqx';

test.describe('MacTest Application', () => {
  test('boot system and run MacTest from archive', async ({ page, log }) => {
    test.setTimeout(240_000);

    log('[mactest] booting with ROM and MacTest disk image');
    
    // Boot with ROM and MacTest disk - now with proper track reset on disk insertion
    await bootWithMedia(page, ROM_REL, MACTEST_ARCHIVE_REL);
  
    await runCommand(page, 'schedule max');

    log('[mactest] matching screen: MacTest booted');
    // Verify we see the MacTest disk booted
    await matchScreenFast(page, 'mactest-booted', {
      initialWaitMs: 5000,
      waitBeforeUpdateMs: 30_000,
      timeoutMs: 60_000
    });

    log('[mactest] deselecting unneeded tests');
    await mouseDrag(page, 100, 10, 100, 90); // deselect short ram test
    await mouseDrag(page, 100, 10, 100, 75); // deselect logic board 2 test
    await mouseDrag(page, 100, 10, 100, 125); // deselect serial loopback test
    await mouseDrag(page, 100, 10, 100, 140); // deselect scsi loopback test

    await runCommand(page, 'schedule hw');

    log('[mactest] start the MacTest test suite');
    await mouseClick(page, 340, 210);

    log('[mactest] wait for logic test screen match');
    await matchScreenFast(page, 'mactest-logic', { waitBeforeUpdateMs: 5_000 });

    log('[mactest] wait for floppy test screen match');
    await matchScreenFast(page, 'mactest-floppy', { waitBeforeUpdateMs: 20_000 });

    log('[mactest] wait for MacTest success screen match');
    await matchScreenFast(page, 'mactest-success', { waitBeforeUpdateMs: 120_000 });

    log('[mactest] MacTest application test complete');
  });
});
