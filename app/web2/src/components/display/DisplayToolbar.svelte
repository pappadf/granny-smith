<script lang="ts">
  import { machine, setSchedulerMode, setZoom } from '@/state/machine.svelte';
  import { layout, setPanelPos, setPanelCollapsed, type PanelPos } from '@/state/layout.svelte';
  import { theme, cycleTheme, resolveTheme } from '@/state/theme.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { pauseEmulator, resumeEmulator, shutdownEmulator, saveCheckpoint } from '@/bus';
  import Icon from '../common/Icon.svelte';
  import type { IconName } from '@/lib/icons';
  import type { SchedulerMode } from '@/state/machine.svelte';

  // Enable predicates (Phase 2 wiring). `isLive` covers running + paused — the
  // states where machine-dependent toolbar buttons are interactive. After a
  // Shut Down the status is 'stopped'; Welcome view is shown again so the user
  // can pick a new config, but the Run/Save/etc. buttons stay disabled until
  // they do.
  const isLive = $derived(machine.status === 'running' || machine.status === 'paused');
  const everStarted = $derived(machine.status !== 'no-machine');

  let saving = $state(false);
  const zoomInput = $derived(`${machine.zoom}%`);

  function layoutIcon(pos: PanelPos): IconName {
    const active = layout.panelPos === pos && !layout.panelCollapsed;
    if (pos === 'left') return active ? 'layout-left' : 'layout-left-off';
    if (pos === 'right') return active ? 'layout-right' : 'layout-right-off';
    return active ? 'layout-bottom' : 'layout-bottom-off';
  }

  function onLayoutClick(pos: PanelPos) {
    if (layout.panelCollapsed) {
      setPanelCollapsed(false);
      setPanelPos(pos);
    } else if (pos === layout.panelPos) {
      setPanelCollapsed(true);
    } else {
      setPanelPos(pos);
    }
  }

  function onFullscreenClick() {
    if (document.fullscreenElement) {
      void document.exitFullscreen().catch(() => undefined);
    } else {
      document.documentElement.requestFullscreen().catch(() => {
        showNotification('Full screen blocked by the browser', 'warning');
      });
    }
  }

  $effect(() => {
    const handler = () => {
      layout.fullscreen = !!document.fullscreenElement;
    };
    document.addEventListener('fullscreenchange', handler);
    return () => document.removeEventListener('fullscreenchange', handler);
  });

  const fullscreenIcon: IconName = $derived(layout.fullscreen ? 'screen-normal' : 'screen-full');
  const fullscreenTitle = $derived(
    layout.fullscreen ? 'Exit full screen' : 'Enter full screen — hide panel and chrome',
  );

  const themeTitle = $derived(
    theme.mode === 'system'
      ? `Theme: system (${resolveTheme(theme.mode)}). Click for dark.`
      : theme.mode === 'dark'
        ? 'Theme: dark. Click for light.'
        : 'Theme: light. Click for system.',
  );

  // Run/Pause icon flip — prototype app.js:877-881.
  const runIcon: IconName = $derived(machine.status === 'running' ? 'pause' : 'play');
  const runTitle = $derived(machine.status === 'running' ? 'Pause' : 'Run');

  async function onRunPause() {
    if (machine.status === 'running') await pauseEmulator();
    else if (machine.status === 'paused') await resumeEmulator();
  }

  async function onShutdown() {
    await shutdownEmulator();
  }

  async function onSave() {
    saving = true;
    try {
      const path = await saveCheckpoint();
      showNotification(`State saved (${path})`, 'info');
    } finally {
      // Match prototype's 400 ms re-enable delay (app.js:962).
      setTimeout(() => (saving = false), 400);
    }
  }

  function onSchedulerClick(mode: SchedulerMode) {
    setSchedulerMode(mode);
  }

  function onZoomInput(e: Event) {
    const input = e.target as HTMLInputElement;
    const n = parseInt(input.value, 10);
    if (Number.isFinite(n)) setZoom(n);
    // If the user typed a non-number, the $derived `zoomInput` will revert
    // the displayed value on the next reactive tick without us touching it.
    else input.value = `${machine.zoom}%`;
  }
</script>

