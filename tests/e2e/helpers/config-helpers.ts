// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import type { Page } from '@playwright/test';
import * as fs from 'fs';
import * as path from 'path';

/** Simulate dropping a single file (ROM/floppy) onto #screen canvas. */
export async function dragDropFile(page: Page, absPath: string, fileName?: string) {
	const bytes = fs.readFileSync(absPath);
	const name = fileName || path.basename(absPath);
	await page.evaluate(({ bytes, name }) => {
		const canvas = document.getElementById('screen');
		const dt = new DataTransfer();
		const blob = new Blob([new Uint8Array(bytes)], { type: 'application/octet-stream' });
		const file = new File([blob], name);
		dt.items.add(file);
		const ev = new DragEvent('drop', { dataTransfer: dt, bubbles: true });
		canvas?.dispatchEvent(ev);
	}, { bytes: Array.from(bytes), name });
}
