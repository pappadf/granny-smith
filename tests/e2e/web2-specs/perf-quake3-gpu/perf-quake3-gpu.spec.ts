// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e, measurement: Quake III Arena's demo `four` on the beige G3 with
// a Rage 128 GL, in the browser, with the 3D drawn by the host's GPU
// (the WebGPU takeover) and the PowerPC JIT on: what a user sees.
//
// OPT-IN (GS_Q3_GPU=1) and meant for a machine with a real GPU — an Apple
// silicon Mac, run natively (not in a container).  On a software adapter
// (SwiftShader: CI, containers) it skips: the numbers would time
// SwiftShader.  README.md, next to this file, is the runbook.
//
// The flow is the integration row g3-rage128-quake3-demo's: the stripped
// 9.2.1 disk and the cut Quake III 1.30 CD from gs-test-data, the Finder
// opens the CD and starts Quake3, Tab + Return past the CD key screen,
// Escape to the main menu.  Then, per run (GS_Q3_MODES, default
// "paced,turbo"), in Quake's console:
//     set nextdemo "echo gsdemoend<n>"
//     demo four
// When a demo finishes Quake disconnects and runs `nextdemo`, so
// `gsdemoend<n>` lands on its console at that moment.  The run is timed
// from the Return that enters `demo four` (the scheduler is paused while
// the counters are read and the key is queued) to the poll that first sees
// that line (GS_Q3_POLL_MS apart), where the scheduler is paused again and
// the counters read.  Quake's console text is a ring of 16-bit cells
// (colour, character) in guest RAM, found by the command's echo.
//
// Output: `Q3GPU ...` lines on stdout and perf-quake3-gpu.json, plus each
// run's console text, under test-results/perf-quake3-gpu/.

import { test, expect, type Page } from "@playwright/test";
import * as fs from "node:fs";
import * as path from "node:path";
import { gotoWeb2, stageOpfsFileStreaming } from "../../helpers/web2-fs";
import { gsCallInPage, gsEvalInPage } from "../../helpers/web2-eval";
import { WorkerProfiler, summarize } from "./profile";

const ENABLED = process.env.GS_Q3_GPU === "1";
const ALLOW_SOFTWARE_GPU = process.env.GS_Q3_ALLOW_SOFTWARE_GPU === "1";
const MODES = (process.env.GS_Q3_MODES ?? "paced,turbo")
  .split(",")
  .map((m) => m.trim())
  .filter(Boolean);
const ENGINE = process.env.GS_Q3_ENGINE ?? "jit"; // jit | predecoded | interpreter
const POLL_MS = Number(process.env.GS_Q3_POLL_MS ?? 100);
// The Rage 128's 3D: webgpu (the host GPU) or sw (its software rasteriser,
// the comparison row; also the flow check on a machine without a GPU)
const RASTER = process.env.GS_Q3_RASTER ?? "webgpu";
const TIMEDEMO = process.env.GS_Q3_TIMEDEMO === "1";
const TIMEOUT_MIN = Number(process.env.GS_Q3_TIMEOUT_MIN ?? 90);
const LABEL = process.env.GS_Q3_LABEL ?? "unlabeled";
// A sweep of the governor's floor during each run (a measurement aid):
// e.g. GS_Q3_SWEEP=0.25,0.5,0.75,1, GS_Q3_SWEEP_S seconds at each
const SWEEP = (process.env.GS_Q3_SWEEP ?? "")
  .split(",")
  .filter(Boolean)
  .map(Number);
const SWEEP_S = Number(process.env.GS_Q3_SWEEP_S ?? 8);
// GS_Q3_PROFILE=N: CPU profiles of the page's workers for N seconds of each
// run, GS_Q3_PROFILE_AT seconds in (function names need a module linked
// with EXTRA_LDFLAGS=--profiling-funcs)
const PROFILE_S = Number(process.env.GS_Q3_PROFILE ?? 0);
const PROFILE_AT = Number(process.env.GS_Q3_PROFILE_AT ?? 25);
const CDP_PORT = 9333;
// GS_Q3_JS_FLAGS: V8 flags for the browser (an experiment's knobs, e.g.
// "--no-liftoff" to compile every wasm function optimized at once)
const JS_FLAGS = process.env.GS_Q3_JS_FLAGS ?? "";
// GS_Q3_SET: core settings applied before the timed runs, an experiment's
// knobs (e.g. "jit.budget=100,jit.threshold=100000")
const SETTINGS = (process.env.GS_Q3_SET ?? "")
  .split(",")
  .filter(Boolean)
  .map((s) => s.split("=") as [string, string]);
