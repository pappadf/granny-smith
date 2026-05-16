<script lang="ts">
  import { onMount } from 'svelte';
  import Table, { type TableColumn } from '@/components/common/Table.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { opfs } from '@/bus/opfs';
  import { gsEval } from '@/bus/emulator';
  import { showNotification } from '@/state/toasts.svelte';
  import {
    checkpoints,
    setCheckpointSort,
    selectCheckpoint,
    type CheckpointSortColumn,
  } from '@/state/checkpoints.svelte';
  import { checkpointsView } from './checkpointsView.svelte';
  import {
    checkpointCreatedToDate,
    formatBytes,
    formatCheckpointLabel,
  } from '@/lib/checkpointMeta';
  import type { CheckpointEntry } from '@/bus/types';

  let rows = $state<CheckpointEntry[]>([]);

  async function refresh() {
    rows = await opfs.scanCheckpoints();
  }

  onMount(() => {
    void refresh();
    // Expose refresh to the panel-header button.
    checkpointsView.refresh = refresh;
    return () => {
      if (checkpointsView.refresh === refresh) checkpointsView.refresh = null;
    };
  });

  function formatDate(created: string): string {
    const d = checkpointCreatedToDate(created);
    if (!d) return created;
    return d.toISOString().slice(0, 16).replace('T', ' ');
  }

  const columns: TableColumn<CheckpointEntry>[] = [
    {
      key: 'name',
      label: 'Name',
      width: '1fr',
      cmp: (a, b) => a.label.localeCompare(b.label),
      text: (row) => row.label || formatCheckpointLabel(row.created),
    },
    {
      key: 'machine',
      label: 'Machine',
      width: '110px',
      cmp: (a, b) => a.machine.localeCompare(b.machine),
      text: (row) => row.machine,
    },
    {
      key: 'date',
      label: 'Date',
      width: '160px',
      cmp: (a, b) => a.created.localeCompare(b.created),
      text: (row) => formatDate(row.created),
    },
    {
      key: 'size',
      label: 'Size',
      width: '80px',
      cmp: (a, b) => a.sizeBytes - b.sizeBytes,
      text: (row) => formatBytes(row.sizeBytes),
    },
  ];

  async function loadCheckpoint(row: CheckpointEntry) {
    try {
      const ok = await gsEval('checkpoint.load', [`${row.path}/state.checkpoint`]);
      if (ok === true) {
        showNotification(`Loaded checkpoint '${row.label}'`, 'info');
      } else {
        showNotification('Checkpoint load failed', 'error');
      }
    } catch {
      showNotification('Checkpoint load failed', 'error');
    }
  }

  function onContext(key: string, ev: MouseEvent) {
    ev.preventDefault();
    const row = rows.find((r) => r.path === key);
    if (!row) return;
    selectCheckpoint(key);
    const items: ContextMenuItem[] = [
      { label: 'Load', action: () => loadCheckpoint(row) },
      { sep: true },
      { label: 'Rename', action: () => doRename(row) },
      { label: 'Download', action: () => doDownload(row) },
      { label: 'Delete', action: () => doDelete(row), danger: true },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function doRename(row: CheckpointEntry) {
    if (typeof window === 'undefined' || typeof window.prompt !== 'function') return;
    const next = window.prompt('Checkpoint label', row.label);
    if (!next || next === row.label) return;
    try {
      await opfs.writeJson(`${row.path}/manifest.json`, { label: next, machine: row.machine });
      await refresh();
      showNotification(`Renamed checkpoint to '${next}'`, 'info');
    } catch {
      showNotification('Rename failed', 'error');
    }
  }

  function doDownload(row: CheckpointEntry) {
    showNotification(`Download of '${row.label}' will land in a later phase`, 'warning');
  }

  async function doDelete(row: CheckpointEntry) {
    if (typeof window !== 'undefined' && typeof window.confirm === 'function') {
      if (!window.confirm(`Delete checkpoint '${row.label}'?`)) return;
    }
    try {
      await opfs.delete(row.path);
      await refresh();
      showNotification(`Deleted checkpoint '${row.label}'`, 'info');
    } catch {
      showNotification('Delete failed', 'error');
    }
  }
</script>

<div class="checkpoints-view">
  <Table
    {columns}
    {rows}
    rowKey={(r) => r.path}
    sortColumn={checkpoints.sortColumn}
    sortDir={checkpoints.sortDir}
    onSort={(c) => setCheckpointSort(c as CheckpointSortColumn)}
    selectedKey={checkpoints.selectedDir}
    onSelect={(k) => selectCheckpoint(k)}
    onActivate={(k) => {
      const row = rows.find((r) => r.path === k);
      if (row) void loadCheckpoint(row);
    }}
    onContextMenu={onContext}
  >
    {#snippet empty()}
      No checkpoints yet. Use the <strong>Save State</strong> button or the panel-header
      <strong>Create Checkpoint</strong> action to capture one.
    {/snippet}
  </Table>
</div>

<style>
  .checkpoints-view {
    width: 100%;
    height: 100%;
    min-height: 0;
    display: flex;
    flex-direction: column;
  }
</style>
