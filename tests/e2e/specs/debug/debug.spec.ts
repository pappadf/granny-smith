// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

import { test, expect } from '../../fixtures';
import { bootWithMedia } from '../../helpers/boot';
import { runCommand, waitForPrompt } from '../../helpers/run-command';

// Media relative paths under tests/data
const ROM_REL = 'roms/Plus_v3.rom';

test.describe('Debug Commands', () => {
  test('single-step "s" command executes one instruction', async ({ page, log }) => {
    test.setTimeout(120_000);

    log('[debug] booting with ROM');
    
    // Boot the system with ROM
    await bootWithMedia(page, ROM_REL);
    
    log('[debug] system booted, waiting for ready state');
    await page.waitForTimeout(5000);
    
    // Explicitly interrupt/stop the scheduler to ensure clean state for single-stepping
    log('[debug] stopping scheduler');
    await runCommand(page, 'stop');
    await page.waitForTimeout(100);
    
    log('[debug] getting initial instruction count');
    
    // Get the initial instruction count via shell command
    const initialCount = await runCommand(page, 'get instr');
    
    log(`[debug] initial instruction count: ${initialCount}`);
    
    // Execute single-step command
    log('[debug] executing single-step command "s"');
    const exitCode = await runCommand(page, 's');
    expect(exitCode).toBe(0);
    
    // Get the new instruction count via shell command
    log('[debug] getting instruction count after step');
    const newCount = await runCommand(page, 'get instr');
    
    log(`[debug] new instruction count: ${newCount}`);
    
    const actualInstructions = Number(newCount - initialCount);
    log(`[debug] expected: 1 instruction, actual: ${actualInstructions} instructions`);
    
    // Verify that exactly 1 instruction was executed
    expect(actualInstructions).toBe(1);
    
    log('[debug] single-step test complete - exactly 1 instruction executed');
  });

  test('single-step "s" command with count executes multiple instructions', async ({ page, log }) => {
    test.setTimeout(120_000);

    log('[debug] booting with ROM');
    
    // Boot the system with ROM
    await bootWithMedia(page, ROM_REL);
    
    log('[debug] system booted, waiting for ready state');
    await page.waitForTimeout(5000);
    
    // Explicitly interrupt/stop the scheduler to ensure clean state for single-stepping
    log('[debug] stopping scheduler');
    await runCommand(page, 'stop');
    await page.waitForTimeout(100);
    
    log('[debug] getting initial instruction count');
    
    // Get the initial instruction count via shell command
    const initialCount = await runCommand(page, 'get instr');
    
    log(`[debug] initial instruction count: ${initialCount}`);
    
    // Execute single-step command with count of 5
    const stepCount = 5;
    log(`[debug] executing single-step command "s ${stepCount}"`);
    const exitCode = await runCommand(page, `s ${stepCount}`);
    expect(exitCode).toBe(0);
    
    // Get the new instruction count via shell command
    log('[debug] getting instruction count after stepping');
    const newCount = await runCommand(page, 'get instr');
    
    log(`[debug] new instruction count: ${newCount}`);
    
    const actualInstructions = Number(newCount - initialCount);
    log(`[debug] expected: ${stepCount} instructions, actual: ${actualInstructions} instructions`);
    
    // Verify that exactly stepCount instructions were executed
    expect(actualInstructions).toBe(stepCount);
    
    log(`[debug] multi-step test complete - exactly ${stepCount} instructions executed`);
  });

  test('"set" command modifies CPU registers and memory', async ({ page, log }) => {
    test.setTimeout(120_000);

    log('[debug] booting with ROM');
    
    // Boot the system with ROM
    await bootWithMedia(page, ROM_REL);
    
    log('[debug] system booted, waiting for ready state');
    await page.waitForTimeout(5000);
    
    // Stop the scheduler to ensure stable state
    log('[debug] stopping scheduler');
    await runCommand(page, 'stop');
    await page.waitForTimeout(100);
    
    // Test 1: Set data register D5
    log('[debug] testing: set d5 0x12345678');
    let exitCode = await runCommand(page, 'set d5 0x12345678');
    expect(exitCode).toBe(0);
    
    // Verify by reading terminal output for confirmation
    await page.waitForTimeout(100);
    
    // Test 2: Set address register A3
    log('[debug] testing: set a3 0x87654321');
    exitCode = await runCommand(page, 'set a3 0x87654321');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 3: Set program counter (PC)
    log('[debug] testing: set pc 0x400000');
    exitCode = await runCommand(page, 'set pc 0x400000');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 4: Set status register (SR)
    log('[debug] testing: set sr 0x2700');
    exitCode = await runCommand(page, 'set sr 0x2700');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 5: Set CCR
    log('[debug] testing: set ccr 0x1f');
    exitCode = await runCommand(page, 'set ccr 0x1f');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 6: Set individual condition code (Z flag)
    log('[debug] testing: set z 1');
    exitCode = await runCommand(page, 'set z 1');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 7: Set memory byte
    log('[debug] testing: set 0x1000.b 0x42');
    exitCode = await runCommand(page, 'set 0x1000.b 0x42');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 8: Set memory word
    log('[debug] testing: set 0x2000.w 0x1234');
    exitCode = await runCommand(page, 'set 0x2000.w 0x1234');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 9: Set memory long
    log('[debug] testing: set 0x3000.l 0xdeadbeef');
    exitCode = await runCommand(page, 'set 0x3000.l 0xdeadbeef');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 10: Case insensitive - lowercase register names
    log('[debug] testing case insensitivity: set d7 0xffffffff');
    exitCode = await runCommand(page, 'set d7 0xffffffff');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 11: SP (alias for A7)
    log('[debug] testing: set sp 0x10000');
    exitCode = await runCommand(page, 'set sp 0x10000');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Test 12: Invalid usage - should fail gracefully
    log('[debug] testing invalid usage: set (no arguments)');
    exitCode = await runCommand(page, 'set');
    expect(exitCode).toBe(0); // Command returns 0 even on usage errors
    await page.waitForTimeout(100);
    
    log('[debug] set command test complete - all variations tested successfully');
  });

  test('"get" command retrieves CPU registers and memory', async ({ page, log }) => {
    test.setTimeout(120_000);

    log('[debug] booting with ROM');
    
    // Boot the system with ROM
    await bootWithMedia(page, ROM_REL);
    
    log('[debug] system booted, waiting for ready state');
    await page.waitForTimeout(5000);
    
    // Stop the scheduler to ensure stable state
    log('[debug] stopping scheduler');
    await runCommand(page, 'stop');
    await page.waitForTimeout(100);
    
    // Test 1: Set and get data register D5
    log('[debug] testing: set d5 0x12345678 then get d5');
    await runCommand(page, 'set d5 0x12345678');
    await page.waitForTimeout(100);
    let exitCode = await runCommand(page, 'get d5');
    expect(exitCode >>> 0).toBe(0x12345678);
    
    // Test 2: Set and get address register A3
    log('[debug] testing: set a3 0x87654321 then get a3');
    await runCommand(page, 'set a3 0x87654321');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get a3');
    expect(exitCode >>> 0).toBe(0x87654321);
    
    // Test 3: Set and get program counter (PC)
    log('[debug] testing: set pc 0x400000 then get pc');
    await runCommand(page, 'set pc 0x400000');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get pc');
    expect(exitCode >>> 0).toBe(0x400000);
    
    // Test 4: Set and get status register (SR)
    log('[debug] testing: set sr 0x2700 then get sr');
    await runCommand(page, 'set sr 0x2700');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get sr');
    expect(exitCode >>> 0).toBe(0x2700);
    
    // Test 5: Set and get CCR
    log('[debug] testing: set ccr 0x1f then get ccr');
    await runCommand(page, 'set ccr 0x1f');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get ccr');
    expect(exitCode >>> 0).toBe(0x1f);
    
    // Test 6: Set and get individual condition code (Z flag)
    log('[debug] testing: set z 1 then get z');
    await runCommand(page, 'set z 1');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get z');
    expect(exitCode >>> 0).toBe(1);
    
    log('[debug] testing: set z 0 then get z');
    await runCommand(page, 'set z 0');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get z');
    expect(exitCode >>> 0).toBe(0);
    
    // Test 7: Set and get memory byte
    log('[debug] testing: set 0x1000.b 0x42 then get 0x1000.b');
    await runCommand(page, 'set 0x1000.b 0x42');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get 0x1000.b');
    expect(exitCode >>> 0).toBe(0x42);
    
    // Test 8: Set and get memory word
    log('[debug] testing: set 0x2000.w 0x1234 then get 0x2000.w');
    await runCommand(page, 'set 0x2000.w 0x1234');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get 0x2000.w');
    expect(exitCode >>> 0).toBe(0x1234);
    
    // Test 9: Set and get memory long
    log('[debug] testing: set 0x3000.l 0xdeadbeef then get 0x3000.l');
    await runCommand(page, 'set 0x3000.l 0xdeadbeef');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get 0x3000.l');
    expect(exitCode >>> 0).toBe(0xdeadbeef);
    
    // Test 10: Case insensitive - lowercase register names
    log('[debug] testing case insensitivity: set d7 0xffffffff then get d7');
    await runCommand(page, 'set d7 0xffffffff');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get d7');
    expect(exitCode >>> 0).toBe(0xffffffff);
    
    // Test 11: SP (alias for A7)
    log('[debug] testing: set sp 0x10000 then get sp');
    await runCommand(page, 'set sp 0x10000');
    await page.waitForTimeout(100);
    exitCode = await runCommand(page, 'get sp');
    expect(exitCode >>> 0).toBe(0x10000);
    
    // Test 12: Invalid usage - should fail gracefully
    log('[debug] testing invalid usage: get (no arguments)');
    exitCode = await runCommand(page, 'get');
    expect(exitCode >>> 0).toBe(0); // Command returns 0 on usage errors
    await page.waitForTimeout(100);
    
    log('[debug] get command test complete - all variations tested successfully');
  });

  test('"x" (examine) command displays memory in hex and ASCII format', async ({ page, log }) => {
    test.setTimeout(120_000);

    log('[debug] booting with ROM');
    
    // Boot the system with ROM
    await bootWithMedia(page, ROM_REL);
    
    log('[debug] system booted, waiting for ready state');
    await page.waitForTimeout(5000);
    
    // Stop the scheduler to ensure stable state
    log('[debug] stopping scheduler');
    await runCommand(page, 'stop');
    await page.waitForTimeout(100);
    
    // Test 1: Write known pattern to memory first
    log('[debug] writing test pattern to memory at 0x1000');
    await runCommand(page, 'set 0x1000.l 0x48656c6c'); // "Hell" in ASCII
    await runCommand(page, 'set 0x1004.l 0x6f20576f'); // "o Wo" in ASCII
    await runCommand(page, 'set 0x1008.l 0x726c6421'); // "rld!" in ASCII
    await runCommand(page, 'set 0x100c.l 0x0a0d0000'); // newline, CR, nulls
    await page.waitForTimeout(100);
    
    // Test 2: Run examine command and capture output
    log('[debug] executing: x 0x1000 32');
    const exitCode = await runCommand(page, 'x 0x1000 32');
    expect(exitCode).toBe(0);
    await page.waitForTimeout(200);
    
    // Test 3: Capture terminal output to verify format
    log('[debug] capturing terminal output');
    const terminalOutput = await page.evaluate(() => {
      try {
        const snapFn = (window as any).__gsTest?.getTerminalSnapshot 
          || (window as any).__gsTestShim?.getTerminalSnapshot;
        if (snapFn) return snapFn(100);
        return '';
      } catch {
        return '';
      }
    });
    
    log('[debug] terminal output length: ' + terminalOutput.length);
    
    // Verify the output format
    // Should contain lines starting with address in format: "00001000  "
    expect(terminalOutput).toContain('00001000  ');
    
    // Verify hex bytes are present (48 65 6c 6c = "Hell")
    expect(terminalOutput).toMatch(/48 65 6c 6c/);
    
    // Verify ASCII representation is present
    // The string "Hello World!" should appear in the ASCII column
    expect(terminalOutput).toMatch(/Hell/);
    expect(terminalOutput).toMatch(/o Wo/);
    expect(terminalOutput).toMatch(/rld!/);
    
    // Test 4: Test default byte count (64 bytes)
    log('[debug] executing: x 0x1000 (default count)');
    await runCommand(page, 'x 0x1000');
    await page.waitForTimeout(100);
    
    // Test 5: Test with ROM address
    log('[debug] executing: x 0x400000 16 (ROM address)');
    const romExitCode = await runCommand(page, 'x 0x400000 16');
    expect(romExitCode).toBe(0);
    await page.waitForTimeout(100);
    
    // Capture terminal to verify ROM data was displayed
    const romOutput = await page.evaluate(() => {
      try {
        const snapFn = (window as any).__gsTest?.getTerminalSnapshot 
          || (window as any).__gsTestShim?.getTerminalSnapshot;
        if (snapFn) return snapFn(100);
        return '';
      } catch {
        return '';
      }
    });
    
    // Should show address starting with 00400000
    expect(romOutput).toContain('00400000  ');
    
    log('[debug] examine command test complete - memory display format verified');
  });

  test('breakpoint "br" command should not get stuck after running past it', async ({ page, log }) => {
    test.setTimeout(30_000);

    log('[debug] booting with ROM');
    await bootWithMedia(page, ROM_REL);
    
    await runCommand(page, 'br 0x004007ba');

    await waitForPrompt(page);

    let pc = await runCommand(page, 'get pc');
    expect(pc >>> 0).toBe(0x004007ba);

    await runCommand(page, 'br 0x004007bc');

    await runCommand(page, 'run');

    await waitForPrompt(page);

    pc = await runCommand(page, 'get pc');
    expect(pc >>> 0).toBe(0x004007bc);
  });

  test('logpoint command logs without stopping execution', async ({ page, log }) => {
    test.setTimeout(30_000);

    log('[debug] booting with ROM');
    await bootWithMedia(page, ROM_REL);
    
    // Enable logging for our test category
    log('[debug] enabling log category "testlog" at level 50');
    await runCommand(page, 'log testlog 50');
    await page.waitForTimeout(100);
    
    // Set a logpoint at a specific address
    log('[debug] setting logpoint at 0x004007ba with category testlog, level 10');
    await runCommand(page, 'logpoint 0x004007ba category=testlog level=10');
    await page.waitForTimeout(100);
    
    // Set a breakpoint a bit further to stop execution and check output
    log('[debug] setting breakpoint at 0x004007c0 to stop after logpoint');
    await runCommand(page, 'br 0x004007c0');
    await page.waitForTimeout(100);
    
    // Run until we hit the breakpoint
    log('[debug] running emulation');
    await runCommand(page, 'run');
    
    // Wait for breakpoint to be hit
    await waitForPrompt(page);
    
    // Capture terminal output to verify logpoint was triggered
    log('[debug] capturing terminal output to check for logpoint messages');
    const terminalOutput = await page.evaluate(() => {
      try {
        const snapFn = (window as any).__gsTest?.getTerminalSnapshot 
          || (window as any).__gsTestShim?.getTerminalSnapshot;
        if (snapFn) return snapFn(100);
        return '';
      } catch {
        return '';
      }
    });
    
    log('[debug] verifying logpoint output in terminal');
    
    // Check that the logpoint message appears in the output
    expect(terminalOutput).toContain('logpoint hit at 0x4007ba');
    expect(terminalOutput).toContain('hit count:');
    
    // Verify we stopped at the breakpoint, not the logpoint
    const pc = await runCommand(page, 'get pc');
    expect(pc >>> 0).toBe(0x004007c0);
    
    log('[debug] logpoint test complete - verified logging without stopping execution');
  });
});