// GS_Q3_AFTER: calls logged after each run, "path:arg;path:arg" (e.g.
// "machine.cpu.mmu.translate:0x00afe2c0"), a probe of what the run left
const AFTER = (process.env.GS_Q3_AFTER ?? "")
  .split(";")
  .filter(Boolean)
  .map((s) => s.split(":") as [string, string]);

const DATA = path.resolve(__dirname, "../../../data");
const ROM = path.join(DATA, "roms", "pmg3dt-pmg3mt-78f57389.rom");
const PROM = path.join(DATA, "roms", "rage128-nexus128-103-35b433aa.prom");
const HD = path.join(
  DATA,
  "systems",
  "mac_os_9_2_1_516mb_g3_rage128_demos.dmg",
);
const CD = path.join(DATA, "apps", "quake3_1_30_demo_four_cd.iso");
const STORED_ROM = "/opfs/images/rom/78f57389-7b8375af2e19914d";
const OPFS_HD = "/opfs/upload/q3/mac_os_9_2_1_516mb_g3_rage128_demos.dmg";
const OPFS_CD = "/opfs/upload/q3/quake3_1_30_demo_four_cd.iso";
const OUT = path.resolve(__dirname, "../../test-results/perf-quake3-gpu");

// Console cells: colour 7 for the echo and the engine's lines, 3 for
// warnings
const cells = (colour: number, text: string) =>
  [...text]
    .map(
      (c) =>
        `${colour.toString(16).padStart(2, "0")} ${c.charCodeAt(0).toString(16).padStart(2, "0")}`,
    )
    .join(" ");
const PAT_ECHO = cells(7, "]\\demo four");
const PAT_FRAGLIMIT = cells(7, " hit the fraglimit");
const PAT_MISSING = cells(3, "could not find");

// Real GPU only (unless overridden): no SwiftShader flags here, unlike the
// suite's default launch options.
test.use({
  launchOptions: {
    args: [
      "--enable-unsafe-webgpu",
      "--ignore-gpu-blocklist",
      ...(PROFILE_S ? [`--remote-debugging-port=${CDP_PORT}`] : []),
      ...(JS_FLAGS ? [`--js-flags=${JS_FLAGS}`] : []),
    ],
  },
  headless: process.env.GS_Q3_HEADED !== "1",
  ...(process.env.GS_Q3_CHANNEL ? { channel: process.env.GS_Q3_CHANNEL } : {}),
});

type Sample = {
  instr: number;
  cycles: number;
  hz: number;
  wallNs: number;
  userNs: number;
  flips: number;
  prims: number;
  jit: number;
  ticks: number;
  gpu: Record<string, number>;
  nodeMs: number;
};

const num = (v: unknown): number => {
  if (typeof v === "number") return v;
  if (typeof v === "string" && /^-?\d+(\.\d+)?$/.test(v)) return Number(v);
  // A hex-flagged unsigned (an address from debug.find.*) is a JSON string
  if (typeof v === "string" && /^0x[0-9a-f]+$/i.test(v)) return Number(v);
  if (v && typeof v === "object" && "value" in (v as object))
    return num((v as { value: unknown }).value);
  return NaN;
};

