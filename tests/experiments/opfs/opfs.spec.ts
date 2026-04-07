// OPFS Experiment Playwright Specs
// Run: npx playwright test tests/experiments/opfs/opfs.spec.ts --config=tests/experiments/opfs/playwright.config.ts

import { test, expect } from '@playwright/test';

// Helper: wait for WASM module to be ready
async function waitForReady(page) {
  await page.waitForFunction(() => window['__opfsReady'] === true, null, { timeout: 30_000 });
}

// Helper: collect console output
function collectOutput(page): string[] {
  const lines: string[] = [];
  page.on('console', msg => lines.push(msg.text()));
  return lines;
}

// Helper: run a C-side experiment and check for PASS
async function runCExperiment(page, name: string) {
  const rc = await page.evaluate(async (n) => {
    return await window['__opfs'].runExperiment(n);
  }, name);
  return rc;
}

// ============================================================================
// Experiment 1: OPFS root filesystem + /tmp memory backend
// ============================================================================
test('exp1: OPFS root filesystem setup — C POSIX I/O', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  const rc = await runCExperiment(page, 'filesystem_setup');
  const log = output.join('\n');
  console.log(log);

  expect(rc).toBe(0);
  expect(log).toContain('[exp1] PASS');
});

// ============================================================================
// Experiment 2: JS FS.* API on OPFS root
// ============================================================================
test('exp2: JS FS.* API on OPFS-backed paths', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  const results = await page.evaluate(async () => {
    return await window['__opfs'].testJsFsApi();
  });

  console.log('JS FS API results:', results);

  expect(results.mkdir).toBe('ok');
  expect(results.writeFile).toBe('ok');
  expect(results.readFile).toBe('ok');
  expect(results.stat).toContain('ok');
  expect(results.readdir).toBe('ok');
  expect(results.unlink).toBe('ok');
  expect(results.rmdir).toBe('ok');

  // isFile/isDir may not be available in WasmFS — log but don't fail
  if (results.isFile === 'NOT_AVAILABLE') {
    console.log('NOTE: FS.isFile() not available in WasmFS');
  } else {
    expect(results.isFile).toBe('ok');
  }
  if (results.isDir === 'NOT_AVAILABLE') {
    console.log('NOTE: FS.isDir() not available in WasmFS');
  } else {
    expect(results.isDir).toBe('ok');
  }
});

// ============================================================================
// Experiment 3: ccall without { async: true }
// ============================================================================
test('exp3: ccall with OPFS I/O (sync and async)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  // Test sync ccall (may fail with mounted OPFS — that's an expected finding)
  const syncResult = await page.evaluate(() => {
    return window['__opfs'].testSyncCcall();
  });
  console.log('Sync ccall result:', syncResult);

  // Test async ccall (should always work)
  const asyncResult = await page.evaluate(async () => {
    return await window['__opfs'].testAsyncCcall();
  });
  console.log('Async ccall result:', asyncResult);

  // Async ccall must work
  expect(asyncResult.ok).toBe(true);
  expect(asyncResult.rc).toBe(0);

  // Log whether sync works (informational — don't fail the test)
  if (syncResult.ok) {
    console.log('NOTE: sync ccall works with OPFS — async mutex can be dropped');
  } else {
    console.log('NOTE: sync ccall fails with OPFS — { async: true } required');
    console.log('  Error:', syncResult.error);
  }
});

// ============================================================================
// Experiment 4: fseek/fwrite granularity (512-byte blocks)
// ============================================================================
test('exp4: fseek/fwrite at 512-byte granularity', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  const rc = await runCExperiment(page, 'seek_write');
  const log = output.join('\n');
  console.log(log);

  expect(rc).toBe(0);
  expect(log).toContain('[exp2] PASS');
});

