// A recording stand-in for the gsEval bridge.
//
// Component tests mock whole bus functions (writeRegister, removeBreakpoint…),
// so a bus function that sends the core a path it never resolves still passes
// them.  Bus tests use this instead: the real bus module runs, and only the
// bridge underneath it is replaced, so a test asserts the exact (path, args)
// the frontend sends and sees how the bus reacts to the core's real answer
// shapes — a value, `null` (a V_NONE success), or `{ error }`.
//
// Usage:
//   import { bridge } from '../helpers/bridgeMock';
//   vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());
//   bridge.reply('debug.breakpoints.count', 2);
//   ... call the bus function ...
//   expect(bridge.paths()).toContain('debug.breakpoints.count');

import { vi } from 'vitest';

// One recorded bridge request.
export interface BridgeCall {
  path: string;
  args: unknown[] | undefined;
}

// A canned reply: a fixed value, or a function of the request.
type Reply = unknown | ((args: unknown[] | undefined) => unknown);

// The core's answer to a path nobody taught the mock: what gs_eval really says.
function unresolved(path: string): { error: string } {
  return { error: `path '${path}' did not resolve` };
}

// The bridge double: a route table, and a log of every call made through it.
class BridgeMock {
  calls: BridgeCall[] = [];
  private routes = new Map<string, Reply>();

  // Answer `path` with `value` (or with the result of calling it on the args).
  reply(path: string, value: Reply): this {
    this.routes.set(path, value);
    return this;
  }

  // Forget every route and every recorded call.
  reset(): void {
    this.calls = [];
    this.routes.clear();
  }

  // The paths requested so far, in order.
  paths(): string[] {
    return this.calls.map((c) => c.path);
  }

  // The gsEval replacement the mocked module exports.
  gsEval = vi.fn(async (path: string, args?: unknown[]): Promise<unknown> => {
    this.calls.push({ path, args });
    if (!this.routes.has(path)) return unresolved(path);
    const r = this.routes.get(path);
    return typeof r === 'function' ? (r as (a: unknown[] | undefined) => unknown)(args) : r;
  });
}

// The single instance shared by a test file and its vi.mock factory.
export const bridge = new BridgeMock();

// The module that replaces '@/bus/emulator': the real exports, with the bridge
// swapped for the double and the module reported ready.
export async function emulatorModule(): Promise<Record<string, unknown>> {
  const actual = await vi.importActual<Record<string, unknown>>('@/bus/emulator');
  return {
    ...actual,
    gsEval: bridge.gsEval,
    isModuleReady: () => true,
  };
}
