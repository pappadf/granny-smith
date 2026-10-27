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

const ENABLED = process.env.GS_Q3_GPU === "1";
const ALLOW_SOFTWARE_GPU = process.env.GS_Q3_ALLOW_SOFTWARE_GPU === "1";
const MODES = (process.env.GS_Q3_MODES ?? "paced,turbo")
  .split(",")
  .map((m) => m.trim())
  .filter(Boolean);
const ENGINE = process.env.GS_Q3_ENGINE ?? "jit"; // jit | predecoded | interpreter
const POLL_MS = Number(process.env.GS_Q3_POLL_MS ?? 100);
const TIMEDEMO = process.env.GS_Q3_TIMEDEMO === "1";
const TIMEOUT_MIN = Number(process.env.GS_Q3_TIMEOUT_MIN ?? 90);
const LABEL = process.env.GS_Q3_LABEL ?? "unlabeled";

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
  launchOptions: { args: ["--enable-unsafe-webgpu", "--ignore-gpu-blocklist"] },
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
  const browserLog: string[] = [];
  page.on("console", (m) => browserLog.push(`${m.type()}: ${m.text()}`));
  const ev = (p: string, a?: unknown[] | Record<string, unknown>) =>
    gsEvalInPage(page, p, a);
  const call = (p: string, a?: unknown[] | Record<string, unknown>) =>
    gsCallInPage(page, p, a);

  await gotoWeb2(page);

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
    software && !ALLOW_SOFTWARE_GPU,
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
        options: { memory: "32m", raster: "webgpu" },
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
  const raster = await ev("machine.pci.slot[1].card.regs.raster");
  log(`raster=${JSON.stringify(raster)}`);
  expect(raster, "the Rage 128 draws on the GPU (raster webgpu)").toBe(
    "webgpu",
  );
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

  // The guest's own pace for the boot and Quake's start: turbo
  await call("scheduler.mode", ["turbo"]);
  await call("scheduler.run", []);
  const instr = async () => num(await ev("scheduler.instr_count"));
  // Wait until the guest has retired n more instructions
  const waitInstr = async (n: number) => {
    const until = (await instr()) + n;
    while ((await instr()) < until) await page.waitForTimeout(100);
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
    await waitInstr(2e9);
  };
  const open = async (name: string, run: number) => {
    await call("machine.adb.keyboard.type", [name]);
    await waitInstr(1.5e9);
    await cmdKey("o");
    await waitInstr(run);
  };
  const shot = (name: string) =>
    page.screenshot({ path: path.join(OUT, `${name}.png`) });

  const tBoot = Date.now();
  await waitInstr(30e9);
  log(`desktop after ${Math.round((Date.now() - tBoot) / 1000)} s`);
  await shot("1-desktop");
  await open("Q3A", 6e9);
  await open("Quake3", 6e9);
  await open("Quake3", 32e9);
  await shot("2-cdkey");
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
  const peekBytes = async (addr: number, len: number): Promise<Buffer> => {
    const r = await ev("machine.memory.peek.bytes", [addr, len]);
    if (typeof r === "string") return Buffer.from(r.replace(/^0x/, ""), "hex");
    if (Array.isArray(r)) return Buffer.from(r.map(num));
    throw new Error(`peek.bytes answered ${JSON.stringify(r)}`);
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

  const results: Record<string, unknown>[] = [];
  let run = 0;
  for (const mode of MODES) {
    run++;
    const marker = `gsdemoend${run}`;
    const patEnd = cells(7, marker);
    await call("scheduler.mode", [mode === "fast" ? "accelerated" : mode]);
    await call("machine.adb.keyboard.down", [0x32]);
    await waitInstr(60e6);
    await call("machine.adb.keyboard.up", [0x32]);
    await waitInstr(1.5e9);
    await consoleLine(`set nextdemo "echo ${marker}"`);
    if (TIMEDEMO) await consoleLine("timedemo 1");
    await call("machine.adb.keyboard.type", ["demo four"]);
    await waitInstr(500e6);

    // The start: paused, the counters, Return queued, resumed
    const echoesBefore = new Set(await find(PAT_ECHO));
    const fragBefore = new Set(await find(PAT_FRAGLIMIT));
    await call("scheduler.stop");
    const s0 = await sample();
    await press("return");
    await call("scheduler.run", []);

    // The console: the new echo of the command
    let echo = -1;
    for (let i = 0; i < 100 && echo < 0; i++) {
      await page.waitForTimeout(100);
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
    while (Date.now() < deadline) {
      await page.waitForTimeout(POLL_MS);
      if (
        !fragAt &&
        (await find(PAT_FRAGLIMIT, lo, hi)).some((a) => !fragBefore.has(a))
      )
        fragAt = Date.now();
      if (fragAt && (await ended())) break;
    }
    await call("scheduler.stop");
    const s1 = await sample();
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
      timedemo: TIMEDEMO,
      adapter,
      host_wall_s: +hostS.toFixed(3),
      node_wall_s: +nodeS.toFixed(3),
      host_cpu_s: +((s1.userNs - s0.userNs) / 1e9).toFixed(3),
      guest_s: +guestS.toFixed(3),
      guest_ticks_s: +((s1.ticks - s0.ticks) / 60).toFixed(3),
      speed_x: +(guestS / hostS).toFixed(3),
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
    // Back to the main menu for the next run
    await call("scheduler.run", []);
    await waitInstr(2e9);
  }

  fs.writeFileSync(
    path.join(OUT, "perf-quake3-gpu.json"),
    JSON.stringify(results, null, 2) + "\n",
  );
  fs.writeFileSync(
    path.join(OUT, "browser-console.log"),
    browserLog.join("\n") + "\n",
  );
  log(`wrote ${path.join(OUT, "perf-quake3-gpu.json")}`);
});
