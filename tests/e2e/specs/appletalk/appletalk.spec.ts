// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test } from '../../fixtures';
import { matchScreenFast } from '../../helpers/screen';
import { bootWithMedia } from '../../helpers/boot';
import { mouseClick, mouseDoubleClick, mouseDrag } from '../../helpers/mouse';
import { runCommand } from '../../helpers/run-command';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const HD_REL = 'systems/hd1.zip';

// Helper options
const SHORT_MATCH = { initialWaitMs: 2000, timeoutMs: 10_000 } as const;
const LONG_MATCH = { initialWaitMs: 2000, timeoutMs: 180_000 } as const; // up to 3 minutes

// NOTE: This suite is initially a copy of the floppy test. We'll adjust scenarios later.
// To ensure baselines exist without copying PNGs, we set forceUpdate on matchScreenFast calls.
test.describe('Appletalk', () => {
  test('Mounting shared volume over AFP', async ({ page, log }) => {
    test.setTimeout(600_000);
    log('[appletalk] booting from HD (systems/hd1.zip)');
    // Boot from hard disk (similar to scsi suite): no floppy, pass HD as 4th arg
    await bootWithMedia(page, ROM_REL, undefined, HD_REL);

    // Wait for a fully booted desktop
  await matchScreenFast(page, 'appletalk-1-booted', { initialWaitMs: 2000, waitBeforeUpdateMs: 60_000, timeoutMs: 60_000 });

  // Create a new shared folder/volume via command.
  await runCommand(page, 'atalk-share-add "My Volume" /tmp');

  // Select Chooser from Apple menu
  await mouseDrag(page, 25, 10, 25, 110);

  // Wait for chooser top open
  await matchScreenFast(page, 'appletalk-2-chooser', { initialWaitMs: 2000, waitBeforeUpdateMs: 20_000, timeoutMs: 20_000 });

  // Select AppleShare
  await mouseClick(page, 100, 85);

  // Wait for dialogue ("AppleShare requires AppleTalk...")
  await matchScreenFast(page, 'appletalk-3-dialogue', { initialWaitMs: 2000, waitBeforeUpdateMs: 10_000, timeoutMs: 10_000 });

  // Click "OK"
  await mouseClick(page, 185, 205);

  // Wait for Chooser ("Select a file server...")
  await matchScreenFast(page, 'appletalk-4-select-server', { initialWaitMs: 2000, waitBeforeUpdateMs: 10_000, timeoutMs: 10_000 });

  // Click "Shared Folders"
  await mouseClick(page, 300, 85);

  // Wait a bit
  await page.waitForTimeout(1_000);

  // Click "OK"
  await mouseClick(page, 330, 180);

  // Wait for "Connect to the file server..." dialogue
  await matchScreenFast(page, 'appletalk-5-connect-to-server', { initialWaitMs: 2000, waitBeforeUpdateMs: 10_000, timeoutMs: 10_000 });

  // Click "OK"
  await mouseClick(page, 390, 260);

  // Wait for "Select server volumes..." dialogue
  await matchScreenFast(page, 'appletalk-6-select-server-volumes', { initialWaitMs: 2000, waitBeforeUpdateMs: 10_000, timeoutMs: 10_000 });

  // Select volume
  await mouseClick(page, 140, 110);

  // Wait a bit
  await page.waitForTimeout(1_000);

  // Click "OK"
  await mouseClick(page, 330, 260);

  // Wait for volume to be mounted
  await matchScreenFast(page, 'appletalk-7-mounted', { pollMs: 1000, initialWaitMs: 2000, waitBeforeUpdateMs: 10_000, timeoutMs: 10_000 });

 
  });
});
