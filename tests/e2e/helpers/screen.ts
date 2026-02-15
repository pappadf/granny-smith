// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { PNG } from 'pngjs';
import { test } from '@playwright/test';
import type { Page } from '@playwright/test';
import * as fs from 'fs';
import * as path from 'path';
import { runCommand } from './run-command';

export interface CaptureResult { full: Buffer; region: Buffer; }
export interface CaptureOptions { regionW: number; regionH: number; }
export interface MatchRegion { top: number; left: number; bottom: number; right: number; }
export interface MatchScreenOptions { forceUpdate?: boolean; }
export interface MatchScreenRunOptions extends MatchScreenOptions { region?: MatchRegion; timeoutMs?: number; pollMs?: number; waitBeforeUpdateMs?: number; initialWaitMs?: number; }

export async function captureProcessedRegion(page: Page, { regionW, regionH }: CaptureOptions): Promise<CaptureResult> {
	const screen = page.locator('#screen');
	const raw = await screen.screenshot();
	let workPng = PNG.sync.read(raw);
	const tlR = workPng.data[0];
	const tlG = workPng.data[1];
	const tlB = workPng.data[2];
	const isPureBW = ((tlR === 0 && tlG === 0 && tlB === 0) || (tlR === 255 && tlG === 255 && tlB === 255));
	if (!isPureBW) {
		if (workPng.width <= 1 || workPng.height <= 1) throw new Error('Image too small to crop');
		const cropped = new PNG({ width: workPng.width - 1, height: workPng.height - 1 });
		const bpp = 4;
		for (let y = 1; y < workPng.height; y++) {
			for (let x = 1; x < workPng.width; x++) {
				const srcIdx = (y * workPng.width + x) * bpp;
				const dstIdx = ((y - 1) * cropped.width + (x - 1)) * bpp;
				cropped.data[dstIdx] = workPng.data[srcIdx];
				cropped.data[dstIdx + 1] = workPng.data[srcIdx + 1];
				cropped.data[dstIdx + 2] = workPng.data[srcIdx + 2];
				cropped.data[dstIdx + 3] = workPng.data[srcIdx + 3];
			}
		}
		workPng = cropped;
	}
	if (workPng.width % 512 !== 0) throw new Error(`Width ${workPng.width} not multiple of 512`);
	if (workPng.height % 342 !== 0) throw new Error(`Height ${workPng.height} not multiple of 342`);
	const multX = workPng.width / 512;
	const multY = workPng.height / 342;
	const bpp = 4;
	const bw = new PNG({ width: 512, height: 342 });
	for (let y = 0; y < 342; y++) {
		const srcY = y * multY;
		for (let x = 0; x < 512; x++) {
			const srcX = x * multX;
			const srcIdx = (srcY * workPng.width + srcX) * bpp;
			const r = workPng.data[srcIdx];
			const g = workPng.data[srcIdx + 1];
			const b = workPng.data[srcIdx + 2];
			const lum = (r + g + b) / 3;
			const v = lum < 128 ? 0 : 255;
			const dstIdx = (y * 512 + x) * bpp;
			bw.data[dstIdx] = v;
			bw.data[dstIdx + 1] = v;
			bw.data[dstIdx + 2] = v;
			bw.data[dstIdx + 3] = 255;
		}
	}
	if (bw.width < regionW || bw.height < regionH) throw new Error('Processed screen smaller than region');
	const regionPng = new PNG({ width: regionW, height: regionH });
	for (let y = 0; y < regionH; y++) {
		const srcRow = y * bw.width * bpp;
		const dstRow = y * regionW * bpp;
		bw.data.copy(regionPng.data, dstRow, srcRow, srcRow + regionW * bpp);
	}
	return { full: PNG.sync.write(bw), region: PNG.sync.write(regionPng) };
}

async function captureProcessedFull(page: Page): Promise<Buffer> {
	const { full } = await captureProcessedRegion(page, { regionW: 512, regionH: 342 });
	return full;
}

/**
 * Capture the emulated screen using the browser-based method.
 * This maintains compatibility with existing baselines.
 */
export async function captureScreen(page: Page): Promise<Buffer> {
	return captureProcessedFull(page);
}

