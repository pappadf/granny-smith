// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 in-browser trace of the speed governor through a whole boot: a Power
// Macintosh 8500 booting System 7.6 (the pm8500-boot-desktop scenario's
// machine and disk) under web2's default pacing (Faster, the governor on),
// the engine the page picks (the PowerPC translation tier).  Every
// PERF_POLL_MS it reads the status bar -- the speed rung the governor is
// at, the MIPS, ticks per second, the typical and worst tick, the engine
// and the share that ran translated -- and every PERF_PROBE_MS it asks the
// core for the guest's instruction count and cycles, so the readings line
// up with the boot's phases (tests/bench/pm8500-boot-desktop.script).
//
// Results are `PERFGOV t=<s> ...` (status bar) and `PERFGOVP t=<s> ...`
// (core probe) lines on stdout -- a trace, not a gate.  Opt-in:
// PERF_BOOT_GOVERNOR=1.  PERF_SECS (default 300) bounds the run;
// PERF_ENGINE=predecoded|jit|jit-kernel switches the engine first;
// PERF_SPEED overrides the pacing (paced|accelerated|turbo); PERF_SETUP
// runs shell lines (separated by ';') after the machine starts -- with
// `log.perf_every = 1` the core's perf lines come out as `PERFLOG t=<s>`.

import { test, expect, type Page } from "../helpers/test";
import type { CDPSession } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFileStreaming } from "../helpers/web2-fs";
import { CONSOLE_OUTPUT, terminalRun as typeLine } from "../helpers/terminal";

test.skip(!process.env.PERF_BOOT_GOVERNOR, "opt-in: PERF_BOOT_GOVERNOR=1");
// web2's own default pacing unless PERF_SPEED names one
test.use({ gsSpeed: process.env.PERF_SPEED ?? null });

const terminalRun = (page: Page, line: string) =>
  typeLine(page, line, { settleMs: 100 });

const DATA = path.resolve(__dirname, "../../data");
const ROM = path.join(DATA, "roms", "pm7500-pm8500-pm9500-96cd923d.rom");
const HD = path.join(DATA, "systems", "system_7_6_170mb_24ac.img");
const SECS = Number(process.env.PERF_SECS ?? 300);
const POLL_MS = Number(process.env.PERF_POLL_MS ?? 500);
const PROBE_MS = Number(process.env.PERF_PROBE_MS ?? 5000);
// PERF_CPU_PROFILE=<secs>: sample every emscripten pthread with the V8 CPU
// profiler, one .cpuprofile per thread per window of that many seconds,
// written to the test's output directory (load them in DevTools, or sum
// them with a script).  Function names need a wasm linked with names
// (`make ui2 EXTRA_LDFLAGS=-g2`) served by a server started by hand
// (`python3 tests/e2e/test_server.py --root app/web2/dist --port 18090`,
// which the config then reuses): its own webServer runs `make ui2`, which
// relinks without them.
const PROFILE_SECS = Number(process.env.PERF_CPU_PROFILE ?? 0);

// The CPU profiler on the page's workers, through one CDP session
// (non-flattened target messages: Playwright's session carries no
// sessionId of its own).
class WorkerProfiler {
  private seq = 0;
  private pending = new Map<string, (r: any) => void>();
  readonly threads: string[] = [];
  constructor(private cdp: CDPSession) {
    cdp.on("Target.attachedToTarget", (e: any) => {
      if (e.targetInfo.type === "worker" && e.targetInfo.title === "em-pthread") this.threads.push(e.sessionId);
    });
    cdp.on("Target.receivedMessageFromTarget", (e: any) => {
      const m = JSON.parse(e.message);
      const k = `${e.sessionId}:${m.id}`;
      if (m.id !== undefined && this.pending.has(k)) {
        this.pending.get(k)!(m.result ?? m.error);
        this.pending.delete(k);
      }
    });
  }
  static async attach(page: Page): Promise<WorkerProfiler> {
    const cdp = await page.context().newCDPSession(page);
    const p = new WorkerProfiler(cdp);
    await cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: false });
    return p;
  }
  send(sessionId: string, method: string, params: object = {}): Promise<any> {
    const id = ++this.seq;
    const r = new Promise<any>((res) => this.pending.set(`${sessionId}:${id}`, res));
    void this.cdp.send("Target.sendMessageToTarget", { sessionId, message: JSON.stringify({ id, method, params }) });
    return r;
  }
  async start(): Promise<void> {
    for (const t of this.threads) {
      await this.send(t, "Profiler.enable");
      await this.send(t, "Profiler.setSamplingInterval", { interval: 500 });
      await this.send(t, "Profiler.start");
    }
  }
  // Stop every thread's profile, save it, and start the next window
  async cut(dir: string, label: string): Promise<void> {
    fs.mkdirSync(dir, { recursive: true });
    for (const [i, t] of this.threads.entries()) {
      const r = await this.send(t, "Profiler.stop");
      if (r?.profile) fs.writeFileSync(path.join(dir, `${label}-thread${i}.cpuprofile`), JSON.stringify(r.profile));
      await this.send(t, "Profiler.start");
    }
  }
}

let probeSeq = 0;
async function probe(page: Page, exprs: string[]): Promise<string[] | null> {
  const key = `pg${++probeSeq}`;
  const body = exprs.map((e) => `\${${e}}`).join(",");
  await terminalRun(page, `echo "${key}=[${body}]"`);
  for (let i = 0; i < 20; i++) {
    await page.waitForTimeout(100);
    const text = await page.locator(CONSOLE_OUTPUT).innerText();
    const m = text.match(new RegExp(`${key}=\\[([^\\]$]*)\\]`));
    if (m) return m[1].split(",");
  }
  return null;
}

