// Shared rune-state for the Logs panel header. PanelHeader.svelte
// triggers the popover open/close; LogsView.svelte renders it. Both
// import this module so neither has to know about the other.

interface State {
  popoverOpen: boolean;
}

export const logsPanelHeader: State = $state({
  popoverOpen: false,
});

export function toggleLogsPopover(): void {
  logsPanelHeader.popoverOpen = !logsPanelHeader.popoverOpen;
}
