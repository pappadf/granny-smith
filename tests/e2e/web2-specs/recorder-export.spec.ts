// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// web2 e2e: Export MP4 of a finished recording (app/web2/src/recorder/).
//
// Where the browser has no H.264 encoder, the action is disabled and says
// why: in the finished recording's toast and on the .mkv's context menu in
// the Filesystem panel (simulated here by removing WebCodecs' encoders
// before the page loads).  Where it has one, a short Plus recording is
// exported through the dialog: the worker reads the .mkv from OPFS, the
// decoder module decodes it, WebCodecs encodes it, and the .mp4 lands
// beside the recording.  The file is downloaded, checked with ffprobe when
// it is installed (clean decode, constant 1203/20 fps, every frame, the
// sound as long as the picture), and played in the page's own <video>.
// Playwright's Chromium has an H.264 encoder but no AAC one, so the sound
// is Opus there; a branded Chrome or Edge (or Safari) gives AAC.
// See docs/internals/core/record/record.md, "MP4 export".

import { test, expect, type Page } from "../helpers/test";
import * as path from "node:path";
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { gsCallInPage, gsEvalInPage } from "../helpers/web2-eval";
import { bootPlus, recordFrames, startFromToolbar } from "../helpers/recorder";
import { row } from "../helpers/web2-fs";

// What the page's WebCodecs says about the export's configurations.
async function encoders(
  page: Page,
): Promise<{ h264: boolean; aac: boolean; opus: boolean }> {
  return page.evaluate(async () => {
    const w = window as unknown as {
      VideoEncoder?: typeof VideoEncoder;
      AudioEncoder?: typeof AudioEncoder;
    };
    const v = async (c: VideoEncoderConfig) =>
      !!w.VideoEncoder &&
      (await w.VideoEncoder.isConfigSupported(c)).supported === true;
    const a = async (c: AudioEncoderConfig) =>
      !!w.AudioEncoder &&
      (await w.AudioEncoder.isConfigSupported(c)).supported === true;
    const h264 =
      (await v({ codec: "avc1.64001f", width: 512, height: 342 })) ||
      (await v({
        codec: "avc1.64001f",
        width: 512,
        height: 342,
        hardwareAcceleration: "prefer-software",
      }));
    return {
      h264,
      aac: await a({
        codec: "mp4a.40.2",
        sampleRate: 48000,
        numberOfChannels: 1,
        bitrate: 128000,
      }),
      opus: await a({
        codec: "opus",
        sampleRate: 48000,
        numberOfChannels: 1,
        bitrate: 128000,
      }),
    };
  });
}

// An OPFS file's bytes, read by the page.
async function opfsBytes(page: Page, file: string): Promise<number[]> {
  return page.evaluate(async (file) => {
    const rel = file.replace(/^\/opfs\/?/, "").split("/");
    const name = rel.pop() as string;
    let dir = await navigator.storage.getDirectory();
    for (const part of rel) dir = await dir.getDirectoryHandle(part);
    return Array.from(
      new Uint8Array(
        await (await (await dir.getFileHandle(name)).getFile()).arrayBuffer(),
      ),
    );
  }, file);
}

// ffmpeg's decoding of `file` with output options `args`, as bytes.
function decode(file: string, args: string[]): Buffer {
  const r = spawnSync("ffmpeg", ["-v", "error", "-i", file, ...args, "-"], {
    maxBuffer: 1 << 30,
  });
  if (r.status !== 0) throw new Error(`ffmpeg: ${r.stderr?.toString()}`);
  return r.stdout as Buffer;
}

// The first moment a mono s16 stream at 48 kHz exceeds a quarter of its peak, in seconds.
function onset(b: Buffer): number {
  const s = new Int16Array(b.buffer, b.byteOffset, b.length / 2);
  let peak = 0;
  for (const v of s) peak = Math.max(peak, Math.abs(v));
  for (let i = 0; i < s.length; i++)
    if (Math.abs(s[i]) > peak / 4) return i / 48000;
  return -1;
}

// Record about `frames` frames and stop; the recording's path and frame count.
async function recordAndStop(page: Page, frames: number) {
  const file = await startFromToolbar(page);
  await recordFrames(page, frames);
  await page.locator(".record-btn").click();
  const toast = page.locator(".toast", { hasText: "Recording saved" });
  await expect(toast).toBeVisible({ timeout: 30_000 });
  return {
    file,
    frames: (await gsEvalInPage(page, "recorder.frames")) as number,
    toast,
  };
}

