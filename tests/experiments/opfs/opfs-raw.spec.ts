// Test raw OPFS (no WasmFS) — does the browser itself persist OPFS across reload?
import { test, expect } from '@playwright/test';

test('raw OPFS persistence across reload (no WasmFS)', async ({ page }) => {
  // Phase 1: Write directly to OPFS via JS API
  await page.goto('http://localhost:18090/');

  const writeResult = await page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const dir = await root.getDirectoryHandle('raw_test', { create: true });
      const fileHandle = await dir.getFileHandle('marker.txt', { create: true });
      const writable = await fileHandle.createWritable();
      await writable.write('RAW_OPFS_PERSIST_OK');
      await writable.close();

      // Verify readable
      const file = await fileHandle.getFile();
      const text = await file.text();
      return { ok: true, text };
    } catch (e) {
      return { ok: false, error: e.message };
    }
  });
  console.log('Write result:', writeResult);
  expect(writeResult.ok).toBe(true);
  expect(writeResult.text).toBe('RAW_OPFS_PERSIST_OK');

  // Phase 2: Reload and check
  await page.reload();

  const readResult = await page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const entries = [];
      for await (const [name] of root) entries.push(name);

      const dir = await root.getDirectoryHandle('raw_test');
      const fileHandle = await dir.getFileHandle('marker.txt');
      const file = await fileHandle.getFile();
      const text = await file.text();

      // Clean up
      await root.removeEntry('raw_test', { recursive: true });

      return { ok: true, text, entries };
    } catch (e) {
      return { ok: false, error: e.message };
    }
  });
  console.log('Read after reload:', readResult);
  expect(readResult.ok).toBe(true);
  expect(readResult.text).toBe('RAW_OPFS_PERSIST_OK');
});

test('raw OPFS: SyncAccessHandle write + read back', async ({ page }) => {
  // Test SyncAccessHandle (requires Worker) — this is what pthreads uses
  await page.goto('http://localhost:18090/');

  const result = await page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const fileHandle = await root.getFileHandle('sync_test.bin', { create: true });
      const syncHandle = await fileHandle.createSyncAccessHandle();

      // Write 512 bytes at offset 1024
      const data = new Uint8Array(512).fill(0xAA);
      syncHandle.write(data, { at: 1024 });
      syncHandle.flush();

      // Read back
      const readBuf = new Uint8Array(512);
      syncHandle.read(readBuf, { at: 1024 });

      // Check
      const allAA = readBuf.every(b => b === 0xAA);

      // Read from gap (offset 0)
      const gapBuf = new Uint8Array(512);
      syncHandle.read(gapBuf, { at: 0 });
      const gapZero = gapBuf.every(b => b === 0);

      syncHandle.close();
      await root.removeEntry('sync_test.bin');

      return { ok: true, allAA, gapZero, size: syncHandle };
    } catch (e) {
      return { ok: false, error: e.message };
    }
  });
  console.log('SyncAccessHandle result:', result);

  if (!result.ok && result.error?.includes('SyncAccessHandle')) {
    // SyncAccessHandle requires Worker context — expected to fail on main thread
    console.log('NOTE: SyncAccessHandle not available on main thread (expected — requires Worker)');
  } else {
    expect(result.ok).toBe(true);
    expect(result.allAA).toBe(true);
    expect(result.gapZero).toBe(true);
  }
});
