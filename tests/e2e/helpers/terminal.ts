// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// moved from tests/utils/terminal.ts and adjusted to keep all artifacts under tests/e2e/test-results
import type { Page, TestInfo } from '@playwright/test';
import * as fs from 'fs';
import * as path from 'path';

/**
 * Install the in-page test shim that captures the xterm instance and provides
 * helper methods such as `injectMedia` and `getTerminalSnapshot`.
 *
 * This is factored out so the shim can be installed centrally from the
 * fixtures before any page scripts run.
 */
export async function installTestShim(page: Page) {
		await page.addInitScript(() => {
				// Ensure a holder exists. Keep the shim separate from any page-provided
				// `__gsTest` so tests can install their own hooks without interfering.
				(window as any).__gsTestShim = (window as any).__gsTestShim || {};
				
				// Install assertion failure tracking
				(window as any).__gsAssertionFailures = [];
				(window as any).__gsAssertionPromise = null;
				(window as any).__gsAssertionResolve = null;
				
				// Create a promise that resolves when an assertion fails
				// This allows tests to await assertion failures
				(window as any).__gsAssertionPromise = new Promise((resolve) => {
					(window as any).__gsAssertionResolve = resolve;
				});
				
				// Listen for assertion failure events and resolve the promise
				window.addEventListener('gs-assertion-failure', (event: any) => {
					const info = event.detail;
					(window as any).__gsAssertionFailures.push(info);
					if ((window as any).__gsAssertionResolve) {
						(window as any).__gsAssertionResolve(info);
					}
				});

				// Intercept assignment to window.Terminal so we can capture the
				// xterm instance when the page creates it. This allows us to read
				// the xterm buffer for accurate terminal snapshots.
				let saved = (window as any).Terminal;
				Object.defineProperty(window, 'Terminal', {
					configurable: true,
					set(v) {
						// If the page assigned the real Terminal class, wrap its
						// constructor so we can capture the instance created.
						const Real = v;
						function ProxyTerminal(...args) {
							// eslint-disable-next-line new-cap
							const inst = new Real(...args);
							(window as any).__gsTestShim._xterm = inst;
							return inst;
						}
						ProxyTerminal.prototype = Real.prototype;
						// Replace Terminal with proxy so subsequent `new Terminal()`
						// yields an instance we can capture.
						saved = ProxyTerminal;
					},
					get() { return saved; }
				});

				// injectMedia writes test files directly to the in-page filesystem.
				// Uses __Module.FS for direct writes, bypassing the drop pipeline
				// so that ROM/disk probing and auto-run are not triggered.
				(window as any).__gsTestShim.injectMedia = (window as any).__gsTestShim.injectMedia || function(files: any[]) {
					return new Promise((resolve, reject) => {
						try {
							// Wait for Module and FS to be available
							const waitForReady = () => new Promise<void>((res, rej) => {
								const deadline = Date.now() + 15000;
								(function poll() {
									const mod = (window as any).__Module;
									if (mod && mod.FS && typeof (window as any).runCommand === 'function') {
										res();
										return;
									}
									if (Date.now() > deadline) {
										rej(new Error('Module.FS not ready in time for injectMedia'));
										return;
									}
									setTimeout(poll, 100);
								})();
							});

							waitForReady().then(() => {
								const FS = (window as any).__Module.FS;
								try { FS.mkdir('/tmp'); } catch (_) {}
								for (const f of files) {
									const bytes = f.data instanceof Uint8Array ? f.data : new Uint8Array(f.data);
									const path = '/tmp/' + f.name;
									try { FS.unlink(path); } catch (_) {}
									FS.writeFile(path, bytes);
								}
								console.log('[test-shim] injectMedia: wrote', files.length, 'file(s) to /tmp');
								resolve(true);
							}).catch(reject);
						} catch (e) {
							console.error('[test-shim] injectMedia error', e);
							reject(e);
						}
					});
				};

				(window as any).__gsTestShim.getTerminalSnapshot = (window as any).__gsTestShim.getTerminalSnapshot || function(maxLines = 600) {
					// Require that we have captured a real xterm instance. If not,
					// fail hard so the test surfaces a clear error instead of
					// silently continuing with empty output.
					const inst = (window as any).__gsTestShim && (window as any).__gsTestShim._xterm;
					if (!inst || !inst.buffer || !inst.buffer.active) {
						throw new Error('xterm buffer not available for getTerminalSnapshot');
					}
					const b = inst.buffer.active;
					const total = b.length;
					const start = Math.max(0, total - (maxLines || 600));
					const lines = [];
					for (let i = start; i < total; i++) {
						const line = b.getLine(i);
						if (line) lines.push(line.translateToString(true));
					}
					return lines.join('\n');
				};

				// Do not attempt to overwrite or lock `window.__gsTest` — use the
				// separate `__gsTestShim` name so the page can install its own hooks
				// if it wants while our shim remains independent.

								// Ensure command logging exists and robustly wrap runCommand to record commands,
								// even if the app assigns it later. Use a property hook.
								(window as any).__commandLog = (window as any).__commandLog || [];
								const logCommand = (cmd: string) => {
									try { (window as any).__commandLog.push(String(cmd)); } catch {}
								};
								let _runCommand: any = (window as any).runCommand;
								if (typeof _runCommand !== 'function') {
									_runCommand = (cmd: string) => { logCommand(cmd); };
								} else {
									const real = _runCommand;
									_runCommand = (cmd: string) => { logCommand(cmd); return real(cmd); };
								}
								try {
									Object.defineProperty(window, 'runCommand', {
										configurable: true,
										get() { return _runCommand; },
										set(v) {
											if (typeof v === 'function') {
												const real = v;
												_runCommand = (cmd: string) => { logCommand(cmd); return real(cmd); };
											} else {
												_runCommand = (cmd: string) => { logCommand(cmd); };
											}
										}
									});
								} catch {
									// Fallback if defineProperty fails: at least ensure a wrapper exists now
									(window as any).runCommand = _runCommand;
								}
								// Backward-compat alias
								(window as any).queueCommand = _runCommand;

						// Provide a minimal __gsTest facade for tests that expect it while
						// delegating implementation to the shimbed functions.
						if (typeof (window as any).__gsTest === 'undefined') {
							(window as any).__gsTest = {
								get commandLog() {
									try { return ((window as any).__commandLog || []).slice(); } catch { return []; }
								},
								clearLog() {
									try { (window as any).__commandLog = []; } catch {}
								},
								getTerminalSnapshot(maxLines = 600) {
									try {
										const snapFn = (window as any).__gsTestShim?.getTerminalSnapshot;
										return typeof snapFn === 'function' ? snapFn(maxLines) : '';
									} catch { return ''; }
								}
							};
						}
		});
}

