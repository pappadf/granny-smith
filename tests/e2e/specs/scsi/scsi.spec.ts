// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import * as fs from 'fs';
import { test } from '../../fixtures';
import { matchScreenFast } from '../../helpers/screen';
// xterm capture now automatic via fixtures
import { bootWithMedia } from '../../helpers/boot';
import { mouseClick, mouseDoubleClick } from '../../helpers/mouse';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const HD_REL = 'systems/hd1.zip';
const FD_REL = 'systems/System_6_0_5.dsk';

test.describe('SCSI Test 1: Boot from hd0', () => {
	test('boots and matches full-screen baseline', async ({ page, log }) => {
		test.setTimeout(120_000);
		log('[test] start');
		await bootWithMedia(page, ROM_REL, undefined, HD_REL);
		log('[test] bootWithMedia complete');
		try {
			await matchScreenFast(page, 'baseline-test-1A');
		} finally {
			try { await page.goto('about:blank'); } catch { }
		}
	});

	test('SCSI Test 2: Boot from fd0, test and reformat hd0', async ({ page, log }) => {
		test.setTimeout(480_000); // extended for long waits
		log('[test] start');
		await bootWithMedia(page, ROM_REL, FD_REL, HD_REL);
		try {
			await matchScreenFast(page, 'baseline-test-2A');
			await mouseDoubleClick(page, 189, 128);
			await page.waitForTimeout(20_000);
			await mouseClick(page, 180, 194);
			await page.waitForTimeout(10_000);
			await mouseClick(page, 397, 158);
			await page.waitForTimeout(60_000);
			await matchScreenFast(page, 'baseline-test-2B');
			await mouseClick(page, 180, 120);
			await page.waitForTimeout(10_000);
			await mouseClick(page, 358, 160);
			await page.waitForTimeout(60_000);
			await matchScreenFast(page, 'baseline-test-2C');
			await mouseClick(page, 183, 224);
			await page.waitForTimeout(20_000);
			await matchScreenFast(page, 'baseline-test-2D');
		} finally {
			try { await page.goto('about:blank'); } catch { }
		}
	});
});