// The core's perf lines (log.perf_every) land in the terminal: print each
// new one once, stamped with when it was collected
const seenPerf = new Set<string>();
async function collectPerfLines(page: Page, t: number): Promise<void> {
  const text = await page.locator(CONSOLE_OUTPUT).innerText();
  for (const line of text.split("\n")) {
    if (!line.includes("perf:") || seenPerf.has(line)) continue;
    seenPerf.add(line);
    console.log(`PERFLOG t=${t.toFixed(1)} ${line.trim()}`);
  }
}

test("perf-boot-governor: pm8500 System 7.6 boot under the governor (trace)", async ({
  page,
}) => {
  test.setTimeout((SECS + 300) * 1000);
  const profiler = PROFILE_SECS > 0 ? await WorkerProfiler.attach(page) : null;
  await gotoWeb2(page);
  await stageOpfsFileStreaming(page, `/opfs/images/hd/${path.basename(HD)}`, HD);
  const [romChooser] = await Promise.all([
    page.waitForEvent("filechooser"),
    page.getByRole("button", { name: "Load ROM..." }).click(),
  ]);
  await romChooser.setFiles(ROM);
  await page.getByRole("button", { name: "New Machine..." }).click();
  const model = page.locator("#cfg-model");
  await expect(model.locator('option[value="pm8500"]')).toHaveCount(1, { timeout: 30_000 });
  await model.selectOption("pm8500");
  await page.locator("#cfg-opt-memory").selectOption({ label: "32 MB" });
  await page.locator("#cfg-media-scsi-0").selectOption({ label: path.basename(HD) });
  await page.getByRole("button", { name: "Start", exact: true }).click();
  if (profiler) await profiler.start();
  const t0 = Date.now();
  let nextCut = PROFILE_SECS;
  await expect(
    page.locator(".toast .msg").filter({ hasText: "Machine started" }),
  ).toBeVisible({ timeout: 60_000 });
  await page.locator('button.ptab[data-tab="terminal"]').click();
  await expect(page.locator(".console")).toBeVisible({ timeout: 15_000 });
  const engine = process.env.PERF_ENGINE;
  if (engine) {
    const steps: [string, number][] =
      engine === "predecoded"
        ? [["predecode.enabled", 1], ["jit.enabled", 0]]
        : [["predecode.enabled", 1], ["jit.supervisor", engine === "jit-kernel" ? 1 : 0], ["jit.enabled", 1]];
    for (const [k, v] of steps) await terminalRun(page, `${k} = ${v}`);
  }
  for (const line of (process.env.PERF_SETUP ?? "").split(";").map((l) => l.trim()).filter(Boolean))
    await terminalRun(page, line);
  const mode = await probe(page, ["scheduler.mode", "scheduler.cpi", "jit.enabled", "jit.supervisor", "jit.async", "checkpoint.auto"]);
  console.log(`PERFGOV start mode=${mode?.join(" ")} engine=${engine ?? "default"}`);

  let nextProbe = 0;
  while ((Date.now() - t0) / 1000 < SECS) {
    const t = (Date.now() - t0) / 1000;
    const sb = await page.evaluate(() => {
      const q = (s: string) => document.querySelector(s);
      return {
        speed: q(".sb-speed .label")?.textContent?.trim() ?? "",
        mips: q(".sb-mips .label")?.textContent?.trim() ?? "",
        mipsTitle: q(".sb-mips")?.getAttribute("title") ?? "",
        engine: q(".sb-engine .label")?.textContent?.trim() ?? "",
        engineTitle: q(".sb-engine")?.getAttribute("title") ?? "",
      };
    });
    const tps = sb.mipsTitle.match(/([\d.]+) ticks\/s; tick ([\d.]+) ms typical, ([\d.]+) ms worst; bridge request ([\d.]+) ms worst/);
    const jit = sb.engineTitle.match(/([\d.]+)\s*%/);
    console.log(
      `PERFGOV t=${t.toFixed(1)} speed=${sb.speed} mips=${sb.mips.replace(" MIPS", "")} tps=${tps?.[1] ?? ""} tick_p50=${tps?.[2] ?? ""} tick_max=${tps?.[3] ?? ""} poll_max=${tps?.[4] ?? ""} engine=${sb.engine.replace(/\s+/g, "_")} jit=${jit?.[1] ?? ""}`,
    );
    if (t >= nextProbe) {
      nextProbe = t + PROBE_MS / 1000;
      const p = await probe(page, ["machine.cpu.instr_count", "scheduler.cycles", "scheduler.host_wall_ns", "machine.cpu.pc:08x", "jit.instructions", "jit.compile_us", "jit.deferred"]);
      if (p)
        console.log(
          `PERFGOVP t=${((Date.now() - t0) / 1000).toFixed(1)} instr=${p[0]} cycles=${p[1]} wall_ns=${p[2]} pc=${p[3]} jit_instr=${p[4]} jit_compile_us=${p[5]} jit_deferred=${p[6]}`,
        );
    }
    if (Math.floor(t / 5) !== Math.floor((t + POLL_MS / 1000) / 5)) await collectPerfLines(page, t);
    if (profiler && t >= nextCut) {
      await profiler.cut(test.info().outputDir, `w${String(Math.round(nextCut)).padStart(4, "0")}`);
      console.log(`PERFPROF t=${t.toFixed(1)} window ending ${nextCut}s saved`);
      nextCut += PROFILE_SECS;
    }
    await page.waitForTimeout(POLL_MS);
  }
  await collectPerfLines(page, (Date.now() - t0) / 1000);
  await page.screenshot({ path: test.info().outputPath("end.png") });
});
