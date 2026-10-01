<script lang="ts">
  import StatusBar from '@/components/status-bar/StatusBar.svelte';
  import { machine, type MachineStatus } from '@/state/machine.svelte';
  import { activity } from '@/state/activity.svelte';
  import { printer } from '@/state/printer.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // The bar in every machine state; `activity` lights every indicator on a
  // running bar, `idle-activity` the same on the idle (no machine) bar.
  // svelte-ignore state_referenced_locally
  const v = variant;
  const status: MachineStatus =
    v === 'idle' || v === 'idle-activity'
      ? 'no-machine'
      : v === 'activity'
        ? 'running'
        : (v as MachineStatus);
  machine.status = status;
  machine.model = 'Macintosh IIcx';
  machine.ram = '8 MB';
  machine.drives = { hd: true, fd: true, cd: true };
  machine.scheduler = v === 'running' ? 'accel' : 'live';
  machine.acceleratedSpeed = 2.5;
  machine.mips = status === 'running' ? 12.3 : 0;
  if (v === 'activity' || v === 'idle-activity') {
    machine.driveActivity = { hd: 'read', fd: 'write', cd: 'idle' };
    machine.capsLock = true;
    printer.activity = 'printing';
    printer.job = 'Untitled';
    printer.page = 2;
    activity.current = 'System 7.5.3.dsk';
    activity.verb = 'Uploading';
  } else if (v === 'stopped') {
    printer.activity = 'error';
    printer.error = 'PostScript error';
  } else if (v === 'paused') {
    printer.document = { name: 'doc.pdf', title: 'Read Me', url: 'about:blank', pages: 1 };
  } else if (v === 'idle') {
    activity.current = 'ROM upload';
  }
</script>

<div class="frame"><StatusBar /></div>

<style>
  .frame {
    display: flex;
    flex-direction: column;
    margin-top: 16px;
  }
</style>
