// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';
import { matchScreenFast } from '../../helpers/screen';
// xterm capture now automatic via fixtures
import { bootWithMedia } from '../../helpers/boot';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';
const SYS_REL = 'systems/System_4_1_0.dsk';

// Baseline full-screen image (processed 512x342 1-bit) in same directory.
const BASELINE_FULL = 'baseline-full';
const WATCHDOG_MS = 60_000; // Fail if no successful boot match within 60s

test.describe('Configuration: URL param boot (rom + fd0)', () => {
	test('boots and matches full-screen baseline', async ({ page, log }) => {
		test.setTimeout(120_000);
		log('[test] start');
		await bootWithMedia(page, ROM_REL, SYS_REL);
		try {
			await matchScreenFast(page, BASELINE_FULL, { timeoutMs: WATCHDOG_MS });
		} finally {
			try { await page.goto('about:blank'); } catch { }
		}
	});
});
