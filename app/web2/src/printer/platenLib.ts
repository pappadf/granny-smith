// EfterScript's platen library as the worker sees it: the standalone
// Emscripten module `make platen-module` links from the released archive
// (build/platen-<version>.js + .wasm), loaded on the first print job, and
// a thin wrapper over its C ABI (local/platen/<version>/platen.h: one job
// per PAP job — new, feed, read replies / errors, finish, pdf, free).  The
// module is non-threaded and owns its own memory; nothing here touches the
// emulator's heap.  Every function that can allocate may grow that memory,
// so the heap views are re-read from the module after each call.

// The module surface the wrapper uses (EXPORTED_FUNCTIONS and
// EXPORTED_RUNTIME_METHODS in the Makefile's platen-module target).
export interface PlatenModule {
  HEAPU8: Uint8Array;
  HEAPU32: Uint32Array;
  UTF8ToString(ptr: number): string;
  _malloc(bytes: number): number;
  _free(ptr: number): void;
  _platen_job_new(cfg: number): number;
  _platen_job_feed(job: number, bytes: number, len: number): number;
  _platen_job_read_replies(job: number, buf: number, cap: number): number;
  _platen_job_read_errors(job: number, buf: number, cap: number): number;
  _platen_job_finish(job: number): number;
  _platen_job_pdf(job: number, lenPtr: number): number;
  _platen_job_error_name(job: number): number;
  _platen_job_offending(job: number): number;
  _platen_job_pages(job: number): number;
  _platen_job_free(job: number): void;
  _platen_last_error(): number;
}

interface PlatenModuleConfig {
  locateFile?(path: string): string;
  print?(s: string): void;
  printErr?(s: string): void;
}

type PlatenModuleFactory = (config: PlatenModuleConfig) => Promise<PlatenModule>;

// platen.h constants.
export const PLATEN_ABI_VERSION = 1;
export const PLATEN_OK = 0;
export const PLATEN_DONE = 1;
export const PLATEN_OUTCOME_OK = 0;
export const PLATEN_OUTCOME_ERROR = 1;
export const PLATEN_OUTCOME_BUDGET = 2;

// platen_config on wasm32: abi_version u32 @0, identity ptr @4,
// identity_len @8, prelude ptr @12, prelude_len @16, server_password i32
// @20, compress int @24, embed_all_fonts int @28, step_budget u64 @32
// (8-aligned); 40 bytes.  platen_entry: key ptr @0, value ptr @4; 8 bytes.
const CONFIG_BYTES = 40;
const ENTRY_BYTES = 8;

// The scratch buffer the reply / error channels are drained through.
const SCRATCH_BYTES = 64 << 10;

// Fetches and instantiates the module.  `moduleUrl` is the ES6 glue
// (cache-busted by the page like main.mjs), `wasmUrl` where its binary is.
export async function loadPlatenModule(moduleUrl: string, wasmUrl: string): Promise<PlatenModule> {
  const mod = (await import(/* @vite-ignore */ moduleUrl)) as { default: PlatenModuleFactory };
  return mod.default({
    locateFile: (p: string) => (p.endsWith('.wasm') ? wasmUrl : p),
    print: (s: string) => console.log('[platen]', s),
    printErr: (s: string) => console.warn('[platen]', s),
  });
}

// One statusdict identity entry, as OPEN carries it (the value a
// PostScript literal).
export interface PlatenEntry {
  key: string;
  value: string;
}

// The platen_config fields the bridge sends in OPEN.
export interface PlatenJobConfig {
  identity: PlatenEntry[];
  prelude: Uint8Array;
  serverPassword: number;
  compress: boolean;
  embedAllFonts: boolean;
  stepBudgetLo: number;
  stepBudgetHi: number;
}

// A drained channel: its bytes, cut at `cap` with `truncated` set when
// the channel held more (the rest is dropped).
export interface Drained {
  bytes: Uint8Array;
  truncated: boolean;
}

// The library, one call per ABI function.  A job handle is the C pointer.
export class PlatenLib {
  private readonly m: PlatenModule;
  private scratch = 0; // the drain buffer, allocated on first use

  constructor(m: PlatenModule) {
    this.m = m;
  }

