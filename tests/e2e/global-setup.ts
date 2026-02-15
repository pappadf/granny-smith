// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// moved from tests/global-setup.ts; path to test_server.py updated (same dir)
import { FullConfig } from '@playwright/test';
import { spawn, execSync } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';

async function run(cmd: string, args: string[]) {
  await new Promise<void>((res, rej) => {
    const p = spawn(cmd, args, { stdio: 'inherit' });
    p.on('exit', c => c === 0 ? res() : rej(new Error(`${cmd} failed (${c})`)));
  });
}

/**
 * Check if proprietary test data is available.
 * Returns true if tests/data has the required files.
 */
function checkTestDataAvailable(): boolean {
  const repoRoot = path.join(__dirname, '..', '..');
  const fetchScript = path.join(repoRoot, 'scripts', 'fetch-test-data.sh');
  
  try {
    execSync(`${fetchScript} --check`, { stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}

export default async function globalSetup(_config: FullConfig) {
  // Ensure we run from repository root (two levels up from tests/e2e)
  const repoRoot = path.join(__dirname, '..', '..');
  process.chdir(repoRoot);
  console.log('[globalSetup] cwd', process.cwd());

  // Check for test data availability
  const hasTestData = checkTestDataAvailable();
  if (!hasTestData) {
    console.error('\n' + '='.repeat(70));
    console.error('ERROR: Test data is not available');
    console.error('='.repeat(70));
    console.error('');
    console.error('E2E tests require proprietary test data (ROM images, disk images)');
    console.error('that cannot be distributed with this open source project.');
    console.error('');
    console.error('For maintainers:');
    console.error('  Run: ./scripts/fetch-test-data.sh');
    console.error('  (requires GS_TEST_DATA_TOKEN environment variable)');
    console.error('');
    console.error('For contributors:');
    console.error('  See docs/TEST_DATA.md for information on obtaining or');
    console.error('  providing your own test data.');
    console.error('');
    console.error('='.repeat(70) + '\n');
    throw new Error('Test data not available. See docs/TEST_DATA.md for details.');
  }
  console.log('[globalSetup] test data available');

  // Best-effort cleanup from previous runs
  try {
    const pidFile = path.join(process.cwd(), 'test-server.pid');
    if (fs.existsSync(pidFile)) {
      const pid = parseInt(fs.readFileSync(pidFile, 'utf8'), 10);
      if (!isNaN(pid)) {
        try { process.kill(pid); } catch {}
      }
      try { fs.unlinkSync(pidFile); } catch {}
    }
  } catch {}
  console.log('[globalSetup] make');
  await run('make', []);
  console.log('[globalSetup] start dev server');
  const script = path.join(__dirname, 'test_server.py');
  const proc = spawn('python3', [script, '--root', 'build', '--port', '18080'], { stdio: 'inherit' });
  fs.writeFileSync(path.join(process.cwd(), 'test-server.pid'), String(proc.pid));
  await new Promise(r => setTimeout(r, 1500));
}
