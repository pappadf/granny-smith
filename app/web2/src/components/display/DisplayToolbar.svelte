<script lang="ts">
  import { machine } from '@/state/machine.svelte';
  import { layout, setPanelPos, setPanelCollapsed, type PanelPos } from '@/state/layout.svelte';
  import { theme, cycleTheme, resolveTheme } from '@/state/theme.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import Icon from '../common/Icon.svelte';
  import type { IconName } from '@/lib/icons';

  const noMachine = $derived(machine.status === 'no-machine');

  // Layout buttons swap icons depending on whether their position is active.
  // Active position uses the "filled" variant (e.g. layout-bottom); inactive
  // positions use the "-off" variant (layout-bottom-off).
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
      // Request on <html> so the entire viewport is taken over.
      document.documentElement.requestFullscreen().catch(() => {
        showNotification('Full screen blocked by the browser', 'warning');
      });
    }
  }

  // Sync layout.fullscreen with the browser's fullscreen state.
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
</script>

<div class="gs-toolbar" role="toolbar" aria-label="Display toolbar">
  <div class="tg execution">
    <button class="tbtn" title="Run" disabled={noMachine}>
      <Icon name="play" />
    </button>
    <button class="tbtn" title="Shut down — return to Welcome view" disabled={noMachine}>
      <Icon name="sign-out" />
    </button>
    <div class="sep"></div>
    <div class="scheduler" role="group" aria-label="Scheduler mode">
      <button class="sch-btn" disabled={noMachine}>strict</button>
      <button class="sch-btn active" disabled={noMachine}>live</button>
      <button class="sch-btn" disabled={noMachine}>fast</button>
    </div>
  </div>
  <div class="sep"></div>
  <div class="tg view">
    <button class="tbtn" title="Zoom out" disabled={noMachine}>
      <Icon name="minus" />
    </button>
    <input class="zoom-input" value="200%" disabled={noMachine} aria-label="Zoom level" />
    <button class="tbtn" title="Zoom in" disabled={noMachine}>
      <Icon name="plus" />
    </button>
  </div>
  <div class="sep"></div>
  <div class="tg actions">
    <button class="tbtn" title="Save State" disabled={noMachine}>
      <Icon name="download" />
    </button>
  </div>
  <div class="layout-controls">
    <button class="tbtn" title={themeTitle} onclick={cycleTheme}>
      <Icon name="color-mode" />
    </button>
    <button class="tbtn" title={fullscreenTitle} onclick={onFullscreenClick}>
      <Icon name={fullscreenIcon} />
    </button>
    <div class="sep"></div>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'left' && !layout.panelCollapsed}
      title="Panel Left"
      onclick={() => onLayoutClick('left')}
    >
      <Icon name={layoutIcon('left')} />
    </button>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'bottom' && !layout.panelCollapsed}
      title="Panel Bottom"
      onclick={() => onLayoutClick('bottom')}
    >
      <Icon name={layoutIcon('bottom')} />
    </button>
    <button
      class="tbtn layout-btn"
      class:active={layout.panelPos === 'right' && !layout.panelCollapsed}
      title="Panel Right"
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
