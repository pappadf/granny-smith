<script lang="ts">
  import { machine, type MachineStatus } from '@/state/machine.svelte';
  import { activity, bridgeBusy } from '@/state/activity.svelte';
  import { printer, reopenPrintedDocument } from '@/state/printer.svelte';
  import { setCapsLock } from '@/bus/emulator';
  import { shortModel } from '@/lib/machine';
  import Icon from '../common/Icon.svelte';
  import ToggleChip from '../ui/ToggleChip.svelte';
  import DriveLight from '../ui/DriveLight.svelte';
  import StatusDot from '../ui/StatusDot.svelte';
  import ActivityDot from '../ui/ActivityDot.svelte';

  // Hidden before first machine start. Also surfaces during
  // pre-boot uploads so the user can see large-file progress in the
  // status bar.
  const visible = $derived(machine.status !== 'no-machine' || activity.current !== null);

  const stateLabel: Record<MachineStatus, string> = {
    'no-machine': '',
    running: 'Running',
    paused: 'Paused',
    stopped: 'Stopped',
    crashed: 'Crashed',
  };

  const desc = $derived(
    machine.model && machine.ram ? `${shortModel(machine.model)} · ${machine.ram}` : '',
  );

  // Accelerated-mode CPU speed readout. The core pushes the applied multiplier
  // (1x in every other mode), so gate on the mode too. The governor's ladder
  // gives 1/1.5/2/3/4/6/8x — show up to one decimal, dropping a trailing .0.
  const showSpeed = $derived(machine.scheduler === 'accel');
  const speedLabel = $derived(
    Number.isInteger(machine.acceleratedSpeed)
      ? `${machine.acceleratedSpeed}×`
      : `${machine.acceleratedSpeed.toFixed(1)}×`,
  );

  // Background-checkpoint heartbeat — same flash idiom as the drive
  // lights; each core push (state/machine setCheckpointSaved stores a
  // fresh object, so identity re-fires the effect) lights the glyph for
  // the drive lights' 180 ms. The tooltip carries the time + duration
  // that used to spam the terminal.
  let cpFlash = $state(false);
  $effect(() => {
    if (!machine.checkpoint) return;
    cpFlash = true;
    const t = setTimeout(() => (cpFlash = false), 180);
    return () => clearTimeout(t);
  });
  // The LaserWriter: what it is doing while a job runs (the core's PAP
  // status, state/printer), then a button that reopens the last document.
  const printerJob = $derived(printer.job ? ` “${printer.job}”` : '');
  const printerLabel = $derived(
    printer.activity === 'starting'
      ? 'Printer starting up'
      : printer.activity === 'busy'
        ? `Printing${printerJob}`
        : printer.activity === 'printing'
          ? `Printing${printerJob} · page ${printer.page}`
          : printer.activity === 'error'
            ? `Print failed: ${printer.error}`
            : '',
  );
  const printerBusy = $derived(
    printer.activity === 'starting' ||
      printer.activity === 'busy' ||
      printer.activity === 'printing',
  );

  // The bar's state: no machine reads as idle.
  const barState = $derived(machine.status === 'no-machine' ? 'idle' : machine.status);

  const cpTitle = $derived(
    machine.checkpoint
      ? `Last background checkpoint ${new Date(machine.checkpoint.at).toLocaleTimeString()} (${machine.checkpoint.ms.toFixed(1)} ms)`
      : 'Background checkpoint: none yet this session',
  );
</script>

