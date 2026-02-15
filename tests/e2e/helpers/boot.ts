// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// moved from tests/utils/boot.ts unchanged
import type { Page } from '@playwright/test';
import * as fs from 'fs';
import * as path from 'path';
import JSZip = require('jszip');
// Media root path under tests/data
const TEST_MEDIA_ROOT = path.join(process.cwd(), 'tests', 'data');
const TEST_MEDIA_BASE_URL = 'http://localhost:18080/tests-media';
interface MediaEntry { rel: string; abs: string; baseName: string; }
function resolveRequired(relPath: string, label: string): MediaEntry { const rel = relPath.replace(/^\/+/,''); const abs = path.isAbsolute(rel) ? rel : path.join(TEST_MEDIA_ROOT, rel); if(!fs.existsSync(abs)) throw new Error(`Missing ${label} media: ${abs}`); return { rel: path.relative(TEST_MEDIA_ROOT, abs), abs, baseName: path.basename(abs)}; }
function resolveOptional(relPath: string | undefined, label: string): MediaEntry | undefined { if(!relPath) return; return resolveRequired(relPath, label);} 
export async function bootWithMedia(page: Page, romRel: string, fd0Rel?: string, hd0Rel?: string, speed?: 'max' | 'realtime' | 'hardware') {
	const rom = resolveRequired(romRel,'ROM');
	const fd0 = resolveOptional(fd0Rel,'fd0');
	const hd0 = resolveOptional(hd0Rel,'hd0');
	const mapping: Record<string, MediaEntry> = { [rom.baseName]: rom };
	if(fd0) mapping[fd0.baseName]=fd0;
	if(hd0) mapping[hd0.baseName]=hd0;
	await page.route('**/tests-media/**', async route => {
		const url = new URL(route.request().url());
		const file = url.pathname.replace(/.*\/tests-media\//,'');
		const entry = mapping[file];
		if(!entry) return route.fulfill({ status:404, body:'missing'});
		const body = fs.readFileSync(path.join(TEST_MEDIA_ROOT, entry.rel));
		await route.fulfill({ status:200, headers:{'Content-Type':'application/octet-stream','Cache-Control':'no-store'}, body });
	});
	const params: string[] = [];
	params.push(`rom=${encodeURIComponent(`${TEST_MEDIA_BASE_URL}/${rom.baseName}`)}`);
	if(fd0) params.push(`fd0=${encodeURIComponent(`${TEST_MEDIA_BASE_URL}/${fd0.baseName}`)}`);
	if(hd0) params.push(`hd0=${encodeURIComponent(`${TEST_MEDIA_BASE_URL}/${hd0.baseName}`)}`);
	if(speed) params.push(`speed=${encodeURIComponent(speed)}`);
	// Prefer relative navigation; Playwright baseURL (playwright.config.ts) provides the origin.
	const rel = `/index.html?${params.join('&')}`;
	await page.goto(rel);
	
	// Wait for ROM loading and initial run to complete before returning.
	// The page auto-downloads the ROM, loads it, and runs. We need to wait for:
	// 1. Test shim to be ready (provides getTerminalSnapshot)
	// 2. ROM to be downloaded and loaded
	// 3. Initial 'run' command to execute
	
	// First, wait for the test shim to be ready
	await page.waitForFunction(() => {
		return typeof (window as any).__gsTestShim?.getTerminalSnapshot === 'function';
	}, { timeout: 10000 });
	
	// Then wait for ROM loading to complete (checks command log for load-rom)
	await page.waitForFunction(() => {
		try {
			const commandLog = (window as any).__commandLog || [];
			return commandLog.some((cmd: string) => cmd.includes('load-rom'));
		} catch (e) {
			return false;
		}
	}, { timeout: 15000 });
} 

/**
 * Prepare boot by uploading media directly into the in-page filesystem (via the
 * Playwright-installed test shim) and issuing only the non-running boot commands.
 *
 * Differences from bootWithMedia:
 *  - uploads ROM/FD/HD bytes into /tmp inside the page (no URL params, no fetch)
 *  - issues load-rom / attach-hd / insert-fd commands but DOES NOT run
 *  - leaves starting the emulator (sending 'run') to the caller/test
 */
export async function bootWithUploadedMedia(
	page: Page,
	romRel: string,
	fd0Rel?: string,
	hd0ZipRel?: string,
	options?: { hdSlot?: number; navigatePath?: string; hideOverlay?: boolean }
) {
	const hdSlot = options?.hdSlot ?? 0;
	const navigatePath = options?.navigatePath ?? '/index.html';
	const hideOverlay = options?.hideOverlay ?? true;

	// Resolve required/optional media on disk
	const rom = resolveRequired(romRel, 'ROM');
	const fd0 = resolveOptional(fd0Rel, 'fd0');
	const hdZip = resolveOptional(hd0ZipRel, 'HD zip');

	// Navigate to the app (no media URL params).
	await page.goto(navigatePath);

	// Wait for shim and command bridge. injectMedia now uses drop events (no __Module.FS needed).
	await page.waitForFunction(() => {
		const shim = (window as any).__gsTestShim;
		const hasInject = typeof shim?.injectMedia === 'function';
		const hasRunCommand = typeof (window as any).runCommand === 'function';
		return hasInject && hasRunCommand;
	}, { timeout: 60000 });

	if (options?.wipeCheckpoints !== false) {
		await page.evaluate(async () => {
			const shim = (window as any).__gsTestShim;
			const clear = shim && typeof shim.clearCheckpoints === 'function' ? shim.clearCheckpoints : null;
			if (clear) await clear();
		});
	}

	// Build uploads: ROM always; optional FD0; HD is extracted from zip to its inner image
	const uploads: { name: string; data: Uint8Array }[] = [];
	uploads.push({ name: 'rom', data: readFileBytes(rom.abs) });
	if (fd0) uploads.push({ name: 'fd0', data: readFileBytes(fd0.abs) });
	if (hdZip) {
		const hd = await extractFirstFileFromZip(hdZip.abs);
		uploads.push({ name: `hd${hdSlot}`, data: hd.data });
	}

	// Inject bytes into in-page FS under /tmp
	const injected = await page.evaluate((files) => {
		const prepared = files.map((f: { name: string; data: any }) => {
			const bytes = f.data instanceof Uint8Array ? f.data : new Uint8Array(f.data);
			return { name: f.name, data: bytes };
		});
		return (window as any).__gsTestShim.injectMedia(prepared);
	}, uploads);
	if (!injected) throw new Error('injectMedia reported failure');

	// Optionally hide ROM-required overlay to keep canvas unobscured for snapshots
	if (hideOverlay) {
		await page.evaluate(() => {
			try { if (typeof (window as any).hideRomOverlay === 'function') (window as any).hideRomOverlay(); } catch (_) {}
			try {
				const el = document.getElementById('rom-required-overlay');
				if (el) {
					try { el.classList.remove('visible'); } catch(_){}
					try { (el as HTMLElement).style.display = 'none'; } catch(_){}
					try { if (el.parentNode) el.parentNode.removeChild(el); } catch(_){}
				}
			} catch (_) {}
		});
		await page.waitForTimeout(50); // small settle
	}

	// Wait for full page boot sequence to complete (FS init, checkpoint
	// polling, media-persist wiring).  Without this, the checkpoint-poll's
	// FS.syncfs(true) can race with newly-created .blocks/ directories
	// under /persist/images/ and delete them.
	await page.waitForFunction(() => {
		return (window as any).__gsBootReady === true;
	}, { timeout: 60000 });

	// Issue boot-time commands but DO NOT run
	await page.evaluate(({ hasFd, hasHd, hdSlot }) => {
		const send = (window as any).runCommand;
		send('load-rom /tmp/rom');
		if (hasFd) send('insert-fd /tmp/fd0');
		if (hasHd) send(`attach-hd /tmp/hd${hdSlot} ${hdSlot}`);
	}, { hasFd: Boolean(fd0), hasHd: Boolean(hdZip), hdSlot });
}

interface UploadEntry { name: string; data: Uint8Array; }

function readFileBytes(absPath: string): Uint8Array {
	return new Uint8Array(fs.readFileSync(absPath));
}

async function extractFirstFileFromZip(absPath: string): Promise<UploadEntry> {
	const zip = await JSZip.loadAsync(fs.readFileSync(absPath));
	const files = zip.filter((_path, entry) => !entry.dir);
	if (!files.length) throw new Error(`Zip archive ${absPath} contains no files`);
	if (files.length > 1) {
		console.warn(`[boot] zip ${absPath} contains multiple entries; using ${files[0].name}`);
	}
	const primary = files[0];
	const data = await primary.async('uint8array');
	return { name: path.basename(primary.name), data };
}

// bootWithUploadedMedia folded into individual specs when needed.

export { TEST_MEDIA_ROOT, TEST_MEDIA_BASE_URL };
