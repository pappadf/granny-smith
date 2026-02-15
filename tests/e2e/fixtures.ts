// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// moved from tests/fixtures.ts unchanged except updated utils paths
import { test as base, expect, Page } from '@playwright/test';
import { initLogging, TestLogger } from './helpers/logging';
import { captureXterm, installTestShim } from './helpers/terminal';

type LoggingFixtures = {
  log: (msg: string) => void;
  logger: TestLogger;
};

export const test = base.extend<LoggingFixtures>({
  // Create a completely fresh browser context for each test
  // This ensures total isolation - no shared storage, cookies, or state
  context: async ({ browser }, use, testInfo) => {
    // Pass through all project use options (viewport, deviceScaleFactor, etc.)
    // and layer our video recording logic on top
    const { video, trace, screenshot, ...contextOptions } = testInfo.project.use;
    const context = await browser.newContext({
      ...contextOptions,
      recordVideo: video && video !== 'off' ? {
        dir: testInfo.outputDir,
        size: testInfo.project.use.viewport
      } : undefined
    });
    await use(context);
    await context.close();
    // Video is automatically saved to outputDir and attached by Playwright
  },
  
  // Ensure the page has the shim installed before any navigation or other
  // fixtures run.
  page: async ({ context }, use, testInfo) => {
    const page = await context.newPage();
    let assertionFailure: any = null;
    let pollingActive = true;
    let testPromise: Promise<void> | null = null;
    
    // Listen for assertion failures via console.error
    // The box drawn by the browser is already prominent, no need for additional wrapper
    page.on('console', (msg) => {
      // Just pass through - box is self-explanatory
    });
    
    // Listen for uncaught exceptions
    page.on('pageerror', (error) => {
      if (error.name === 'GSAssertionError' || error.message.includes('GS_ASSERT')) {
        console.error(`\n${'='.repeat(70)}`);
        console.error('GS_ASSERT FAILURE DETECTED (page error):');
        console.error(error.message);
        console.error('='.repeat(70) + '\n');
      }
    });
    
    try { await installTestShim(page as Page); } catch {}
    
    // Poll for assertion failures and abort test immediately when detected
    const pollForAssertion = async () => {
      try {
        while (pollingActive && !assertionFailure) {
          try {
            const detected = await Promise.race([
              page.evaluate(() => window.__gsAssertionDetected || false),
              new Promise<boolean>((_, reject) => 
                setTimeout(() => reject(new Error('eval timeout')), 1000)
              )
            ]).catch(() => false);
            
            if (detected && !assertionFailure) {
              const failures = await page.evaluate(() => window.__gsAssertionFailures || []).catch(() => []);
              if (failures.length > 0) {
                const failure = failures[0];
                assertionFailure = {
                  expr: failure.expr,
                  file: failure.file,
                  line: failure.line,
                  func: failure.func,
                  timestamp: failure.timestamp
                };
                // Stop polling
                pollingActive = false;
                // Immediately reject the test promise if it's running
                if (testPromise) {
                  throw new Error(
                    `GS_ASSERT failed: ${assertionFailure.expr}\n` +
                    `  at ${assertionFailure.file}:${assertionFailure.line} in ${assertionFailure.func}\n` +
                    `  timestamp: ${new Date(assertionFailure.timestamp).toISOString()}`
                  );
                }
                break;
              }
            }
          } catch (e: any) {
            // Page might be closing or navigating
            if (e.message?.includes('Target closed') || e.message?.includes('Execution context')) {
              break;
            }
            // Re-throw assertion errors
            if (e.message?.includes('GS_ASSERT')) {
              throw e;
            }
          }
          await new Promise(resolve => setTimeout(resolve, 50));
        }
      } catch (e) {
        // Stop polling
        pollingActive = false;
        throw e;
      }
    };
    
    // Start polling in background
    const pollerPromise = pollForAssertion();
    
    try {
      // Race the test execution against the assertion poller
      testPromise = (async () => {
        await use(page);
      })();
      
      await Promise.race([
        testPromise,
        pollerPromise
      ]);
    } finally {
      pollingActive = false;
      
      // Capture terminal snapshot before throwing any errors or closing page
      try {
        const slug = testInfo.title.replace(/[^a-z0-9]+/gi, '-').replace(/^-+|-+$/g, '').toLowerCase();
        await captureXterm(page as Page, slug, testInfo);
      } catch {}
      
      // Attach assertion details if detected
      if (assertionFailure) {
        try {
          testInfo.attach('assertion-failure', {
            body: JSON.stringify(assertionFailure, null, 2),
            contentType: 'application/json'
          });
        } catch {}
        
        throw new Error(
          `GS_ASSERT failed: ${assertionFailure.expr}\n` +
          `  at ${assertionFailure.file}:${assertionFailure.line} in ${assertionFailure.func}\n` +
          `  timestamp: ${new Date(assertionFailure.timestamp).toISOString()}`
        );
      }
    }
  },
  logger: async ({ page }, use, testInfo) => {
    const label = testInfo.title;
    const logger = initLogging(page, { label });
    try { await use(logger); } finally { await logger.attachAll(testInfo); }
  },
  log: async ({ logger }, use) => { await use(logger.log); }
});

export { expect } from '@playwright/test';
