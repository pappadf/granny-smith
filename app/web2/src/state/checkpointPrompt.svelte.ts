// Reactive UI state for the boot-time "Resume from checkpoint?" prompt.
// The bus orchestration lives in src/bus/checkpoint.ts; this file owns the
// rune-backed flag the prompt component subscribes to.

interface PromptState {
  shown: boolean;
}

export const checkpointPrompt: PromptState = $state({ shown: false });

let resolver: ((accept: boolean | null) => void) | null = null;

// Open the prompt and return a promise that settles when the user clicks
// Resume (true) or Discard (false); resolveCheckpointPrompt closes it from
// code too.  A prompt still open is settled first as withdrawn (null) --
// not as Discard, which would delete the checkpoint the new prompt offers --
// so no caller waits forever on an orphaned promise.
export function showCheckpointPrompt(): Promise<boolean | null> {
  resolver?.(null);
  checkpointPrompt.shown = true;
  return new Promise<boolean | null>((resolve) => {
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