{#if visible}
  <div class="gs-statusbar" data-state={barState} role="status">
    <div class="statusbar-left">
      <div class="gs-statusbar__item sb-item sb-state" title="Machine state">
        {#if barState === 'crashed'}<Icon name="error" size="sm" />{:else}<StatusDot
            class="dot"
            state={barState}
          />{/if}<span class="label">{stateLabel[machine.status]}</span>
      </div>
      {#if showSpeed}
        <div
          class="gs-statusbar__item sb-item sb-speed"
          title="CPU running at {speedLabel} the original Mac's speed (Accelerated mode); games, sound and animation stay real-time"
        >
          <Icon name="chip" size="sm" /><span class="label">{speedLabel}</span>
        </div>
      {/if}
      {#if machine.status === 'running' && machine.mips > 0}
        <div
          class="gs-statusbar__item sb-item sb-mips"
          title="Emulated CPU throughput: {machine.mips.toFixed(
            1,
          )} million instructions/second ({machine.ticksPerSecond.toFixed(
            0,
          )} ticks/s); tick {machine.tickP50Ms.toFixed(1)} ms typical, {machine.tickMaxMs.toFixed(
            1,
          )} ms worst; bridge request {machine.pollMaxMs.toFixed(1)} ms worst"
        >
          <span class="label">{machine.mips.toFixed(1)} MIPS</span>
        </div>
      {/if}
      {#if machine.drives.hd}
        <DriveLight label="HD" title="Hard disk" activity={machine.driveActivity.hd} />
      {/if}
      {#if machine.drives.fd}
        <DriveLight label="FD" title="Floppy disk" activity={machine.driveActivity.fd} />
      {/if}
      {#if machine.drives.cd}
        <DriveLight label="CD" title="CD-ROM" activity={machine.driveActivity.cd} />
      {/if}
      <DriveLight label="CP" title={cpTitle} activity={cpFlash ? 'write' : 'idle'} />
      <ToggleChip
        class="gs-statusbar__item sb-item sb-caps"
        pressed={machine.capsLock}
        label="Caps Lock"
        title="Caps Lock latch — a mechanically locking key, kept down across restarts. Latch it and Restart to boot Mac OS 8 (Copland) from a volume that has it installed."
        icon="arrow-up"
        onToggle={() => void setCapsLock(!machine.capsLock)}
      />
    </div>
    <div class="statusbar-right">
      {#if bridgeBusy.path}
        <div
          class="gs-statusbar__item sb-item sb-busy"
          title="The emulator is still working on a request"
        >
          <span class="label">Busy: {bridgeBusy.path} ({bridgeBusy.seconds} s)</span>
        </div>
      {/if}
      {#if printerLabel}
        <div
          class="gs-statusbar__item sb-item sb-printer"
          class:error={printer.activity === 'error'}
          data-state={printer.activity === 'error' ? 'error' : undefined}
          title="LaserWriter — {printer.status}"
        >
          {#if printerBusy}<ActivityDot
              class="upload-spinner"
            />{:else if printer.activity === 'error'}<Icon name="error" size="sm" />{/if}
          <span class="printer-label">{printerLabel}</span>
        </div>
      {:else if printer.document}
        <button
          class="gs-statusbar__item gs-statusbar__button sb-item sb-printer sb-printed"
          title="Show the last printed document ({printer.document.name})"
          onclick={reopenPrintedDocument}
        >
          <Icon name="file" size="sm" /><span class="printer-label"
            >{printer.document.title || printer.document.name}</span
          >
        </button>
      {/if}
      {#if activity.current}
        <div class="gs-statusbar__item sb-item sb-upload" title="{activity.verb} in progress">
          <ActivityDot class="upload-spinner" />
          <span class="upload-label">{activity.verb}: {activity.current}</span>
        </div>
      {/if}
      <div class="gs-statusbar__item sb-item sb-desc">{desc}</div>
    </div>
  </div>
{/if}

<style>
  .gs-statusbar {
    flex: 0 0 var(--gs-statusbar-height);
    height: var(--gs-statusbar-height);
    display: flex;
    align-items: stretch;
    background: var(--gs-statusbar-bg-idle);
    color: var(--gs-statusbar-fg-idle);
    font-size: var(--gs-statusbar-font-size);
    line-height: var(--gs-statusbar-height);
    border-top: var(--gs-border-width) solid var(--gs-border);
    transition:
      background-color var(--gs-duration-quick) var(--gs-ease-out),
      color var(--gs-duration-quick) var(--gs-ease-out);
    padding: 0 var(--gs-space-1);
    user-select: none;
  }
  .gs-statusbar:not([data-state='idle']) {
    color: var(--gs-statusbar-fg-active);
  }
  .gs-statusbar[data-state='running'] {
    background: var(--gs-statusbar-bg-running);
  }
  .gs-statusbar[data-state='paused'] {
    background: var(--gs-statusbar-bg-paused);
  }
  .gs-statusbar[data-state='stopped'] {
    background: var(--gs-statusbar-bg-stopped);
  }
  .gs-statusbar[data-state='crashed'] {
    background: var(--gs-statusbar-bg-crashed);
  }
  .statusbar-left,
  .statusbar-right {
    display: flex;
    align-items: stretch;
  }
  .statusbar-left {
    flex: 1 1 auto;
    min-width: 0;
  }
  .statusbar-right {
    flex-direction: row-reverse;
  }
  /* Items are plain text; only the buttons (the caps chip, the printed
     document) light up under the pointer. */
  .gs-statusbar__item {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    padding: 0 var(--gs-space-2);
  }
  .gs-statusbar__button {
    cursor: pointer;
  }
  .gs-statusbar__button:hover {
    background: var(--gs-statusbar-item-hover);
  }
  .gs-statusbar__button:focus-visible {
    outline: var(--gs-focus-width) solid var(--gs-focus-ring);
    outline-offset: var(--gs-focus-offset);
  }
  .sb-speed {
    font-variant-numeric: var(--gs-numeric);
  }
  .sb-speed :global(.icon) {
    opacity: 0.85;
  }
  .sb-mips {
    font-variant-numeric: var(--gs-numeric);
    opacity: 0.75;
  }
  .sb-upload {
    gap: var(--gs-space-1-5);
    font-size: var(--gs-font-size-xs);
  }
  .sb-printer {
    gap: var(--gs-space-1-5);
    font-size: var(--gs-font-size-xs);
  }
  .sb-printer[data-state='error'] {
    color: var(--gs-statusbar-printer-error);
  }
  .sb-printed {
    background: none;
    border: none;
    color: inherit;
    font: inherit;
    font-size: var(--gs-font-size-xs);
    line-height: inherit;
  }
  .printer-label,
  .upload-label {
    max-width: 28ch;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