function cropRegion(fullPng: PNG, region: MatchRegion): PNG {
	const { top, left, bottom, right } = region;
	if (top < 0 || left < 0 || bottom <= top || right <= left) throw new Error('Invalid region coordinates');
	if (bottom > fullPng.height || right > fullPng.width) throw new Error('Region outside bounds');
	const width = right - left;
	const height = bottom - top;
	const out = new PNG({ width, height });
	const bpp = 4;
	for (let y = 0; y < height; y++) {
		const srcRow = (top + y) * fullPng.width * bpp + left * bpp;
		const dstRow = y * width * bpp;
		fullPng.data.copy(out.data, dstRow, srcRow, srcRow + width * bpp);
	}
	return out;
}

function decodePng(buf: Buffer): PNG { return PNG.sync.read(buf); }
function encodePng(png: PNG): Buffer { return PNG.sync.write(png); }

/**
 * Calculate a simple checksum of a PNG image, matching the emulator's
 * `screenshot --checksum` command. This allows fast screen comparison by polling
 * the checksum instead of transferring full PNG images.
 * 
 * The checksum is calculated on the packed 1-bit framebuffer representation:
 * - Each pixel is converted to 1-bit (black=1, white=0)
 * - Pixels are packed into bytes (MSB first, 8 pixels per byte)
 * - A simple polynomial hash (checksum = checksum * 31 + byte) is computed
 * 
 * @param png PNG buffer or decoded PNG object
 * @param region Optional region to checksum (default: full image)
 * @returns 32-bit checksum matching the emulator's calculation
 */
export function calculateScreenChecksum(png: Buffer | PNG, region?: MatchRegion): number {
	const img = Buffer.isBuffer(png) ? PNG.sync.read(png) : png;
	
	// Determine bounds (use region or full image)
	const top = region?.top ?? 0;
	const left = region?.left ?? 0;
	const bottom = region?.bottom ?? img.height;
	const right = region?.right ?? img.width;
	
	// Validate bounds
	if (top < 0 || left < 0 || bottom <= top || right <= left ||
		bottom > img.height || right > img.width) {
		throw new Error(`Invalid region bounds: (${top},${left})-(${bottom},${right}) for ${img.width}x${img.height} image`);
	}

	// Calculate checksum matching C algorithm for regions
	// Pack bits row by row, emitting bytes as we complete them or at row end
	let checksum = 0;
	
	for (let y = top; y < bottom; y++) {
		let accumByte = 0;
		for (let x = left; x < right; x++) {
			const pxIdx = (y * img.width + x) * 4;
			// Use red channel (R=G=B for grayscale)
			const isBlack = img.data[pxIdx] < 128;
			const relX = x - left;
			const byteBit = 7 - (relX % 8);
			
			if (byteBit === 7) accumByte = 0; // Start new byte
			if (isBlack) accumByte |= (1 << byteBit);
			
			// Emit byte when complete or at end of row
			if (byteBit === 0 || x === right - 1) {
				checksum = ((checksum * 31) + accumByte) >>> 0;
				accumByte = 0;
			}
		}
	}
	return checksum;
}

/**
 * Get the current screen checksum from the emulator.
 * This is much faster than capturing and transferring a full PNG.
 * Note: Emscripten ccall returns signed 32-bit, we convert to unsigned.
 * @param page Playwright page
 * @param region Optional region bounds (top, left, bottom, right)
 */
export async function getScreenChecksum(page: Page, region?: MatchRegion): Promise<number> {
	let cmd = 'screenshot --checksum';
	if (region) {
		cmd += ` ${region.top} ${region.left} ${region.bottom} ${region.right}`;
	}
	const result = await runCommand(page, cmd);
	// Convert signed int32 to unsigned uint32
	return result >>> 0;
}

/**
 * Fast screen matching using checksums for polling.
 * 
 * This is significantly faster than the old matchScreen() because:
 * - No PNG encoding/decoding on each poll
 * - No image data transfer (just a 32-bit number)
 * - Checksum calculation is O(n) on raw framebuffer bytes
 * 
 * Supports both full-screen and region matching via the C-side region checksum.
 * Only captures the full PNG image at the end for attachments/debugging.
 */
