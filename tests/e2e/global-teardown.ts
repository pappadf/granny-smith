// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// moved from tests/global-teardown.ts unchanged
import * as fs from 'fs';
import * as path from 'path';
import { FullConfig } from '@playwright/test';

export default async function globalTeardown(_config: FullConfig) {
  const pidFile = path.join(process.cwd(), 'test-server.pid');
  if (fs.existsSync(pidFile)) {
    try {
      const pid = parseInt(fs.readFileSync(pidFile, 'utf8'), 10);
      if (!isNaN(pid)) { try { process.kill(pid); } catch {} }
    } finally {
      fs.unlinkSync(pidFile);
    }
  }
}