  // Creates a job from `cfg`; 0 on failure (lastError says why).  The
  // config and its strings live in one allocation freed after the call:
  // the library retains nothing the host passes in.
  jobNew(cfg: PlatenJobConfig): number {
    const enc = new TextEncoder();
    const strings: Uint8Array[] = [];
    let stringBytes = 0;
    for (const e of cfg.identity) {
      const k = enc.encode(e.key + '\0');
      const v = enc.encode(e.value + '\0');
      strings.push(k, v);
      stringBytes += k.length + v.length;
    }
    const entriesAt = CONFIG_BYTES;
    const stringsAt = entriesAt + ENTRY_BYTES * cfg.identity.length;
    const preludeAt = stringsAt + stringBytes;
    const total = preludeAt + cfg.prelude.length;
    const block = this.m._malloc(total);
    if (!block) throw new Error('platen: out of memory for a job configuration');
    try {
      const u8 = this.m.HEAPU8;
      const u32 = this.m.HEAPU32;
      u8.fill(0, block, block + total);
      // The entries, then their strings
      let sp = block + stringsAt;
      for (let i = 0; i < cfg.identity.length; i++) {
        const k = strings[2 * i];
        const v = strings[2 * i + 1];
        u8.set(k, sp);
        u32[(block + entriesAt + ENTRY_BYTES * i) >> 2] = sp;
        sp += k.length;
        u8.set(v, sp);
        u32[(block + entriesAt + ENTRY_BYTES * i + 4) >> 2] = sp;
        sp += v.length;
      }
      u8.set(cfg.prelude, block + preludeAt);
      // The config itself
      u32[block >> 2] = PLATEN_ABI_VERSION;
      u32[(block + 4) >> 2] = cfg.identity.length ? block + entriesAt : 0;
      u32[(block + 8) >> 2] = cfg.identity.length;
      u32[(block + 12) >> 2] = cfg.prelude.length ? block + preludeAt : 0;
      u32[(block + 16) >> 2] = cfg.prelude.length;
      u32[(block + 20) >> 2] = cfg.serverPassword >>> 0;
      u32[(block + 24) >> 2] = cfg.compress ? 1 : 0;
      u32[(block + 28) >> 2] = cfg.embedAllFonts ? 1 : 0;
      u32[(block + 32) >> 2] = cfg.stepBudgetLo >>> 0;
      u32[(block + 36) >> 2] = cfg.stepBudgetHi >>> 0;
      return this.m._platen_job_new(block);
    } finally {
      this.m._free(block);
    }
  }

  // Appends `bytes` and executes as far as they allow: PLATEN_OK,
  // PLATEN_DONE, or a negative failure code.
  feed(job: number, bytes: Uint8Array): number {
    if (bytes.length === 0) return this.m._platen_job_feed(job, 0, 0);
    const buf = this.m._malloc(bytes.length);
    if (!buf) throw new Error('platen: out of memory for a feed');
    try {
      this.m.HEAPU8.set(bytes, buf);
      return this.m._platen_job_feed(job, buf, bytes.length);
    } finally {
      this.m._free(buf);
    }
  }

  // Drains one channel up to `cap` bytes; what exceeds the cap is read
  // and dropped so the channel is empty afterwards.
  private drain(
    read: (job: number, buf: number, cap: number) => number,
    job: number,
    cap: number,
  ): Drained {
    if (!this.scratch) {
      this.scratch = this.m._malloc(SCRATCH_BYTES);
      if (!this.scratch) throw new Error('platen: out of memory for the drain buffer');
    }
    const chunks: Uint8Array[] = [];
    let total = 0;
    let truncated = false;
    for (;;) {
      const n = read(job, this.scratch, SCRATCH_BYTES);
      if (n === 0) break;
      if (total >= cap) {
        truncated = true;
        continue;
      }
      const keep = Math.min(n, cap - total);
      if (keep < n) truncated = true;
      chunks.push(this.m.HEAPU8.slice(this.scratch, this.scratch + keep));
      total += keep;
    }
    const bytes = new Uint8Array(total);
    let at = 0;
    for (const c of chunks) {
      bytes.set(c, at);
      at += c.length;
    }
    return { bytes, truncated };
  }

  // Pending reply bytes (the program's standard output), at most `cap`.
  readReplies(job: number, cap: number): Drained {
    return this.drain((j, b, c) => this.m._platen_job_read_replies(j, b, c), job, cap);
  }

  // Pending error-report bytes (standard error), at most `cap`.
  readErrors(job: number, cap: number): Drained {
    return this.drain((j, b, c) => this.m._platen_job_read_errors(j, b, c), job, cap);
  }

  // End of data: runs to completion and closes the document; a
  // PLATEN_OUTCOME_* code or a negative failure code.
  finish(job: number): number {
    return this.m._platen_job_finish(job);
  }

  // A copy of the finished document (empty before finish or when there
  // is none).
  pdf(job: number): Uint8Array {
    const lenPtr = this.m._malloc(4);
    if (!lenPtr) throw new Error('platen: out of memory');
    try {
      const ptr = this.m._platen_job_pdf(job, lenPtr);
      const len = this.m.HEAPU32[lenPtr >> 2];
      if (!ptr || !len) return new Uint8Array(0);
      return this.m.HEAPU8.slice(ptr, ptr + len);
    } finally {
      this.m._free(lenPtr);
    }
  }

  errorName(job: number): string {
    return this.m.UTF8ToString(this.m._platen_job_error_name(job));
  }

  offending(job: number): string {
    return this.m.UTF8ToString(this.m._platen_job_offending(job));
  }

  pages(job: number): number {
    return this.m._platen_job_pages(job) >>> 0;
  }

  free(job: number): void {
    this.m._platen_job_free(job);
  }

  // The message of the last failure, or "".
  lastError(): string {
    return this.m.UTF8ToString(this.m._platen_last_error());
  }
}
