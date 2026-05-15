<script lang="ts">
  import { machine, type MachineStatus } from '@/state/machine.svelte';
  import DriveActivity from './DriveActivity.svelte';

  // Spec §11: hidden before first machine start.
  const visible = $derived(machine.status !== 'no-machine');

  const stateLabel: Record<MachineStatus, string> = {
    'no-machine': '',
    running: 'Running',
    paused: 'Paused',
    stopped: 'Stopped',
  };

  const desc = $derived(
    machine.model && machine.ram ? `${shortModel(machine.model)} · ${machine.ram}` : '',
  );

  function shortModel(model: string): string {
    return model.replace(/^Macintosh\s+/i, '');
  }
</script>

{#if visible}
  <div
    class="gs-statusbar"
    class:running={machine.status === 'running'}
    class:paused={machine.status === 'paused'}
    class:stopped={machine.status === 'stopped'}
    role="status"
  >
    <div class="statusbar-left">
      <div class="sb-item sb-state" title="Machine state">
        <span class="dot"></span><span class="label">{stateLabel[machine.status]}</span>
      </div>
      <DriveActivity label="HD" title="Hard disk" activity={machine.driveActivity.hd} />
      <DriveActivity label="FD" title="Floppy disk" activity={machine.driveActivity.fd} />
      <DriveActivity label="CD" title="CD-ROM" activity={machine.driveActivity.cd} />
    </div>
    <div class="statusbar-right">
      <div class="sb-item sb-desc">{desc}</div>
    </div>
  </div>
{/if}

<style>
  .gs-statusbar {
    flex: 0 0 22px;
    height: 22px;
    display: flex;
    align-items: stretch;
    background: var(--gs-sb-idle-bg);
    color: var(--gs-sb-idle-fg);
    font-size: 12px;
    line-height: 22px;
    border-top: 1px solid var(--gs-border);
    transition:
      background-color 0.15s ease-out,
      color 0.15s ease-out;
    padding: 0 4px;
    user-select: none;
  }
  .gs-statusbar.running {
    background: var(--gs-sb-running);
    color: var(--gs-sb-fg-running);
  }
  .gs-statusbar.paused {
    background: var(--gs-sb-paused);
    color: var(--gs-sb-fg-running);
  }
  .gs-statusbar.stopped {
    background: var(--gs-sb-stopped);
    color: var(--gs-sb-fg-running);
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
    gap: 4px;
    padding: 0 8px;
    cursor: pointer;
  }
  .sb-item:hover {
    background: var(--gs-sb-hover);
  }
  .sb-state .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--gs-fg-dim);
    display: inline-block;
  }
  .gs-statusbar.running .sb-state .dot {
    background: #89d185;
  }
  .gs-statusbar.paused .sb-state .dot {
    background: #cca700;
  }
  .gs-statusbar.stopped .sb-state .dot {
    background: #f14c4c;
  }
</style>
