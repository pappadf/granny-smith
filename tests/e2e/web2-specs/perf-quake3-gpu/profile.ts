// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// CPU profiles of a page's workers (the emulator's pthreads, the GPU
// worker), for perf-quake3-gpu's GS_Q3_PROFILE.  Playwright opens CDP
// sessions on pages only, so this talks CDP itself over the browser's
// remote-debugging port: attach to the page, auto-attach its workers (and
// theirs), and run the sampling profiler on each.  Names are the wasm
// module's own with a build linked with --profiling-funcs.

type Msg = {
  id?: number;
  method?: string;
  params?: Record<string, unknown>;
  result?: Record<string, unknown>;
  error?: { message: string };
  sessionId?: string;
};

type ProfileNode = {
  id: number;
  callFrame: { functionName: string; url: string };
  children?: number[];
};
export type CpuProfile = {
  nodes: ProfileNode[];
  samples: number[];
  timeDeltas: number[];
  startTime: number;
  endTime: number;
};

export class WorkerProfiler {
  private ws!: WebSocket;
  private nextId = 1;
  private pending = new Map<number, (m: Msg) => void>();
  readonly workers = new Map<string, string>(); // sessionId -> url

  static async connect(port: number, pageUrlPart: string): Promise<WorkerProfiler> {
    const p = new WorkerProfiler();
    const version = (await (await fetch(`http://127.0.0.1:${port}/json/version`)).json()) as {
      webSocketDebuggerUrl: string;
    };
    p.ws = new WebSocket(version.webSocketDebuggerUrl);
    await new Promise<void>((ok, fail) => {
      p.ws.onopen = () => ok();
      p.ws.onerror = () => fail(new Error("CDP websocket did not open"));
    });
    p.ws.onmessage = (e) => p.onMessage(JSON.parse(String(e.data)) as Msg);
    const targets = (await p.send("Target.getTargets")).targetInfos as {
      targetId: string;
      type: string;
      url: string;
    }[];
    const page = targets.find((t) => t.type === "page" && t.url.includes(pageUrlPart));
    if (!page) throw new Error(`no page target with ${pageUrlPart}`);
    const att = await p.send("Target.attachToTarget", { targetId: page.targetId, flatten: true });
    await p.autoAttach(att.sessionId as string);
    return p;
  }

  private async autoAttach(sessionId: string) {
    await this.send(
      "Target.setAutoAttach",
      { autoAttach: true, waitForDebuggerOnStart: false, flatten: true },
      sessionId,
    );
  }

  private onMessage(m: Msg) {
    if (m.id !== undefined) {
      this.pending.get(m.id)?.(m);
      this.pending.delete(m.id);
      return;
    }
    if (m.method === "Target.attachedToTarget") {
      const sid = m.params!.sessionId as string;
      const info = m.params!.targetInfo as { type: string; url: string };
      if (info.type === "worker") {
        this.workers.set(sid, info.url);
        // a worker's own workers (pthreads started from a pthread)
        void this.autoAttach(sid).catch(() => {});
      }
    } else if (m.method === "Target.detachedFromTarget") {
      this.workers.delete(m.params!.sessionId as string);
    }
  }

  send(method: string, params: Record<string, unknown> = {}, sessionId?: string) {
    const id = this.nextId++;
    this.ws.send(JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) }));
    return new Promise<Record<string, unknown>>((ok, fail) => {
      this.pending.set(id, (m) => (m.error ? fail(new Error(`${method}: ${m.error.message}`)) : ok(m.result ?? {})));
    });
  }

  // Profile every attached worker for `ms`, sampling every `intervalUs`
  async profile(ms: number, intervalUs = 200): Promise<Map<string, CpuProfile>> {
    const sids = [...this.workers.keys()];
    for (const sid of sids) {
      await this.send("Profiler.enable", {}, sid);
      await this.send("Profiler.setSamplingInterval", { interval: intervalUs }, sid);
      await this.send("Profiler.start", {}, sid);
    }
    await new Promise((r) => setTimeout(r, ms));
    const out = new Map<string, CpuProfile>();
    for (const sid of sids) {
      const r = await this.send("Profiler.stop", {}, sid).catch(() => null);
      if (r) out.set(`${sid.slice(0, 8)} ${this.workers.get(sid) ?? ""}`, r.profile as CpuProfile);
    }
    return out;
  }

  close() {
    this.ws.close();
  }
}

// Self and total time per function, the busiest first, as text lines
export function summarize(p: CpuProfile, top = 40): { lines: string[]; busyMs: number } {
  const byId = new Map(p.nodes.map((n) => [n.id, n]));
  const parent = new Map<number, number>();
  for (const n of p.nodes) for (const c of n.children ?? []) parent.set(c, n.id);
  const self = new Map<string, number>();
  const total = new Map<string, number>();
  let busy = 0;
  let all = 0;
  for (let i = 0; i < p.samples.length; i++) {
    const dt = (p.timeDeltas[i] ?? 0) / 1000;
    all += dt;
    const n = byId.get(p.samples[i]);
    if (!n) continue;
    const name = n.callFrame.functionName || "(anonymous)";
    if (name !== "(idle)" && name !== "(program)") busy += dt;
    self.set(name, (self.get(name) ?? 0) + dt);
    const seen = new Set<string>();
    for (let id: number | undefined = n.id; id !== undefined; id = parent.get(id)) {
      const f = byId.get(id)!.callFrame.functionName || "(anonymous)";
      if (seen.has(f)) continue;
      seen.add(f);
      total.set(f, (total.get(f) ?? 0) + dt);
    }
  }
  const lines = [`sampled ${all.toFixed(0)} ms, busy ${busy.toFixed(0)} ms`, "self ms  self%  total ms  function"];
  for (const [f, ms] of [...self].sort((a, b) => b[1] - a[1]).slice(0, top))
    lines.push(
      `${ms.toFixed(1).padStart(7)}  ${((100 * ms) / all).toFixed(1).padStart(5)}  ${(total.get(f) ?? 0).toFixed(1).padStart(8)}  ${f}`,
    );
  lines.push("", "by total (inclusive):");
  for (const [f, ms] of [...total].sort((a, b) => b[1] - a[1]).slice(0, top))
    lines.push(`${ms.toFixed(1).padStart(8)}  ${((100 * ms) / all).toFixed(1).padStart(5)}  ${f}`);
  return { lines, busyMs: busy };
}
