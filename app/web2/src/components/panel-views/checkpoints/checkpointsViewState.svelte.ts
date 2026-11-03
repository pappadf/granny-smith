// Shared rune state for the Checkpoints view. PanelHeader uses the
// `refresh` hook to trigger a re-scan after Create Checkpoint, and the
// view itself sets the hook on mount + clears on destroy.

interface State {
  refresh: (() => Promise<void>) | null;
}

export const checkpointsView: State = $state({ refresh: null });