test("Export MP4 is disabled with the reason where the browser cannot encode H.264", async ({
  page,
}) => {
  test.setTimeout(180_000);
  // A browser without WebCodecs' encoders (as Firefox once was, or a
  // Chromium built without H.264)
  await page.addInitScript(() => {
    const w = window as unknown as Record<string, unknown>;
    delete w.VideoEncoder;
    delete w.AudioEncoder;
  });
  await bootPlus(page);
  const { file, toast } = await recordAndStop(page, 60);

  const action = toast.getByRole("button", { name: "Export MP4…" });
  await expect(action).toBeDisabled();
  await expect(action).toHaveAttribute("title", /cannot encode H\.264/);
  await expect(action).toHaveAttribute("title", /ffmpeg -i/);

  // The same on the recording's context menu, reached through Show in Files
  await toast.getByRole("button", { name: "Show in Files" }).click();
  const name = path.basename(file);
  await row(page, name).first().waitFor({ state: "visible" });
  await row(page, name).first().click({ button: "right" });
  const item = page.locator(".context-menu .item", { hasText: "Export MP4…" });
  await expect(item).toHaveAttribute("aria-disabled", "true");
  await expect(item).toHaveAttribute("title", /cannot encode H\.264/);
  // A click on it does nothing (Playwright's click would wait for it to be enabled)
  await item.dispatchEvent("click");
  await expect(page.locator(".export-body")).toHaveCount(0);
});

