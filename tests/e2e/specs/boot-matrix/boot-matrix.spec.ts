// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test } from '../../fixtures';
import * as fs from 'fs';
import * as path from 'path';
import { matchScreenFast } from '../../helpers/screen';
import { bootWithMedia } from '../../helpers/boot';

interface ManifestEntry { id: string; file: string; size?: number; }
interface Manifest { roms: ManifestEntry[]; systems: ManifestEntry[]; }

const TESTS_DIR = path.join(process.cwd(), 'tests');
// Shared media root for all test suites
const MEDIA_ROOT = path.join(TESTS_DIR, 'data');

// ROM and system disk configurations for boot matrix testing
const manifest: Manifest = {
	roms: [
		{ id: "plus_v3", file: "roms/Plus_v3.rom", size: 131072 }
	],
	systems: [
		{ id: "sys_201", file: "systems/System_2_0_1.dsk" },
		{ id: "sys_204", file: "systems/System_2_0_4.dsk" },
		{ id: "sys_300", file: "systems/System_3_0_0.dsk" },
		{ id: "sys_320", file: "systems/System_3_2_0.dsk" },
		{ id: "sys_330", file: "systems/System_3_3_0.dsk" },
		{ id: "sys_400", file: "systems/System_4_0_0.dsk" },
		{ id: "sys_410", file: "systems/System_4_1_0.dsk" },
		{ id: "sys_420", file: "systems/System_4_2_0.dsk" },
		{ id: "sys_430", file: "systems/System_4_3_0.dsk" },
		{ id: "sys_600", file: "systems/System_6_0_0.dsk" },
		{ id: "sys_603", file: "systems/System_6_0_3.dsk" },
		{ id: "sys_605", file: "systems/System_6_0_5.dsk" },
		{ id: "sys_608", file: "systems/System_6_0_8.dsk" },
		{ id: "sys_710", file: "systems/System_7_1_0.dsk" }
	]
};

// Shared constants
const REGION_W = 32;
const REGION_H = 12;
const WATCHDOG_MS = parseInt(process.env.CAPTURE_WATCHDOG_MS || '120000', 10);

for (const rom of manifest.roms) {
	for (const sys of manifest.systems) {
		test.describe(`${rom.id} + ${sys.id}`, () => {
			test(`boots to Apple menu signature`, async ({ page }) => {
				test.setTimeout(180_000);
				// Media presence (single canonical root)
				const resolveMedia = (p: string) => path.join(MEDIA_ROOT, p.replace(/^media\//, '').replace(/^\/+/, ''));
				const romPath = resolveMedia(rom.file);
				const sysPath = resolveMedia(sys.file);
				if (!fs.existsSync(romPath) || !fs.existsSync(sysPath)) {
					test.skip(true, `Missing media asset(s): ${rom.file} / ${sys.file}`);
				}
				// Boot via URL parameters (ROM + floppy as fd0)
				const romRel = rom.file.replace(/^media\//, '').replace(/^\/+/, '');
				const sysRel = sys.file.replace(/^media\//, '').replace(/^\/+/, '');
				// Start emulator at maximum speed for faster boot during matrix tests.
				await bootWithMedia(page, romRel, sysRel, undefined, 'max');
				// Wait for overlay dismissal (ROM loaded + disk inserted triggers boot)
				await page.waitForFunction(() => {
					const el = document.getElementById('rom-required-overlay');
					return !el || !el.classList.contains('visible');
				}, { timeout: 20000 });
				// Use shared-region baseline across all ROM/system combos.
				await matchScreenFast(page, 'shared-region', {
					region: { top: 0, left: 0, bottom: REGION_H, right: REGION_W },
					pollMs: 500,
				});
			});
		});
	}
}
