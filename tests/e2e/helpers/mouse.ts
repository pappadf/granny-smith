// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import type { Page } from '@playwright/test';

async function waitForPrompt(page: Page, timeoutMs = 5000) {
	await page.waitForFunction(() => {
		try {
			const snap = (window as any).__gsTest?.getTerminalSnapshot(400) || '';
			const lines = snap.split('\n').map((l: string) => l.trim()).filter(Boolean);
			const last = lines.length ? lines[lines.length - 1] : '';
			return />\s*$/.test(last);
		} catch { return false; }
	}, { timeout: timeoutMs });
}

async function ensureRunCommand(page: Page) { await page.waitForFunction(() => typeof (window as any).runCommand === 'function'); }

async function prepareMousePosition(page: Page, x: number, y: number) {
	await ensureRunCommand(page);
	await page.evaluate((cmd) => (window as any).runCommand(cmd), `set-mouse ${x} ${y}`);
}

export async function mouseClick(page: Page, x: number, y: number) {
	await prepareMousePosition(page, x, y);
	await mouseDown(page);
	await page.waitForTimeout(200);
	await mouseUp(page);
}

export async function mouseDoubleClick(page: Page, x: number, y: number) {
	await prepareMousePosition(page, x, y);
	await mouseDown(page); await page.waitForTimeout(100); await mouseUp(page);
	await page.waitForTimeout(100);
	await mouseDown(page); await page.waitForTimeout(100); await mouseUp(page);
}

// Drag from start (x1, y1) to end (x2, y2): mouse down, move, mouse up.
export async function mouseDrag(page: Page, x1: number, y1: number, x2: number, y2: number) {
	// Position at start, press, move to end via set-mouse, then release.
	await prepareMousePosition(page, x1, y1);
	await page.waitForTimeout(500);
	await mouseDown(page);
	await page.waitForTimeout(500);
	await prepareMousePosition(page, x2, y2);
	await page.waitForTimeout(500);
	await mouseUp(page);
	await page.waitForTimeout(500);
}

async function mouseDown(page: Page) {
	await ensureRunCommand(page);
	await page.evaluate(() => (window as any).runCommand('mouse-button down'));
}
async function mouseUp(page: Page) {
	await ensureRunCommand(page);
	await page.evaluate(() => (window as any).runCommand('mouse-button up'));
}