test("Quake III demo four on the host GPU, timed", async ({ page }) => {
  test.skip(
    !ENABLED,
    "opt-in: GS_Q3_GPU=1 (see web2-specs/perf-quake3-gpu/README.md)",
  );
  for (const f of [ROM, PROM, HD, CD])
    test.skip(
      !fs.existsSync(f),
      `test data missing: ${f} (scripts/fetch-test-data.sh)`,
    );
  test.setTimeout(TIMEOUT_MIN * 60_000);
  fs.mkdirSync(OUT, { recursive: true });

  const log = (line: string) => console.log(`Q3GPU ${line}`);
  // The page's console (the emulator's output, the GPU worker's messages),
  // written as it comes, so a failed run keeps it
  const browserLog = path.join(OUT, "browser-console.log");
  fs.writeFileSync(browserLog, "");
  page.on("console", (m) =>
    fs.appendFileSync(browserLog, `${m.type()}: ${m.text()}\n`),
  );
  const ev = (p: string, a?: unknown[] | Record<string, unknown>) =>
    gsEvalInPage(page, p, a);
  const call = (p: string, a?: unknown[] | Record<string, unknown>) =>
    gsCallInPage(page, p, a);

  // The page's 3 s heartbeat watch marks a stalled emulator dead and takes
  // the page down ("The emulator stopped"); a measurement should ride a
  // stall out and report it.  Lift it to 600 s in the served bundle
  // (GS_Q3_KEEP_WATCHDOG=1 keeps it).
  if (process.env.GS_Q3_KEEP_WATCHDOG !== "1") {
    let patched = false;
    await page.route(/assets\/index-[^/]*\.js$/, async (route) => {
      const r = await route.fetch();
      const text = await r.text();
      const body = text.replace(
        /(\w)>=3&&\((\w)\(`emulator not responding/,
        "$1>=600&&($2(`emulator not responding",
      );
      patched = body !== text;
      await route.fulfill({ response: r, body });
    });
    page.once("load", () => log(`heartbeat watch lifted to 600 s: ${patched}`));
  }
  await gotoWeb2(page);
  // The core's perf lines (log.perf_every, on during each timed run), from
  // the console's print lines: the terminal keeps only the last few
  let perfFile = "";
  const printLog = path.join(OUT, "print.log"); // every console line
  fs.writeFileSync(printLog, "");
  await page.exposeFunction("__q3PrintLine", (line: string) => {
    fs.appendFileSync(printLog, line + "\n");
    if (perfFile && line.includes("perf:"))
      fs.appendFileSync(perfFile, line + "\n");
  });
  await page.evaluate(() => {
    const w = window as unknown as {
      __gsUiForTests?: { tapPrint(fn: (l: string) => void): void };
      __q3PrintLine(l: string): void;
    };
    w.__gsUiForTests?.tapPrint((l) => w.__q3PrintLine(l));
  });

  // The adapter: a real GPU, or skip
  const adapter = await page.evaluate(async () => {
    const gpu = (
      navigator as unknown as {
        gpu?: {
          requestAdapter(): Promise<{ info?: Record<string, string> } | null>;
        };
      }
    ).gpu;
    const a = gpu ? await gpu.requestAdapter() : null;
    if (!a) return null;
    const i = a.info ?? {};
    return {
      vendor: i.vendor ?? "",
      architecture: i.architecture ?? "",
      device: i.device ?? "",
      description: i.description ?? "",
    };
  });
  log(
    `adapter ${JSON.stringify(adapter)} userAgent=${JSON.stringify(await page.evaluate(() => navigator.userAgent))}`,
  );
  const software =
    !adapter ||
    /swiftshader|google/i.test(`${adapter.vendor} ${adapter.description}`);
  test.skip(
    RASTER === "webgpu" && software && !ALLOW_SOFTWARE_GPU,
    `no hardware WebGPU adapter (${JSON.stringify(adapter)}): this times SwiftShader, not a GPU`,
  );

  // The G3 ROM and the Rage 128's expansion ROM, through Load ROM...
  for (const [file, toast] of [
    [ROM, "pmg3dt-pmg3mt-78f57389.rom added"],
    [PROM, "PCI expansion ROM for 'rage128'"],
  ] as const) {
    const [chooser] = await Promise.all([
      page.waitForEvent("filechooser"),
      page.getByRole("button", { name: "Load ROM..." }).click(),
    ]);
    await chooser.setFiles(file);
    await expect(
      page.locator(".toast .msg").filter({ hasText: toast }),
    ).toBeVisible({ timeout: 120_000 });
  }
  await stageOpfsFileStreaming(page, OPFS_HD, HD);
  await stageOpfsFileStreaming(page, OPFS_CD, CD);

  const doc = {
    cards: [
      {
        slot: "pci_1",
        card: "rage128",
        options: { memory: "32m", raster: RASTER },
      },
    ],
    displays: { pci_1: { monitor: "13in_rgb" } },
  };
  await call("machine.boot", {
    model: "pmg3dt",
    ram: 262144,
    rom: STORED_ROM,
    config: JSON.stringify(doc),
  });
  await ev("scheduler.stop");
  await ev("checkpoint.auto", [false]);
  // No background checkpoint (the periodic save, the tab-hidden save) in
  // the timed runs
  const autoCheckpoint = await ev("checkpoint.auto");
  log(`checkpoint.auto=${JSON.stringify(autoCheckpoint)}`);
  expect(autoCheckpoint, "background checkpointing is off").toBe(false);
  const raster = await ev("machine.pci.slot[1].card.regs.raster");
  log(`raster=${JSON.stringify(raster)}`);
  expect(raster, `the Rage 128 draws with raster ${RASTER}`).toBe(RASTER);
  if (ENGINE === "interpreter") {
    await call("predecode.enabled", [0]);
  } else {
    await call("predecode.enabled", [1]);
    await call("jit.supervisor", [ENGINE === "jit" ? 1 : 0]);
    await call("jit.enabled", [ENGINE === "jit" ? 1 : 0]);
  }
  log(
    `engine=${ENGINE} predecode=${JSON.stringify(await ev("predecode.enabled"))} jit=${JSON.stringify(await ev("jit.enabled"))}`,
  );
  await call("machine.rtc.time", ["2026-10-01T12:00:00"]);
  await call("machine.scsi.attach_hd", [OPFS_HD, 0]);
  await call("machine.scsi.attach_cdrom", [OPFS_CD, 3]);
  // A backing-store failure shows as a SCSI MEDIUM ERROR to the guest ("a
  // disk error occurred"); the image log names it on the terminal
  await call("log.set", ["image", 1]);

  // The guest's own pace for the boot and Quake's start: turbo
  await call("scheduler.mode", ["turbo"]);
  await call("scheduler.run", []);
  const instr = async () => num(await ev("scheduler.instr_count"));
  // Wait until the guest has retired n more instructions.  An alert (a
  // disk write that failed, a hung guest...) is printed on the terminal,
  // which keeps only its last lines: save it as soon as the count rises.
  let alerts = 0;
  const waitInstr = async (n: number) => {
    const until = (await instr()) + n;
    while ((await instr()) < until) {
      await page.waitForTimeout(100);
      const a = num(await ev("log.alerts"));
      if (a > alerts) {
        alerts = a;
        log(`alert ${a} (terminal-alert${a}.txt)`);
        await saveTerminal(`terminal-alert${a}`);
      }
    }
  };
  const press = async (k: string | number) => {
    await call("machine.adb.keyboard.press", [k]);
  };
  const cmdKey = async (k: string) => {
    await call("machine.adb.keyboard.down", ["Command"]);
    await waitInstr(60e6);
    await call("machine.adb.keyboard.down", [k]);
    await waitInstr(60e6);
    await call("machine.adb.keyboard.up", [k]);
    await call("machine.adb.keyboard.up", ["Command"]);
  };
  const peekBytes = async (addr: number, len: number): Promise<Buffer> => {
    const r = await ev("machine.memory.peek.bytes", [addr, len]);
    if (typeof r === "string") return Buffer.from(r.replace(/^0x/, ""), "hex");
    if (Array.isArray(r)) return Buffer.from(r.map(num));
    throw new Error(`peek.bytes answered ${JSON.stringify(r)}`);
  };
  // The front application: CurApName, a Str31 low-memory global
  const curApp = async (): Promise<string> => {
    const b = await peekBytes(0x910, 32);
    return b.subarray(1, 1 + Math.min(b[0], 31)).toString("latin1");
  };
  // In turbo the guest's clock runs ahead of its instructions, so Energy
  // Saver can put the display to sleep between steps, and the key that
  // wakes it is lost.  Shift first: it wakes the display and types nothing.
  const wake = async () => {
    await press("shift");
    await waitInstr(300e6);
  };
  // The web app's terminal text (the emulator's own output)
  const saveTerminal = async (name: string) =>
    fs.writeFileSync(
      path.join(OUT, `${name}.txt`),
      await page
        .locator(".console-output")
        .first()
        .innerText()
        .catch((e: Error) => `(no terminal: ${e.message})`),
    );
  const shot = (name: string) =>
    page.screenshot({ path: path.join(OUT, `${name}.png`) });

  // The front window's title: WindowList (a low-memory global) points at
  // the front WindowRecord, whose titleHandle (offset 134) is a handle to a
  // Str255
  const frontWindow = async (): Promise<string> => {
    const u32 = async (a: number) => (await peekBytes(a, 4)).readUInt32BE(0);
    const w = await u32(0x9d6);
    if (!w) return "";
    const h = await u32(w + 134);
    if (!h) return "";
    const p = await u32(h);
    if (!p) return "";
    const b = await peekBytes(p, 64);
    return b.subarray(1, 1 + Math.min(b[0], 63)).toString("latin1");
  };
  const flips = async () =>
    num(await ev("machine.pci.slot[1].card.regs.flips"));
  // Run until `cond` holds, polling while the scheduler runs, for at most
  // maxS host seconds: a progress line every 15 s, and on a timeout a
  // screenshot and the terminal before the failure, so a stuck step costs
  // its limit and no more
  const waitUntil = async (
    what: string,
    cond: () => Promise<boolean>,
    maxS: number,
  ) => {
    const t0 = Date.now();
    const i0 = await instr();
    let next = t0 + 15_000;
    for (;;) {
      if (await cond()) {
        log(
          `${what}: after ${((Date.now() - t0) / 1000).toFixed(1)} s, ${((await instr()) - i0) / 1e9} G instructions`,
        );
        return;
      }
      const a = num(await ev("log.alerts"));
      if (a > alerts) {
        alerts = a;
        log(`alert ${a} (terminal-alert${a}.txt)`);
        await saveTerminal(`terminal-alert${a}`);
      }
      const now = Date.now();
      if (now - t0 > maxS * 1000) {
        const tag = what.replace(/\W+/g, "-");
        await shot(`timeout-${tag}`);
        await saveTerminal(`terminal-timeout-${tag}`);
        throw new Error(
          `${what}: not within ${maxS} s (front app ${JSON.stringify(await curApp())}, window ${JSON.stringify(await frontWindow())}, flips ${await flips()})`,
        );
      }
      if (now >= next) {
        next += 15_000;
        const di = (await instr()) - i0;
        log(
          `  waiting for ${what}: ${Math.round((now - t0) / 1000)} s, ${(di / 1e9).toFixed(1)} G instructions (${Math.round(di / (now - t0) / 1e3)} MIPS), app ${JSON.stringify(await curApp())}, window ${JSON.stringify(await frontWindow())}, flips ${await flips()}`,
        );
      }
      await page.waitForTimeout(250);
    }
  };
  // Select a Finder item by name and open it, until `done` holds; once
  // more if it did not take (a key lost to a waking display, an alert)
  const openUntil = async (
    name: string,
    what: string,
    done: () => Promise<boolean>,
    maxS: number,
  ) => {
    const attempt = async () => {
      await wake();
      await call("machine.adb.keyboard.type", [name]);
      await waitInstr(500e6);
      await cmdKey("o");
      await waitUntil(what, done, maxS);
    };
    try {
      await attempt();
    } catch (e) {
      log(`${(e as Error).message}: opening ${name} again`);
      await press("return"); // an alert takes the keys: dismiss it
      await waitInstr(1e9);
      await attempt();
    }
    // The new window's contents drawn before the next name is typed
    await waitInstr(500e6);
  };

  const tBoot = Date.now();
  await waitUntil("the Finder", async () => (await curApp()) === "Finder", 300);
  // The desktop's icons (the CD's among them) once the Finder is up
  await waitInstr(3e9);
  log(`desktop after ${Math.round((Date.now() - tBoot) / 1000)} s`);
  await shot("1-desktop");
  await openUntil(
    "Q3A",
    "the CD's window",
    async () => (await frontWindow()) === "Q3A",
    60,
  );
  await openUntil(
    "Quake3",
    "the Quake3 folder's window",
    async () => (await frontWindow()) === "Quake3",
    60,
  );
  const flips0 = await flips();
  await openUntil(
    "Quake3",
    "Quake3 in front",
    async () => (await curApp()) === "Quake3",
    90,
  );
  // Quake draws its CD key screen through the card: its page flips start
  await waitUntil(
    "Quake drawing",
    async () => (await flips()) - flips0 >= 10,
    180,
  );
  await waitInstr(2e9);
  await shot("2-cdkey");
  await wake();
  await press("tab");
  await waitInstr(600e6);
  await call("machine.adb.keyboard.down", ["return"]);
  await waitInstr(200e6);
  await call("machine.adb.keyboard.up", ["return"]);
  await waitInstr(4e9);
  await press("escape");
  await waitInstr(2e9);
  await shot("3-menu");
  log(`Quake's main menu after ${Math.round((Date.now() - tBoot) / 1000)} s`);

  // Quake's console: open it, type a line, Return
  const consoleLine = async (text: string) => {
    await call("machine.adb.keyboard.type", [text]);
    await waitInstr(1e9);
    await press("return");
    await waitInstr(1e9);
  };
  const find = async (
    pattern: string,
    lo = 0,
    hi = 0x10000000,
  ): Promise<number[]> => {
    const r = await ev("debug.find.bytes", [pattern, lo, hi]);
    return Array.isArray(r) ? r.map(num) : [];
  };
  // The console ring around the echo, as text lines
  const consoleText = async (lo: number, hi: number): Promise<string[]> => {
    const parts: Buffer[] = [];
    for (let a = lo; a < hi; a += 4096)
      parts.push(await peekBytes(a, Math.min(4096, hi - a)));
    const b = Buffer.concat(parts);
    let t = "";
    for (let i = 0; i + 1 < b.length; i += 2)
      t +=
        b[i] < 10 && b[i + 1] >= 32 && b[i + 1] < 127
          ? String.fromCharCode(b[i + 1])
          : "\0";
    return t
      .split(/\0+| {6,}/)
      .map((s) => s.trim())
      .filter((s) => s.length > 2);
  };
  const sample = async (): Promise<Sample> => {
    const gpu: Record<string, number> = {};
    const gs = await ev("machine.pci.slot[1].card.regs.gpu_stats");
    for (const m of String(gs ?? "").matchAll(/(\w+)=(-?\d+)/g))
      gpu[m[1]] = Number(m[2]);
    return {
      instr: await instr(),
      cycles: num(await ev("scheduler.cycles")),
      hz: num(await ev("scheduler.frequency")),
      wallNs: num(await ev("scheduler.host_wall_ns")),
      userNs: num(await ev("scheduler.host_user_ns")),
      flips: num(await ev("machine.pci.slot[1].card.regs.flips")),
      prims: num(await ev("machine.pci.slot[1].card.regs.prims3d")),
      jit: num(await ev("jit.instructions")),
      ticks: num(await ev("debug.mac.globals.read", ["Ticks"])),
      gpu,
      nodeMs: Date.now(),
    };
  };

  for (const [k, v] of SETTINGS) {
    await ev(k, [/^-?[\d.]+$/.test(v) ? Number(v) : v]);
    log(`set ${k}=${JSON.stringify(await ev(k))}`);
  }
  const cpi = num(await ev("scheduler.cpi"));
  const results: Record<string, unknown>[] = [];
  let run = 0;
  for (const mode of MODES) {
    run++;
    const marker = `gsdemoend${run}`;
    const patEnd = cells(7, marker);
    // The console, the end marker and the command, typed in turbo: a key
    // held for a count of instructions is a fixed guest time only at the
    // authentic CPI.  After a switch to paced or fast the governor starts
    // at its floor (0.25x), so the 60 M-instruction console key lasted about
    // 0.9 s of guest time, past Mac OS's key-repeat delay: the repeat
    // toggled the console shut again, and the typed line's Return chose
    // menu items (Single Player, Choose Level).
    await call("scheduler.mode", ["turbo"]);
    const setLine = `set nextdemo "echo ${marker}"`;
    await wake();
    await call("machine.adb.keyboard.down", [0x32]);
    await waitInstr(60e6);
    await call("machine.adb.keyboard.up", [0x32]);
    await waitInstr(1.5e9);
    await consoleLine(setLine);
    const setEchoed = (await find(cells(7, `]\\${setLine}`))).length > 0;
    if (!setEchoed) await shot(`console-miss-run${run}`);
    expect(setEchoed, "the console took the set line").toBe(true);
    if (TIMEDEMO) await consoleLine("timedemo 1");
    await call("machine.adb.keyboard.type", ["demo four"]);
    await waitInstr(500e6);
    // The run's pacing, and a moment for its governor to climb off its
    // floor before the start
    await call("scheduler.mode", [mode === "fast" ? "accelerated" : mode]);
    await page.waitForTimeout(3000);

    // The start: paused, the counters, Return queued, resumed
    const echoesBefore = new Set(await find(PAT_ECHO));
    const fragBefore = new Set(await find(PAT_FRAGLIMIT));
    await call("scheduler.stop");
    perfFile = path.join(OUT, `perf-run${run}-${mode}.log`);
    fs.writeFileSync(perfFile, "");
    await call("log.perf_every", [1]);
    const s0 = await sample();
    await press("return");
    await call("scheduler.run", []);

    // The console: the new echo of the command
    // (a search of all RAM: once a second, so the search does not take the
    // emulator thread's time from the run it measures; a slowed guest can
    // take many seconds to echo)
    let echo = -1;
    for (let i = 0; i < 60 && echo < 0; i++) {
      await page.waitForTimeout(1000);
      echo = (await find(PAT_ECHO)).find((a) => !echoesBefore.has(a)) ?? -1;
    }
    expect(echo, "the console echoes `demo four`").toBeGreaterThan(0);
    const lo = echo - 0x10000;
    const hi = echo + 0x10000;

    // The demo's last event, then its end
    const ended = async () => {
      for (const a of await find(patEnd, lo, hi)) {
        const next = await peekBytes(a + 2 * marker.length, 2);
        if (next[1] !== 0x22) return true; // not the echoed `set ... "echo <marker>"`
      }
      return false;
    };
    const deadline = Date.now() + TIMEOUT_MIN * 60_000;
    let fragAt = 0;
    // GS_Q3_SWEEP: the governor's floor stepped through these speeds,
    // SWEEP_S seconds each, so the perf lines show a frame-unit's cost at
    // each (the governor does not climb off a floor its projection says
    // is full)
    const tSweep = Date.now();
    let sweepAt = -1;
    let profiling: Promise<void> | null = null;
    while (Date.now() < deadline) {
      await page.waitForTimeout(POLL_MS);
      if (PROFILE_S && !profiling && Date.now() - tSweep >= PROFILE_AT * 1000) {
        log(`profiling the workers for ${PROFILE_S} s`);
        profiling = (async () => {
          const prof = await WorkerProfiler.connect(CDP_PORT, new URL(page.url()).host);
          await new Promise((r) => setTimeout(r, 500)); // the auto-attaches land
          const profiles = await prof.profile(PROFILE_S * 1000);
          prof.close();
          let k = 0;
          for (const [name, p] of profiles) {
            k++;
            const { lines, busyMs } = summarize(p);
            const base = path.join(OUT, `profile-run${run}-${mode}-w${k}`);
            fs.writeFileSync(`${base}.cpuprofile`, JSON.stringify(p));
            fs.writeFileSync(`${base}.txt`, [name, ...lines].join("\n") + "\n");
            log(`profile w${k} busy ${busyMs.toFixed(0)} ms: ${name}`);
          }
        })().catch((e) => log(`profiling failed: ${(e as Error).message}`));
      }
      if (SWEEP.length) {
        const k = Math.floor((Date.now() - tSweep) / (SWEEP_S * 1000));
        if (k !== sweepAt && k <= SWEEP.length) {
          sweepAt = k;
          const floor = k < SWEEP.length ? SWEEP[k] : 0.25;
          await ev("scheduler.min_speed", [floor]);
          log(`sweep: min_speed ${floor}`);
          if (perfFile) fs.appendFileSync(perfFile, `sweep: min_speed ${floor}\n`);
        }
      }
      if (
        !fragAt &&
        (await find(PAT_FRAGLIMIT, lo, hi)).some((a) => !fragBefore.has(a))
      )
        fragAt = Date.now();
      if (fragAt && (await ended())) break;
    }
    await call("scheduler.stop");
    if (profiling) await profiling;
    const s1 = await sample();
    await call("log.perf_every", [0]);
    perfFile = "";
    for (const [k, a] of AFTER)
      log(`after: ${k}(${a}) = ${JSON.stringify(await ev(k, a ? [Number(a)] : [])).slice(0, 400)}`);
    // jit.trace=2 collected the exit histogram over the run: print it
    if (SETTINGS.some(([k, v]) => k === "jit.trace" && v === "2")) {
      await ev("jit.trace", [3]);
      await ev("jit.trace", [2]);
    }
    expect(fragAt, "the demo reached its fraglimit").toBeGreaterThan(0);
    expect(await ended(), "Quake ran nextdemo at the demo's end").toBe(true);

    const lines = await consoleText(lo, hi);
    fs.writeFileSync(
      path.join(OUT, `console-run${run}-${mode}.txt`),
      lines.join("\n") + "\n",
    );
    await shot(`4-end-run${run}-${mode}`);
    const missing = (await find(PAT_MISSING, lo, hi)).length;
    const timedemo = lines
      .map((l) => l.match(/(\d+) frames, ([\d.]+) seconds: ([\d.]+) fps/))
      .find(Boolean);

    const guestS = (s1.cycles - s0.cycles) / s0.hz;
    const hostS = (s1.wallNs - s0.wallNs) / 1e9;
    const nodeS = (s1.nodeMs - s0.nodeMs) / 1000;
    const di = s1.instr - s0.instr;
    const frames = s1.flips - s0.flips;
    const dg = (k: string) => (s1.gpu[k] ?? 0) - (s0.gpu[k] ?? 0);
    const r = {
      label: LABEL,
      run,
      mode,
      engine: ENGINE,
      raster: RASTER,
      timedemo: TIMEDEMO,
      adapter,
      host_wall_s: +hostS.toFixed(3),
      node_wall_s: +nodeS.toFixed(3),
      host_cpu_s: +((s1.userNs - s0.userNs) / 1e9).toFixed(3),
      guest_s: +guestS.toFixed(3),
      guest_ticks_s: +((s1.ticks - s0.ticks) / 60).toFixed(3),
      speed_x: +(guestS / hostS).toFixed(3),
      // The guest CPU against the original's: instructions at the
      // authentic CPI over the cycles that passed (the governor's average
      // multiplier in paced and accelerated, 1 in turbo)
      cpu_x: +((di * cpi) / (s1.cycles - s0.cycles)).toFixed(3),
      instructions: di,
      mips_guest: +(di / guestS / 1e6).toFixed(1),
      mips_host: +(di / hostS / 1e6).toFixed(1),
      jit_pct: +((100 * (s1.jit - s0.jit)) / di).toFixed(1),
      frames,
      fps_guest: +(frames / guestS).toFixed(2),
      fps_host: +(frames / hostS).toFixed(2),
      prims3d: s1.prims - s0.prims,
      gpu_presents: dg("presents"),
      gpu_presents_per_s: +(dg("presents") / hostS).toFixed(2),
      gpu_batches: dg("batches"),
      gpu_draws: dg("draws"),
      gpu_readback_bands: dg("readback_bands"),
      gpu_upload_rows: dg("upload_rows"),
      gpu_tex_upload_mb: +(dg("tex_upload_bytes") / 2 ** 20).toFixed(1),
      gpu_fallbacks: Object.keys(s1.gpu)
        .filter((k) => k.startsWith("fallback_"))
        .reduce((n, k) => n + dg(k), 0),
      fraglimit_to_end_s: +((s1.nodeMs - fragAt) / 1000).toFixed(3),
      end_resolution_ms: POLL_MS,
      missing_files: missing,
      quake_timedemo: timedemo
        ? { frames: +timedemo[1], seconds: +timedemo[2], fps: +timedemo[3] }
        : null,
    };
    results.push(r);
    log(
      Object.entries(r)
        .filter(([k]) => k !== "adapter")
        .map(
          ([k, v]) => `${k}=${typeof v === "object" ? JSON.stringify(v) : v}`,
        )
        .join(" "),
    );
    expect(missing, "every file the demo loads is on the CD").toBe(0);
    expect(frames, "Quake drew frames").toBeGreaterThan(500);
    // Back to the main menu for the next run, if there is one
    if (run < MODES.length) {
      await call("scheduler.run", []);
      await waitInstr(2e9);
    }
  }

  fs.writeFileSync(
    path.join(OUT, "perf-quake3-gpu.json"),
    JSON.stringify(results, null, 2) + "\n",
  );
  log(`wrote ${path.join(OUT, "perf-quake3-gpu.json")}`);
});
