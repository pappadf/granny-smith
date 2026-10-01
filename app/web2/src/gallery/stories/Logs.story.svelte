<script lang="ts">
  import LogsView from '@/components/panel-views/logs/LogsView.svelte';
  import { logs } from '@/state/logs.svelte';
  import { logsPanelHeader } from '@/components/panel-views/logs/logsHeader.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  const v = variant;
  if (v !== 'empty') {
    logs.entries = [
      { ts: 0, cat: 'scsi', level: 1, msg: 'target 0: unit attention after reset' },
      { ts: 0, cat: 'floppy', level: 2, msg: 'drive 0: disk inserted (800K GCR)' },
      { ts: 0, cat: 'cpu', level: 3, msg: 'PC=$0040028E trap $A9F4 _ExitToShell' },
      { ts: 0, cat: 'via1', level: 5, msg: 'IFR write $82 (CA2 cleared)' },
      { ts: 0, cat: 'appletalk', level: 8, msg: 'NBP lookup "=:LaserWriter@*" -> 1 tuple' },
    ];
  }
  logs.catLevels = { cpu: 0, floppy: 2, scsi: 1, via1: 5, appletalk: 8 };
  if (v === 'levels') logsPanelHeader.popoverOpen = true;
</script>

<LogsView />
