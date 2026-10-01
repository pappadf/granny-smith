<script lang="ts">
  import { machine, type MachineStatus } from '@/state/machine.svelte';
  import { activity, bridgeBusy } from '@/state/activity.svelte';
  import { printer, reopenPrintedDocument } from '@/state/printer.svelte';
  import { setCapsLock } from '@/bus/emulator';
  import { shortModel } from '@/lib/machine';
  import DriveActivity from './DriveActivity.svelte';
  import Icon from '../common/Icon.svelte';
  import ToggleChip from '../ui/ToggleChip.svelte';

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

  const cpTitle = $derived(
    machine.checkpoint
      ? `Last background checkpoint ${new Date(machine.checkpoint.at).toLocaleTimeString()} (${machine.checkpoint.ms.toFixed(1)} ms)`
      : 'Background checkpoint: none yet this session',
  );
</script>

{#if visible}
  <div
    class="gs-statusbar"
    class:running={machine.status === 'running'}
    class:paused={machine.status === 'paused'}
    class:stopped={machine.status === 'stopped'}
    data-state={machine.status === 'no-machine' ? 'idle' : machine.status}
    role="status"
  >
    <div class="statusbar-left">
      <div class="sb-item sb-state" title="Machine state">
        <span class="dot"></span><span class="label">{stateLabel[machine.status]}</span>
      </div>
      {#if showSpeed}
        <div
          class="sb-item sb-speed"
          title="CPU running at {speedLabel} the original Mac's speed (Accelerated mode); games, sound and animation stay real-time"
        >
          <Icon name="chip" size={13} /><span class="label">{speedLabel}</span>
        </div>
      {/if}
      {#if machine.status === 'running' && machine.mips > 0}
        <div
          class="sb-item sb-mips"
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
        <DriveActivity label="HD" title="Hard disk" activity={machine.driveActivity.hd} />
      {/if}
      {#if machine.drives.fd}
        <DriveActivity label="FD" title="Floppy disk" activity={machine.driveActivity.fd} />
      {/if}
      {#if machine.drives.cd}
        <DriveActivity label="CD" title="CD-ROM" activity={machine.driveActivity.cd} />
      {/if}
      <DriveActivity label="CP" title={cpTitle} activity={cpFlash ? 'write' : 'idle'} />
      <ToggleChip
        class="sb-item sb-caps"
        pressed={machine.capsLock}
        label="Caps Lock"
        title="Caps Lock latch — a mechanically locking key, kept down across restarts. Latch it and Restart to boot Mac OS 8 (Copland) from a volume that has it installed."
        onToggle={() => void setCapsLock(!machine.capsLock)}
        ><span class="label">⇪</span></ToggleChip
      >
    </div>
    <div class="statusbar-right">
      {#if bridgeBusy.path}
        <div class="sb-item sb-busy" title="The emulator is still working on a request">
          <span class="label">Busy: {bridgeBusy.path} ({bridgeBusy.seconds} s)</span>
        </div>
      {/if}
      {#if printerLabel}
        <div
          class="sb-item sb-printer"
          class:error={printer.activity === 'error'}
          title="LaserWriter — {printer.status}"
        >
          {#if printerBusy}<span class="upload-spinner"></span>{/if}
          <span class="printer-label">{printerLabel}</span>
        </div>
      {:else if printer.document}
        <button
          class="sb-item sb-printer sb-printed"
          title="Show the last printed document ({printer.document.name})"
          onclick={reopenPrintedDocument}
        >
          <Icon name="file" size={13} /><span class="printer-label"
            >{printer.document.title || printer.document.name}</span
          >
        </button>
      {/if}
      {#if activity.current}
        <div class="sb-item sb-upload" title="{activity.verb} in progress">
          <span class="upload-spinner"></span>
          <span class="upload-label">{activity.verb}: {activity.current}</span>
        </div>
      {/if}
      <div class="sb-item sb-desc">{desc}</div>
    </div>
  </div>
{/if}

<style>
  .gs-statusbar {
    flex: 0 0 var(--gs-size-statusbar);
    height: var(--gs-size-statusbar);
    display: flex;
    align-items: stretch;
    background: var(--gs-state-idle-bg);
    color: var(--gs-state-idle-fg);
    font-size: var(--gs-font-size-sm);
    line-height: var(--gs-size-statusbar);
    border-top: var(--gs-border-width) solid var(--gs-border);
    transition:
      background-color var(--gs-duration-quick) var(--gs-ease-out),
      color var(--gs-duration-quick) var(--gs-ease-out);
    padding: 0 var(--gs-space-1);
    user-select: none;
  }
  .gs-statusbar.running {
    background: var(--gs-state-running-bg);
    color: var(--gs-state-active-fg);
  }
  .gs-statusbar.paused {
    background: var(--gs-state-paused-bg);
    color: var(--gs-state-active-fg);
  }
  .gs-statusbar.stopped {
    background: var(--gs-state-stopped-bg);
    color: var(--gs-state-active-fg);
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
  .sb-item {
    display: flex;
    align-items: center;
    gap: var(--gs-space-1);
    padding: 0 var(--gs-space-2);
    cursor: pointer;
  }
  .sb-item:hover {
    background: var(--gs-state-hover);
  }
  .sb-state .dot {
    width: var(--gs-size-dot);
    height: var(--gs-size-dot);
    border-radius: var(--gs-radius-round);
    background: var(--gs-statusbar-dot-idle);
    display: inline-block;
  }
  .gs-statusbar.running .sb-state .dot {
    background: var(--gs-statusbar-dot-running);
  }
  .gs-statusbar.paused .sb-state .dot {
    background: var(--gs-statusbar-dot-paused);
  }
  .gs-statusbar.stopped .sb-state .dot {
    background: var(--gs-statusbar-dot-stopped);
  }
  .sb-speed {
    gap: var(--gs-space-1);
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
  .upload-spinner {
    width: var(--gs-size-dot);
    height: var(--gs-size-dot);
    border-radius: var(--gs-radius-round);
    background: currentColor;
    opacity: 0.6;
    animation: gs-upload-pulse var(--gs-duration-pulse) var(--gs-ease-in-out) infinite;
  }
  .sb-printer {
    gap: var(--gs-space-1-5);
    font-size: var(--gs-font-size-xs);
  }
  .sb-printer.error {
    opacity: 0.8;
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
  @keyframes gs-upload-pulse {
    0%,
    100% {
      opacity: 0.3;
    }
    50% {
      opacity: 1;
    }
  }
</style>