<div class="gs-toolbar" role="toolbar" aria-label="Display toolbar">
  <div class="tg execution">
    <button
      class="tbtn"
      title={runTitle}
      aria-label={runTitle}
      disabled={!isLive}
      onclick={onRunPause}
    >
      <Icon name={runIcon} />
    </button>
    <button
      class="tbtn"
      title="Shut down — return to Welcome view"
      aria-label="Shut down"
      disabled={!everStarted}
      onclick={onShutdown}
    >
      <Icon name="sign-out" />
    </button>
    <div class="sep"></div>
    <div class="scheduler" role="group" aria-label="Scheduler mode">
      <button
        class="sch-btn"
        class:active={machine.scheduler === 'strict'}
        disabled={!isLive}
        onclick={() => onSchedulerClick('strict')}>strict</button
      >
      <button
        class="sch-btn"
        class:active={machine.scheduler === 'live'}
        disabled={!isLive}
        onclick={() => onSchedulerClick('live')}>live</button
      >
      <button
        class="sch-btn"
        class:active={machine.scheduler === 'fast'}
        disabled={!isLive}
        onclick={() => onSchedulerClick('fast')}>fast</button
      >
    </div>
  </div>
  <div class="sep"></div>
  <div class="tg view">
    <button
      class="tbtn"
      title="Zoom out"
      aria-label="Zoom out"
      disabled={!isLive}
      onclick={() => setZoom(machine.zoom - 10)}
    >
      <Icon name="minus" />
    </button>
    <input
      class="zoom-input"
      value={zoomInput}
      disabled={!isLive}
      aria-label="Zoom level"
      onchange={onZoomInput}
    />
    <button
      class="tbtn"
      title="Zoom in"
      aria-label="Zoom in"
      disabled={!isLive}
      onclick={() => setZoom(machine.zoom + 10)}
    >
      <Icon name="plus" />
    </button>
  </div>
  <div class="sep"></div>
  <div class="tg actions">
    <button
      class="tbtn"
      title="Save State"
      aria-label="Save State"
      disabled={!isLive || saving}
      onclick={onSave}
    >
      <Icon name="download" />
    </button>
  </div>
  <div class="layout-controls">
    <button class="tbtn" title={themeTitle} aria-label={themeTitle} onclick={cycleTheme}>
      <Icon name="color-mode" />
    </button>
    <button
      class="tbtn"
      title={fullscreenTitle}
      aria-label={fullscreenTitle}
      onclick={onFullscreenClick}
    >
      <Icon name={fullscreenIcon} />
    </button>
    <div class="sep"></div>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'left' && !layout.panelCollapsed}
      title="Panel Left"
      aria-label="Panel Left"
      onclick={() => onLayoutClick('left')}
    >
      <Icon name={layoutIcon('left')} />
    </button>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'bottom' && !layout.panelCollapsed}
      title="Panel Bottom"
      aria-label="Panel Bottom"
      onclick={() => onLayoutClick('bottom')}
    >
      <Icon name={layoutIcon('bottom')} />
    </button>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'right' && !layout.panelCollapsed}
      title="Panel Right"
      aria-label="Panel Right"
      onclick={() => onLayoutClick('right')}
    >
      <Icon name={layoutIcon('right')} />
    </button>
  </div>
</div>

<style>
  .gs-toolbar {
    height: 35px;
    flex: 0 0 35px;
    display: flex;
    align-items: center;
    padding: 0 8px;
    gap: 0;
    background: var(--gs-bg);
    border-bottom: 1px solid var(--gs-border);
    color: var(--gs-fg-bright);
    user-select: none;
  }
  .tg {
    display: flex;
    align-items: center;
    gap: 4px;
  }
  .tg.actions {
    margin-left: 4px;
  }
  .sep {
    width: 1px;
    height: 16px;
    background: var(--gs-border);
    margin: 0 8px;
  }
  .tbtn {
    display: flex;
    align-items: center;
    justify-content: center;
    width: 22px;
    height: 22px;
    padding: 3px;
    border: none;
    border-radius: 6px;
    background: transparent;
    color: inherit;
    cursor: pointer;
  }
  .tbtn:hover:not(:disabled) {
    background: var(--gs-btn-hover);
  }
  .tbtn:active:not(:disabled) {
    background: var(--gs-btn-active);
  }
  .tbtn:focus-visible {
    outline: 1px solid var(--gs-focus);
    outline-offset: -1px;
  }
  .tbtn:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .scheduler {
    display: flex;
    align-items: center;
    gap: 0;
    border-radius: 3px;
    overflow: hidden;
  }
  .sch-btn {
    padding: 2px 6px;
    font-size: 11px;
    background: transparent;
    color: var(--gs-fg-muted);
    border: none;
    border-radius: 3px;
    cursor: pointer;
  }
  .sch-btn:hover:not(:disabled) {
    color: var(--gs-fg-bright);
    background: var(--gs-btn-hover);
  }
  .sch-btn.active {
    color: var(--gs-fg-bright);
    background: var(--gs-btn-hover);
  }
  .sch-btn:disabled {
    opacity: 0.4;
    cursor: default;
  }
  .zoom-input {
    width: 48px;
    height: 22px;
    font-size: 11px;
    text-align: center;
    background: transparent;
    color: var(--gs-fg-bright);
    border: 1px solid transparent;
    border-radius: 2px;
    outline: none;
  }
  .zoom-input:focus {
    border-color: var(--gs-focus);
  }
  .zoom-input:disabled {
    opacity: 0.4;
  }
  .layout-controls {
    display: flex;
    align-items: center;
    gap: 4px;
    margin-left: auto;
    height: 100%;
  }
</style>
