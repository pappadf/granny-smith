<script lang="ts">
  import { machine, setZoom } from '@/state/machine.svelte';
  import { layout, setPanelPos, setPanelCollapsed, type PanelPos } from '@/state/layout.svelte';
  import { theme, cycleTheme, resolveTheme } from '@/state/theme.svelte';
  import { camera, setCameraEnabled } from '@/state/camera.svelte';
  import {
    microphone,
    setMicrophoneEnabled,
    setMicrophoneDevice,
    refreshAudioInputs,
    micStats,
  } from '@/state/microphone.svelte';
  import { openContextMenu, type ContextMenuItem } from '../common/ContextMenu.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import {
    pauseEmulator,
    resumeEmulator,
    shutdownEmulator,
    saveCheckpoint,
    applySchedulerMode,
  } from '@/bus';
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

  // Note: layout.fullscreen is kept in sync with the browser's native
  // fullscreen state by a listener in App.svelte (not here) so it survives
  // this toolbar being unmounted while fullscreen is active.
  const fullscreenIcon: IconName = $derived(layout.fullscreen ? 'screen-normal' : 'screen-full');
  const fullscreenTitle = $derived(
    layout.fullscreen ? 'Exit full screen' : 'Enter full screen — hide panel and chrome',
  );

  const themeTitle = $derived(
    resolveTheme(theme.mode) === 'dark'
      ? 'Theme: dark. Click for light.'
      : 'Theme: light. Click for dark.',
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
    void applySchedulerMode(mode);
  }

  // Camera toggle — shown only on machines with the on-board video
  // digitizer (capabilities.video_in, the AV family). The click doubles as
  // the user gesture getUserMedia needs; the camera light itself follows
  // the guest's capture activity (state/camera.svelte).
  const cameraIcon: IconName = $derived(camera.enabled ? 'camera' : 'camera-off');
  const cameraTitle = $derived(
    camera.enabled
      ? camera.live
        ? 'Camera connected (capturing) — click to disconnect'
        : 'Camera connected — click to disconnect'
      : 'Connect camera to the video input',
  );

  function onCameraClick() {
    void setCameraEnabled(!camera.enabled);
  }

  // Microphone toggle — shown only on machines with on-board audio input
  // (capabilities.audio_in, the AV family). Same shape as the camera: the
  // click is the user gesture getUserMedia needs, and the live indicator
  // follows the guest actually recording (state/microphone.svelte).
  const micIcon: IconName = $derived(microphone.enabled ? 'mic' : 'mic-off');
  // The tooltip carries the transport counters while recording. They are
  // the difference between a diagnosable report and a guess: the guest's
  // recorder gains up hard, so a capture path delivering NOTHING comes back
  // as full-scale white noise — identical, by ear, to one delivering
  // garbage. `in` climbing with `out` means samples are really flowing.
  let micDetail = $state('');
  $effect(() => {
    if (!microphone.live) {
      micDetail = '';
      return;
    }
    const t = setInterval(() => {
      const s = micStats();
      micDetail =
        ` — ${s.rate} Hz, in ${s.produced} / out ${s.consumed}` +
        (s.underruns || s.overruns ? `, under ${s.underruns} over ${s.overruns}` : '');
    }, 500);
    return () => clearInterval(t);
  });

  const micTitle = $derived(
    microphone.enabled
      ? microphone.live
        ? `Microphone connected (recording)${micDetail} — click to choose input`
        : 'Microphone connected — click to choose input'
      : 'Connect microphone to the sound input',
  );

  // A MENU rather than a plain toggle: getUserMedia takes the system default
  // input, and on a docked machine that is regularly not the microphone the
  // user means — a dock's empty headset jack looks perfectly healthy and
  // delivers its own dither forever. Choosing the device has to be reachable
  // from the same control that turns the thing on.
  async function onMicClick(ev: MouseEvent) {
    const btn = ev.currentTarget as HTMLElement;
    const r = btn.getBoundingClientRect();

    if (!microphone.enabled) {
      // Nothing to choose between yet: device labels stay blank until
      // permission has been granted once, so connect first and let the menu
      // do its real work from the second click onwards.
      await setMicrophoneEnabled(true);
      return;
    }

    await refreshAudioInputs();
    const items: ContextMenuItem[] = [
      { label: 'Disconnect microphone', action: () => void setMicrophoneEnabled(false) },
      { sep: true },
      {
        label: `${microphone.deviceId === '' ? '\u2713' : '\u2007'} System default`,
        action: () => void setMicrophoneDevice(''),
      },
      ...microphone.devices
        .filter((d) => d.id && d.id !== 'default')
        .map((d) => ({
          label: `${microphone.deviceId === d.id ? '\u2713' : '\u2007'} ${d.label}`,
          action: () => void setMicrophoneDevice(d.id),
        })),
    ];
    openContextMenu(items, r.left, r.bottom);
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
        class:active={machine.scheduler === 'live'}
        disabled={!isLive}
        title="Real-Time — runs at the original Mac's speed"
        onclick={() => onSchedulerClick('live')}>real-time</button
      >
      <button
        class="sch-btn"
        class:active={machine.scheduler === 'accel'}
        disabled={!isLive}
        title="Accelerated — runs faster while keeping games, sound, and animations at the correct speed, like adding a CPU accelerator card"
        onclick={() => onSchedulerClick('accel')}>accelerated</button
      >
      <button
        class="sch-btn"
        class:active={machine.scheduler === 'turbo'}
        disabled={!isLive}
        title="Fast-Forward — runs everything as fast as possible to skip ahead; games and sound run fast too"
        onclick={() => onSchedulerClick('turbo')}>fast-forward</button
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
    {#if machine.videoIn}
      <button
        class="tbtn"
        class:cam-live={camera.live}
        title={cameraTitle}
        aria-label={cameraTitle}
        aria-pressed={camera.enabled}
        disabled={!isLive}
        onclick={onCameraClick}
      >
        <Icon name={cameraIcon} />
      </button>
    {/if}
    {#if machine.audioIn}
      <button
        class="tbtn"
        class:cam-live={microphone.live}
        title={micTitle}
        aria-label={micTitle}
        aria-pressed={microphone.enabled}
        aria-haspopup="menu"
        disabled={!isLive}
        onclick={onMicClick}
      >
        <Icon name={micIcon} />
      </button>
    {/if}
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
  /* The camera and microphone buttons glow while data is actually flowing
     to the guest. */
  .tbtn.cam-live {
    color: var(--gs-accent, #4ea1ff);
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
