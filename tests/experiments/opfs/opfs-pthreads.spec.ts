// OPFS Pthreads Experiments — run via ?run=experiment_name URL parameter
// Experiments execute in main() on the worker thread (PROXY_TO_PTHREAD)
import { test, expect } from '@playwright/test';

function collectOutput(page): string[] {
  const lines: string[] = [];
  page.on('console', msg => lines.push(msg.text()));
  return lines;
}

async function waitForResult(page, timeout = 30_000) {
  // Wait for EXIT_CODE:N or main loop test completion
  await page.waitForFunction(() => {
    const log = document.getElementById('log')?.textContent || '';
    return log.includes('EXIT_CODE:') || log.includes('[exp5] PASS') || log.includes('[exp5] FAIL');
  }, null, { timeout });
}

function getExitCode(log: string): number {
  const match = log.match(/EXIT_CODE:(-?\d+)/);
  return match ? parseInt(match[1]) : -999;
}

// ============================================================================
test('pt-exp1: C POSIX I/O on OPFS (pthreads)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html?run=filesystem_setup');
  await waitForResult(page);
  const log = output.join('\n');
  console.log(log);

  expect(log).toContain('[exp1] PASS');
  expect(getExitCode(log)).toBe(0);
  expect(log).toContain('no (worker)');  // main() runs on worker
});

// ============================================================================
test('pt-exp2: fseek/fwrite at 512-byte granularity (pthreads)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html?run=seek_write');
  await waitForResult(page);
  const log = output.join('\n');
  console.log(log);

  expect(log).toContain('[exp2] PASS');
  expect(getExitCode(log)).toBe(0);
});

// ============================================================================
test('pt-exp3: SyncAccessHandle performance (pthreads)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html?run=performance');
  await waitForResult(page, 60_000);
  const log = output.join('\n');
  console.log(log);

  expect(log).toContain('[exp3] PASS');

  const openMatch = log.match(/open-handle:\s+([\d.]+)\s+ms.*?([\d.]+)\s+ms\/write/);
  const reopenMatch = log.match(/reopen-each:\s+([\d.]+)\s+ms.*?([\d.]+)\s+ms\/write/);
  const ratioMatch = log.match(/ratio:\s+([\d.]+)x/);
  if (openMatch && reopenMatch && ratioMatch) {
    console.log(`\n=== PERFORMANCE RESULTS ===`);
    console.log(`Open-handle: ${openMatch[2]} ms/write`);
    console.log(`Reopen-each: ${reopenMatch[2]} ms/write`);
    console.log(`Ratio: ${ratioMatch[1]}x`);
    console.log(`===========================\n`);
  }
});

// ============================================================================
test('pt-exp4: persistence across reload (pthreads)', async ({ page }) => {
  // Phase 1: Write marker
  const output1 = collectOutput(page);
  await page.goto('/index_pthreads.html?run=persistence_write');
  await waitForResult(page);
  const log1 = output1.join('\n');
  console.log('WRITE:', log1);
  expect(log1).toContain('[exp4] PASS');

  // Check OPFS root from JS (before reload)
  const opfsCheck = await page.evaluate(async () => {
    const root = await navigator.storage.getDirectory();
    const entries = [];
    for await (const [name] of root) entries.push(name);
    return { entries };
  });
  console.log('OPFS root entries:', opfsCheck);

  // Phase 2: Reload and read back
  const output2: string[] = [];
  page.on('console', msg => output2.push(msg.text()));
  await page.goto('/index_pthreads.html?run=persistence_check');
  await waitForResult(page);
  const log2 = output2.join('\n');
  console.log('READ AFTER RELOAD:', log2);

  expect(log2).toContain('[exp4b] PASS');
  expect(getExitCode(log2)).toBe(0);

  // Cleanup
  const output3: string[] = [];
  page.on('console', msg => output3.push(msg.text()));
  await page.goto('/index_pthreads.html?run=persistence_cleanup');
  await waitForResult(page);
});

// ============================================================================
test('pt-exp5: main_loop with OPFS I/O (pthreads)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html?run=main_loop_io');

  // This experiment uses emscripten_set_main_loop — wait for completion
  await page.waitForFunction(() => {
    const log = document.getElementById('log')?.textContent || '';
    return log.includes('[exp5] PASS') || log.includes('[exp5] FAIL');
  }, null, { timeout: 15_000 });

  const log = output.join('\n');
  console.log(log);
  expect(log).toContain('[exp5] PASS');
});

// ============================================================================
test('pt-exp6: EM_ASM from worker thread (pthreads)', async ({ page }) => {
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html?run=em_asm_worker');
  await waitForResult(page);
  const log = output.join('\n');
  console.log(log);

  expect(log).toContain('[exp6] PASS');
  // Now that experiment runs on worker via main(), document should NOT be available
  console.log('NOTE: document available status tells us if EM_ASM runs on worker or main thread');
});

// ============================================================================
test('pt-exp7: JS FS.* API from main thread (pthreads)', async ({ page }) => {
  // Boot without --run so we can use ccall for JS FS API test
  const output = collectOutput(page);
  await page.goto('/index_pthreads.html');
  await page.waitForFunction(() => window['__opfsReady'] === true, null, { timeout: 30_000 });

  const results = await page.evaluate(async () => window['__opfs'].testJsFsApi());
  console.log('JS FS API results:', results);

  // Log results — some may fail due to cross-thread FS access
  for (const [op, result] of Object.entries(results)) {
    console.log(`  ${op}: ${result}`);
  }
});