export async function matchScreenFast(page: Page, name: string, runOpts: MatchScreenRunOptions = {}): Promise<{ matched: boolean; attempts: number; baselineUpdated: boolean; }> {
	const { region = { top: 0, left: 0, bottom: 342, right: 512 }, timeoutMs = 120_000, pollMs = 1000, waitBeforeUpdateMs = 60_000, initialWaitMs = 0, forceUpdate: forceUpdateFlag = false } = runOpts;
	const testInfo = test.info();
	const isFullScreen = region.top === 0 && region.left === 0 && region.bottom === 342 && region.right === 512;

	if (name.includes('/') || name.includes(path.sep)) throw new Error(`name must be a bare filename stem (no path separators): ${name}`);
	const baselineName = name.endsWith('.png') ? name : `${name}.png`;
	const testDir = path.dirname(testInfo.file);
	const baselinePath = path.join(testDir, baselineName);
	
	const updateEnv = process.env.UPDATE_SNAPSHOTS === '1';
	const forceUpdate = forceUpdateFlag === true;
	const updating = forceUpdate || updateEnv;
	const attachPrefix = name.replace(/\.png$/i, '');

	if (updating) {
		// For updates, capture and save baseline (region or full)
		if (waitBeforeUpdateMs > 0) await page.waitForTimeout(waitBeforeUpdateMs);
		const lastFull = await captureScreen(page);
		fs.mkdirSync(path.dirname(baselinePath), { recursive: true });
		if (isFullScreen) {
			fs.writeFileSync(baselinePath, lastFull);
		} else {
			// Save cropped region as baseline
			const fullPng = decodePng(lastFull);
			const regionPng = cropRegion(fullPng, region);
			fs.writeFileSync(baselinePath, encodePng(regionPng));
		}
		await testInfo.attach(`${attachPrefix}-baseline-full`, { body: lastFull, contentType: 'image/png' });
		fs.writeFileSync(testInfo.outputPath(`${attachPrefix}-full.png`), lastFull);
		return { matched: true, attempts: 1, baselineUpdated: true };
	}

	if (!fs.existsSync(baselinePath)) {
		throw new Error(`Baseline image missing: ${baselinePath} (run with UPDATE_SNAPSHOTS=1 to create)`);
	}

	// Calculate expected checksum from baseline
	// For regions, baseline is already cropped, so calculate checksum on full image
	const baseline = fs.readFileSync(baselinePath);
	const baselinePng = decodePng(baseline);
	// If baseline dimensions match region size, it's a region baseline
	const regionWidth = region.right - region.left;
	const regionHeight = region.bottom - region.top;
	const isRegionBaseline = baselinePng.width === regionWidth && baselinePng.height === regionHeight;
	// Calculate checksum on the baseline (without region offset since it's already cropped)
	const expectedChecksum = isRegionBaseline
		? calculateScreenChecksum(baselinePng)  // Full baseline image checksum
		: calculateScreenChecksum(baselinePng, region);  // Full screen baseline with region

	if (initialWaitMs > 0) await page.waitForTimeout(initialWaitMs);

	// Poll using fast checksum comparison
	const start = Date.now();
	let attempts = 0;
	let matched = false;
	
	while (true) {
		attempts++;
		// For regions, query C-side with region bounds
		const currentChecksum = isFullScreen
			? await getScreenChecksum(page)
			: await getScreenChecksum(page, region);
		if (currentChecksum === expectedChecksum) {
			matched = true;
			break;
		}
		if (Date.now() - start > timeoutMs) break;
		await page.waitForTimeout(pollMs);
	}

	// Capture final screen for attachments (only once at the end)
	const lastFull = await captureScreen(page);
	await testInfo.attach(`${attachPrefix}-full`, { body: lastFull, contentType: 'image/png' });
	fs.writeFileSync(testInfo.outputPath(`${attachPrefix}-full.png`), lastFull);

	if (!matched) {
		const finalChecksum = isFullScreen
			? await getScreenChecksum(page)
			: await getScreenChecksum(page, region);
		throw new Error(`Screen checksum did not match baseline within ${timeoutMs}ms (attempts=${attempts}, expected=0x${expectedChecksum.toString(16)}, got=0x${finalChecksum.toString(16)})`);
	}
	return { matched: true, attempts, baselineUpdated: false };
}