// ============================================================================
// Experiment 5: Open-handle vs reopen performance
// ============================================================================
test('exp5: open-handle vs reopen-each performance', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  const rc = await runCExperiment(page, 'handle_performance');
  const log = output.join('\n');
  console.log(log);

  expect(rc).toBe(0);
  expect(log).toContain('[exp3] PASS');

  // Extract timing from output
  const openMatch = log.match(/open-handle:\s+([\d.]+)\s+ms/);
  const reopenMatch = log.match(/reopen-each:\s+([\d.]+)\s+ms/);
  const ratioMatch = log.match(/ratio:\s+([\d.]+)x/);
  if (openMatch && reopenMatch && ratioMatch) {
    console.log(`Performance: open-handle=${openMatch[1]}ms, reopen=${reopenMatch[1]}ms, ratio=${ratioMatch[1]}x`);
    // We expect reopen to be significantly slower (>2x)
    const ratio = parseFloat(ratioMatch[1]);
    expect(ratio).toBeGreaterThan(1.0);
  }
});

// ============================================================================
// Experiment 6: Sparse file behavior
// ============================================================================
test('exp6: sparse file behavior', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  const rc = await runCExperiment(page, 'sparse_file');
  const log = output.join('\n');
  console.log(log);

  expect(rc).toBe(0);
  expect(log).toContain('[exp4] PASS');
  expect(log).toContain('gap reads as zeros: yes');

  // Extract size info for the report
  const sizeMatch = log.match(/logical size:\s+(\d+)/);
  const blocksMatch = log.match(/st_blocks:\s+(\d+)/);
  if (sizeMatch) console.log(`Sparse file logical size: ${sizeMatch[1]} bytes`);
  if (blocksMatch) console.log(`Sparse file st_blocks: ${blocksMatch[1]}`);
});

// ============================================================================
// Experiment 7: Persistence across page reload
// ============================================================================
test('exp7: persistence survives page reload', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  // Phase 1: Write marker
  const rc1 = await runCExperiment(page, 'persistence_write');
  console.log('WRITE PHASE:', output.join('\n'));
  expect(rc1).toBe(0);

  // Verify marker exists via C (non-destructive check, no cleanup!)
  const verifyRc = await runCExperiment(page, 'persistence_check');
  console.log('PRE-RELOAD VERIFY:', verifyRc === 0 ? 'OK' : 'FAIL');
  expect(verifyRc).toBe(0);

  // Check via OPFS JS API directly (before reload, before any cleanup)
  const opfsCheck = await page.evaluate(async () => {
    try {
      const root = await navigator.storage.getDirectory();
      const entries = [];
      for await (const [name] of root) entries.push(name);
      return { rootEntries: entries };
    } catch (e) {
      return { error: e.message };
    }
  });
  console.log('OPFS root entries (before reload):', JSON.stringify(opfsCheck));

  // Reload the page (fresh WASM instance)
  await page.reload();
  const output2: string[] = [];
  page.on('console', msg => output2.push(msg.text()));
  await waitForReady(page);

  // Phase 2: Read marker back (non-destructive)
  const rc2 = await runCExperiment(page, 'persistence_check');
  const log = output2.join('\n');
  console.log('READ AFTER RELOAD:', log);

  // Clean up
  await runCExperiment(page, 'persistence_cleanup');

  expect(rc2).toBe(0);
  expect(log).toContain('[exp5b] PASS');
});

// ============================================================================
// Experiment 8: emscripten_set_main_loop with file I/O
// ============================================================================
test('exp8: main_loop with OPFS file I/O', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/');
  await waitForReady(page);

  // This experiment runs asynchronously via the main loop
  await page.evaluate(async () => {
    window['__opfs'].runExperiment('main_loop_io');
  });

  // Wait for the experiment to complete (it prints PASS/FAIL after 5 ticks)
  await page.waitForFunction(() => {
    const log = document.getElementById('log')?.textContent || '';
    return log.includes('[exp6] PASS') || log.includes('[exp6] FAIL');
  }, null, { timeout: 10_000 });

  const log = output.join('\n');
  console.log(log);
  expect(log).toContain('[exp6] PASS');
});
