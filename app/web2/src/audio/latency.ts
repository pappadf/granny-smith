// The audio-out latency target: the ring depth the worklet's start gate waits
// for and its rate trim steers to.  em_audio.c defaults it to 83 ms (~5 Plus
// VBLs); `?audio_latency=<ms>` overrides it, within what em_audio.c accepts.

export const AUDIO_LATENCY_MIN_MS = 20;
export const AUDIO_LATENCY_MAX_MS = 250;

// The target in seconds from a page's query string, or undefined (the core's
// default) when the parameter is absent or out of range.
export function audioTargetLatencyFromQuery(search: string): number | undefined {
  const raw = new URLSearchParams(search).get('audio_latency');
  if (raw === null || raw.trim() === '') return undefined;
  const ms = Number(raw);
  if (!Number.isFinite(ms) || ms < AUDIO_LATENCY_MIN_MS || ms > AUDIO_LATENCY_MAX_MS)
    return undefined;
  return ms / 1000;
}