test("Export MP4 makes an MP4 that ffprobe and the page’s <video> accept", async ({
  page,
}, testInfo) => {
  test.setTimeout(240_000);
  // Recording from before the first instruction, so the boot beep is in it
  // (the sound's timing is then checkable)
  await bootPlus(page, async () => {
    expect(await gsCallInPage(page, "recorder.start", [])).toMatch(/\.mkv$/);
  });
  const enc = await encoders(page);
  test.skip(
    !enc.h264,
    "this browser has no WebCodecs H.264 encoder (a real export needs one)",
  );

  await expect(page.getByTestId("recording")).toBeVisible({ timeout: 15_000 });
  const file = (await gsEvalInPage(page, "recorder.path")) as string;
  await recordFrames(page, 180);
  await page.locator(".record-btn").click();
  const toast = page.locator(".toast", { hasText: "Recording saved" });
  await expect(toast).toBeVisible({ timeout: 30_000 });
  const frames = (await gsEvalInPage(page, "recorder.frames")) as number;
  await toast.getByRole("button", { name: "Export MP4…" }).click();

  // The dialog: the native size by default, the sound it will get
  const dialog = page.locator(".export-body");
  await expect(dialog).toHaveAttribute("data-phase", "choose", {
    timeout: 30_000,
  });
  await expect(page.getByLabel(/1× — 512 × 342/)).toBeChecked();
  if (enc.aac)
    await expect(dialog.locator(".export-audio")).toContainText("AAC");
  else if (enc.opus)
    await expect(dialog.locator(".export-audio")).toContainText("Opus");
  else
    await expect(dialog.locator(".export-audio")).toContainText("Video only");
  await page.getByRole("button", { name: "Export", exact: true }).click();

  const done = page.locator(".toast", { hasText: "Exported:" });
  await expect(done).toBeVisible({ timeout: 120_000 });
  const mp4 = file.replace(/\.mkv$/, ".mp4");
  await expect(done).toContainText(path.basename(mp4));

  // Download it, through the core's download path (the core sees the
  // worker's new OPFS file)
  const download = page.waitForEvent("download");
  await done.getByRole("button", { name: "Download" }).click();
  const d = await download;
  expect(d.suggestedFilename()).toBe(path.basename(mp4));
  const saved = testInfo.outputPath(path.basename(mp4));
  await d.saveAs(saved);
  const bytes = fs.readFileSync(saved);
  expect(bytes.subarray(4, 8).toString("latin1")).toBe("ftyp");
  // The index first: moov before mdat, so it streams
  expect(bytes.indexOf("moov")).toBeLessThan(bytes.indexOf("mdat"));

  const seconds = (frames * 20) / 1203;
  const ffprobe = spawnSync("ffprobe", ["-version"]);
  if (ffprobe.status === 0) {
    const warn = spawnSync(
      "ffmpeg",
      ["-v", "warning", "-i", saved, "-f", "null", "-"],
      {
        encoding: "utf8",
      },
    );
    expect(warn.status).toBe(0);
    expect(warn.stderr.trim()).toBe("");
    const p = spawnSync(
      "ffprobe",
      ["-v", "error", "-of", "json", "-show_streams", "-count_frames", saved],
      { encoding: "utf8" },
    );
    const streams = (
      JSON.parse(p.stdout) as { streams: Record<string, string>[] }
    ).streams;
    const v = streams.find((s) => s.codec_type === "video")!;
    expect(v.codec_name).toBe("h264");
    expect(v.r_frame_rate).toBe("1203/20");
    expect(v.avg_frame_rate).toBe("1203/20");
    expect(Number(v.nb_read_frames)).toBe(frames);
    expect(Number(v.duration)).toBeCloseTo(seconds, 2);
    const a = streams.find((s) => s.codec_type === "audio");
    // The recording itself, as ffmpeg decodes it (it reads ZMBV and FLAC)
    const mkv = testInfo.outputPath(path.basename(file));
    fs.writeFileSync(mkv, Buffer.from(await opfsBytes(page, file)));
    if (enc.aac || enc.opus) {
      expect(a?.codec_name).toBe(enc.aac ? "aac" : "opus");
      expect(Math.abs(Number(a!.duration) - seconds)).toBeLessThan(0.025);
      expect(Number(a!.start_time)).toBe(0);
      // The boot beep starts when it starts in the recording
      const at = (f: string) =>
        onset(
          decode(f, ["-map", "0:a", "-f", "s16le", "-ac", "1", "-ar", "48000"]),
        );
      const want = at(mkv);
      expect(want).toBeGreaterThan(0);
      expect(Math.abs(at(saved) - want)).toBeLessThan(0.01);
    }
    // The last frame, close to the recording's (a lossy codec)
    const lastOf = (f: string) =>
      decode(f, [
        "-map",
        "0:v",
        "-vf",
        `select=eq(n\\,${frames - 1})`,
        "-frames:v",
        "1",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
      ]);
    const x = lastOf(saved);
    const y = lastOf(mkv);
    expect(x.length).toBe(512 * 342 * 3);
    expect(y.length).toBe(x.length);
    let diff = 0;
    for (let i = 0; i < x.length; i++) diff += Math.abs(x[i] - y[i]);
    expect(diff / x.length).toBeLessThan(8);
  } else {
    testInfo.annotations.push({
      type: "note",
      description: "ffprobe not installed: file not probed",
    });
  }

  // It plays in the page's own <video>, at its size and length
  const played = await page.evaluate(async (opfsPath) => {
    const rel = opfsPath.replace(/^\/opfs\/?/, "").split("/");
    const name = rel.pop() as string;
    let dir = await navigator.storage.getDirectory();
    for (const part of rel) dir = await dir.getDirectoryHandle(part);
    const blob = await (await dir.getFileHandle(name)).getFile();
    const v = document.createElement("video");
    v.muted = true;
    const canPlay = v.canPlayType('video/mp4; codecs="avc1.64001f"');
    if (!canPlay) return { canPlay };
    v.src = URL.createObjectURL(blob);
    await new Promise((ok, bad) => {
      v.onloadedmetadata = ok;
      v.onerror = () => bad(new Error(`video error ${v.error?.code}`));
    });
    await v.play();
    await new Promise((ok) => setTimeout(ok, 800));
    v.pause();
    const x = v as HTMLVideoElement & {
      webkitDecodedFrameCount?: number;
      webkitAudioDecodedByteCount?: number;
    };
    return {
      canPlay,
      width: v.videoWidth,
      height: v.videoHeight,
      duration: v.duration,
      frames: x.webkitDecodedFrameCount ?? -1,
      audioBytes: x.webkitAudioDecodedByteCount ?? -1,
    };
  }, mp4);
  test.skip(!played.canPlay, "this browser cannot play H.264 in <video>");
  expect(played.width).toBe(512);
  expect(played.height).toBe(342);
  expect(played.duration!).toBeCloseTo(seconds, 1);
  expect(played.frames!).toBeGreaterThan(0);
  if (enc.aac || enc.opus) expect(played.audioBytes!).toBeGreaterThan(0);
});