/**
 * Capture xterm snapshot to the test's output directory, and also mirror it into a
 * deterministic location under the E2E output root (tests/e2e/test-results).
 */
export async function captureXterm(
	page: Page,
	label: string,
	testInfo: TestInfo,
	log?: (m: string) => void
) {
	// Try to collect terminal buffer from the page
	let term = '';
	try {
		term = await page.evaluate(() => {
			try {
				const snapFn = (window as any).__gsTest?.getTerminalSnapshot 
					|| (window as any).__gsTestShim?.getTerminalSnapshot;
				if (snapFn) return snapFn(5000);
				const el = document.getElementById('terminal');
				return el ? (el.textContent || '') : '';
			} catch {
				return '';
			}
		});
	} catch {
		// page might already be closed; continue
	}

	const safeContent = term || '[empty-terminal]';

	// Primary write: under this test's output directory
	const relPath = path.join('xterm', `${label || 'xterm'}.txt`);
	try {
		const outPath = testInfo.outputPath(relPath);
		fs.mkdirSync(path.dirname(outPath), { recursive: true });
		fs.writeFileSync(outPath, safeContent);
	} catch (e: any) {
		log?.(`[xterm] primary write failed: ${e.message}`);
	}

	// Mirror copy: under a stable path inside tests/e2e/test-results
	try {
		const project = (testInfo.project?.name || 'project')
			.replace(/[^a-z0-9]+/gi, '-')
			.replace(/^-+|-+$/g, '')
			.toLowerCase();

		let titleParts: string[] = [];
		if (typeof (testInfo as any).titlePath === 'function') {
			try {
				titleParts = (testInfo as any).titlePath();
			} catch {
				titleParts = [];
			}
		} else if (Array.isArray((testInfo as any).titlePath)) {
			titleParts = (testInfo as any).titlePath;
		} else {
			titleParts = [testInfo.title];
		}

		const titleSlug = titleParts
			.join(' -- ')
			.replace(/[^a-z0-9]+/gi, '-')
			.replace(/^-+|-+$/g, '')
			.toLowerCase();

		// Anchor the mirror to tests/e2e/test-results (relative to this file's dir)
		const resultsRoot = path.resolve(__dirname, '..', 'test-results');
		const mirrorDir = path.join(resultsRoot, `${titleSlug}-${project}`);
		fs.mkdirSync(mirrorDir, { recursive: true });
		const mirrorFile = path.join(mirrorDir, 'xterm.txt');
		fs.writeFileSync(mirrorFile, safeContent);
		log?.(
			`[xterm] mirror saved ${path.relative(process.cwd(), mirrorFile)} (${safeContent.length} chars)`
		);
	} catch (e: any) {
		log?.(`[xterm] mirror write failed: ${e.message}`);
	}
}

/**
 * Check if any GS_ASSERT failures have occurred and throw if so.
 * Call this in tests that want to explicitly verify no assertions fired.
 */
export async function checkForAssertionFailures(page: Page) {
	const failures = await page.evaluate(() => (window as any).__gsAssertionFailures || []);
	if (failures.length > 0) {
		const failure = failures[0];
		throw new Error(
			`GS_ASSERT failed: ${failure.expr}\n` +
			`  at ${failure.file}:${failure.line} in ${failure.func}\n` +
			`  timestamp: ${new Date(failure.timestamp).toISOString()}`
		);
	}
}

/**
 * Wait for a GS_ASSERT failure to occur (useful for negative tests).
 * Returns the assertion failure info or throws on timeout.
 */
export async function waitForAssertionFailure(page: Page, timeoutMs: number = 5000): Promise<any> {
	try {
		const info = await page.evaluate(
			(timeout) => {
				return Promise.race([
					(window as any).__gsAssertionPromise,
					new Promise((_, reject) => 
						setTimeout(() => reject(new Error('Timeout waiting for assertion')), timeout)
					)
				]);
			},
			timeoutMs
		);
		return info;
	} catch (e: any) {
		throw new Error(`Failed to wait for assertion: ${e.message}`);
	}
}

export async function getLastTerminalLine(page: Page): Promise<string> {
	return page.evaluate(() => {
		try {
			const snap = ((window as any).__gsTest?.getTerminalSnapshot 
				|| (window as any).__gsTestShim?.getTerminalSnapshot)?.(400) || '';
			const lines = snap
				.split('\n')
				.map((l: string) => l.trim())
				.filter(Boolean);
			return lines.length ? lines[lines.length - 1] : '';
		} catch {
			return '';
		}
	});
}
