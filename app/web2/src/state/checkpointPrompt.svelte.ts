// Reactive UI state for the boot-time "Resume from checkpoint?" prompt.
// The bus orchestration lives in src/bus/checkpoint.ts; this file owns the
// rune-backed flag the prompt component subscribes to.

interface PromptState {
  shown: boolean;
}

export const checkpointPrompt: PromptState = $state({ shown: false });

let resolver: ((accept: boolean) => void) | null = null;

// Open the prompt and return a promise that settles when the user clicks
// Resume or Discard, or the prompt is withdrawn (resolveCheckpointPrompt
// (false) closes it from code too).  A prompt still open is settled as
// declined first, so no caller waits forever on an orphaned promise.
export function showCheckpointPrompt(): Promise<boolean> {
  resolver?.(false);
  checkpointPrompt.shown = true;
  return new Promise<boolean>((resolve) => {
    resolver = resolve;
  });
}

// Called by the prompt component's buttons.
export function resolveCheckpointPrompt(accept: boolean): void {
  checkpointPrompt.shown = false;
  const r = resolver;
  resolver = null;
  if (r) r(accept);
}
