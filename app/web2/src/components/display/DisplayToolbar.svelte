<script lang="ts">
  import { machine, setZoom } from '@/state/machine.svelte';
  import { layout, setPanelPos, setPanelCollapsed, type PanelPos } from '@/state/layout.svelte';
  import { resolved, setSkin } from '@/state/appearance.svelte';
  import { skins } from '@/skins/registry';
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
  import IconButton from '../ui/IconButton.svelte';
  import SegmentedControl from '../ui/SegmentedControl.svelte';
  import Separator from '../ui/Separator.svelte';
  import Toolbar from '../ui/Toolbar.svelte';
  import TextInput from '../ui/TextInput.svelte';
  import type { IconName } from '@/lib/icons';
  import type { SchedulerMode } from '@/state/machine.svelte';

  // Enable predicates. `isLive` covers running + paused — the
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

  // The appearance menu: every skin, the one on screen checked.
  function onAppearanceMenu(ev: MouseEvent) {
    const r = (ev.currentTarget as HTMLElement).getBoundingClientRect();
    const items: ContextMenuItem[] = skins.map((s) => ({
      label: s.name,
      checked: resolved.skin === s.id,
      action: () => setSkin(s.id),
    }));
    openContextMenu(items, r.left, r.bottom);
  }

  // Run/Pause icon flip.
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
      const res = await saveCheckpoint();
      if (res.ok) showNotification(`State saved (${res.name})`, 'info');
      else showNotification(`Save State failed (${res.step}): ${res.message}`, 'error');
    } finally {
      // Re-enable after 400 ms.
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

  // `live` means the capture graph is up, which is now true whenever the user
  // has the microphone on — it no longer follows the guest's input DMA, because
  // tearing the graph down on every endpointer close is what broke recognition.
  // So "(recording)" has to ask the guest, not the graph.
  const micTitle = $derived(
    microphone.enabled
      ? microphone.guestActive
        ? `Microphone connected (recording)${micDetail} — click to choose input`
        : `Microphone connected${micDetail} — click to choose input`
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
        label: 'System default',
        checked: microphone.deviceId === '',
        action: () => void setMicrophoneDevice(''),
      },
      ...microphone.devices
        .filter((d) => d.id && d.id !== 'default')
        .map((d) => ({
          label: d.label,
          checked: microphone.deviceId === d.id,
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

<Toolbar label="Display toolbar">
  <div class="tg execution">
    <IconButton
      class="tbtn"
      icon={runIcon}
      label={runTitle}
      data-caption={runTitle}
      disabled={!isLive}
      onclick={onRunPause}
    />
    <IconButton
      class="tbtn"
      icon="power"
      label="Shut down"
      title="Shut down — return to Welcome view"
      disabled={!everStarted}
      onclick={onShutdown}
    />
    <Separator class="sep" />
    <SegmentedControl
      class="scheduler"
      optionClass="sch-btn"
      label="Scheduler mode"
      disabled={!isLive}
      value={machine.scheduler}
      onChange={onSchedulerClick}
      options={[
        {
          value: 'live',
          label: 'Real',
          title: "Real — runs at the original Mac's speed",
        },
        {
          value: 'accel',
          label: 'Faster',
          title:
            'Faster — runs faster while keeping games, sound, and animations at the correct speed, like adding a CPU accelerator card',
        },
        {
          value: 'turbo',
          label: 'Max',
          title:
            'Max — runs everything as fast as possible to skip ahead; games and sound run fast too',
        },
      ]}
    />
  </div>
  <Separator class="sep" />
  <div class="tg view" data-caption="Zoom">
    <IconButton
      class="tbtn"
      icon="minus"
      label="Zoom out"
      disabled={!isLive}
      onclick={() => setZoom(machine.zoom - 10)}
    />
    <TextInput
      class="zoom-input"
      bare
      style="width: 48px"
      value={zoomInput}
      disabled={!isLive}
      aria-label="Zoom level"
      onchange={onZoomInput}
    />
    <IconButton
      class="tbtn"
      icon="plus"
      label="Zoom in"
      disabled={!isLive}
      onclick={() => setZoom(machine.zoom + 10)}
    />
  </div>
  <Separator class="sep" />
  <div class="tg actions">
    <IconButton
      class="tbtn"
      icon="download"
      label="Save State"
      disabled={!isLive || saving}
      onclick={onSave}
    />
    {#if machine.videoIn}
      <IconButton
        class="tbtn"
        live={camera.live}
        icon={cameraIcon}
        label={cameraTitle}
        pressed={camera.enabled}
        disabled={!isLive}
        onclick={onCameraClick}
      />
    {/if}
    {#if machine.audioIn}
      <IconButton
        class="tbtn"
        live={microphone.guestActive}
        icon={micIcon}
        label={micTitle}
        pressed={microphone.enabled}
        aria-haspopup="menu"
        disabled={!isLive}
        onclick={onMicClick}
      />
    {/if}
  </div>
  <div class="layout-controls">
    <IconButton
      class="tbtn appearance-menu"
      icon="brush"
      label="Appearance"
      aria-haspopup="menu"
      onclick={onAppearanceMenu}
    />
    <IconButton
      class="tbtn"
      icon={fullscreenIcon}
      label={fullscreenTitle}
      onclick={onFullscreenClick}
    />
    <Separator class="sep" />
    <IconButton
      class="tbtn layout-btn"
      icon={layoutIcon('left')}
      label="Panel Left"
      pressed={layout.panelPos === 'left' && !layout.panelCollapsed}
      onclick={() => onLayoutClick('left')}
    />
    <IconButton
      class="tbtn layout-btn"
      icon={layoutIcon('bottom')}
      label="Panel Bottom"
      pressed={layout.panelPos === 'bottom' && !layout.panelCollapsed}
      onclick={() => onLayoutClick('bottom')}
    />
    <IconButton
      class="tbtn layout-btn"
      icon={layoutIcon('right')}
      label="Panel Right"
      pressed={layout.panelPos === 'right' && !layout.panelCollapsed}
      onclick={() => onLayoutClick('right')}
    />
  </div>
</Toolbar>

<style>
  .tg {
    display: flex;
    align-items: center;
    gap: var(--gs-toolbar-gap);
  }
  .tg.actions {
    margin-left: var(--gs-space-1);
  }
  .layout-controls {
    display: flex;
    align-items: center;
    gap: var(--gs-toolbar-gap);
    margin-left: auto;
    height: 100%;
  }
</style>
