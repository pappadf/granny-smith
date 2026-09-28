# Execution model: the browser measurements (2026-09-27)

The execution-model work moved two things off the emulator thread that
used to stall it: the background checkpoint's write, and every host copy.
This note records what that bought, measured in a real browser on the
build that carries the change ("after") and on `main` just before it
("before"), with the same two specs on both:

- `tests/e2e/web2-specs/checkpoint-stall.spec.ts` — a IIcx with a 20 MB
  disk attached, running live under the RAF loop with `checkpoint.auto`
  on (a background checkpoint every 900 ticks, about every 15 s), probed
  for 50 s.
- `tests/e2e/web2-specs/copy-jitter.spec.ts` — a IIcx (8 MB) running live
  while `storage.cp` copies a 192 MB image inside OPFS; probed for 8 s
  before the copy and 20 s during it.

Both specs need a measurement build (`VITE_GS_MEASURE=1 make ui2`), which
exposes the page's `gsEval` as `window.__gsEval`; the probe is the round
trip of `machine.cpu.pc` issued every ~10 ms from the page. A request is
served at the next frame boundary, so the round trip of a healthy running
machine is one frame (p50 ≈ 6.5 ms here); a stall of the emulator thread
shows as one long round trip. Numbers are milliseconds, one run each,
headless Chromium in the CI dev container (software rendering, 8 vCPUs).

## Background checkpoint

| Machine | Build | p50 | p99 | worst | > 50 ms | > 200 ms |
|---|---|---|---|---|---|---|
| IIcx 32 MB | before | 6.5 | 10.6 | **262** | 4 | 2 |
| IIcx 32 MB | after | 6.5 | 13.3 | **60** | 2 | 0 |
| IIcx 128 MB | before | 6.5 | 9.8 | **796** | 4 | 3 |
| IIcx 128 MB | after | 6.5 | 11.2 | **184** | 2 | 0 |

About three checkpoints landed in each 50 s window. Before, each one
serialised the machine and wrote the file inside the tick: a quarter of a
second on 32 MB, most of a second on 128 MB, during which no frame ran and
no request was answered. After, the tick still serialises into the quick
buffer (that is what remains of the stall: 60 ms and 184 ms, roughly the
memcpy of the machine's RAM) and hands the buffer to the I/O worker, which
writes and renames it while the machine runs on. The proposal's gate was
no frame gap above 50 ms; the 128 MB serialisation alone is above it, so
the remaining stall is the copy of RAM into the quick buffer, not the
write. Splitting that copy across ticks is the next step if it matters.

## A large copy while the machine runs

| Build | copy of 192 MB | round trip before | round trip during (p99 / worst) |
|---|---|---|---|
| before | 20.2 s | 6.8 / 24.8 | 8.6 / **7488** |
| after | 20.2 s | 6.5 / 16.5 | 9.3 / **17.3** |

Before, `storage.cp` ran inside the leaf on the emulator thread: the
machine froze for the copy, and the probe's worst round trip is the copy
(7.5 s here, the remainder having been absorbed by the request that was
already in flight when the copy started). After, the copy is an I/O job:
the worst round trip during the copy is 17 ms, and the core's own
per-second `perf` events over the same window show the longest tick at
2–7 ms (one at 11.7 ms) with the guest's MIPS going from 3.9 to 3.1 while
the copy ran — a 20 % dip, at the edge of what the proposal's gate allowed,
from the copy's contention for the OPFS proxy thread and for the host's
cores. Copy throughput was the same on both builds (about 10 MB/s, bound
by OPFS through WasmFS), so the worker costs nothing in wall time.

## What was not measured

- The chunk-size sweep (256 KB / 1 MB / 4 MB, `GS_IO_CHUNK_KB`): one size,
  1 MB, was measured. The knob exists for the sweep.
- The 600 MB copy and the Marathon-class desktop row: a 192 MB copy on a
  ROM-only IIcx stands in. The mechanism is the same; the numbers would be
  longer, not different in kind.
- Worker spin-up time: both threads are created at boot, where nothing
  waits on them, so there is nothing to observe from the page.
- Guest disk-read latency under the copy: the IIcx here has no disk
  activity of its own; the tick-length distribution above is the proxy.

## Reproducing

```
# after: this checkout
cd tests/e2e && VITE_GS_MEASURE=1 GS_MEASURE_OUT=/tmp/after.jsonl \
  npx playwright test -c playwright.web2.config.ts \
  web2-specs/checkpoint-stall.spec.ts web2-specs/copy-jitter.spec.ts
# before: a worktree of the commit under comparison, the two specs copied
# in and window.__gsEval exposed the same way, then the same command.
```
