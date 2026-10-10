<script lang="ts">
  import { askText, askConfirm } from '@/state/dialogs.svelte';
  import { onMount } from 'svelte';
  import Table, { type TableColumn } from '@/components/common/Table.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { opfs } from '@/bus/opfs';
  import { gsEval } from '@/bus/emulator';
  import { reconcileUiWithMachine } from '@/bus/boot';
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
    describeMachine,
    formatBytes,
    formatCheckpointLabel,
  } from '@/lib/checkpointMeta';
  import { getProfile } from '@/bus/profile';
  import type { CheckpointEntry } from '@/bus/types';
  import { saveBlob } from '@/bus/download';
  import { saveCheckpoint } from '@/bus/checkpoint';
  import { getOrCreateMachine } from '@/lib/machineId';
  import { sanitizeName } from '@/lib/archive';
  import { machine } from '@/state/machine.svelte';

  let rows = $state<CheckpointEntry[]>([]);

  // Model id -> its name, looked up once the rows name a model.
  let modelNames = $state<Record<string, string>>({});

  async function refresh() {
    rows = await opfs.scanCheckpoints();
    for (const m of new Set(rows.map((r) => r.model))) {
      if (!m || modelNames[m]) continue;
      const p = await getProfile(m).catch(() => null);
      if (p?.name) modelNames[m] = p.name;
    }
  }

  // "Macintosh Plus · 4 MB", or "unknown" when the manifest names no model.
  function machineOf(row: CheckpointEntry): string {
    return row.model
      ? describeMachine(modelNames[row.model] ?? row.model, row.ramBytes)
      : 'unknown';
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
      width: 'var(--gs-checkpoints-col-machine)',
      cmp: (a, b) => machineOf(a).localeCompare(machineOf(b)),
      text: (row) => machineOf(row),
    },
    {
      key: 'date',
      label: 'Date',
      width: 'var(--gs-checkpoints-col-date)',
      cmp: (a, b) => a.created.localeCompare(b.created),
      text: (row) => formatDate(row.created),
    },
    {
      key: 'size',
      label: 'Size',
      width: 'var(--gs-checkpoints-col-size)',
      cmp: (a, b) => a.sizeBytes - b.sizeBytes,
      text: (row) => formatBytes(row.sizeBytes),
    },
  ];

  async function loadCheckpoint(row: CheckpointEntry) {
    try {
      const ok = await gsEval('checkpoint.load', [`${row.path}/state.checkpoint`]);
      if (ok === true) {
        await reconcileUiWithMachine('restore');
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
      { label: 'Save to computer…', action: () => doDownload(row) },
      { label: 'Delete', action: () => doDelete(row), danger: true },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function doRename(row: CheckpointEntry) {
    const next = await askText({
      title: 'Rename checkpoint',
      label: 'Checkpoint label',
      initial: row.label,
      submitText: 'Rename',
    });
    if (!next || next === row.label) return;
    try {
      // Only the label changes: the rest of the manifest is the core's.
      const path = `${row.path}/manifest.json`;
      const manifest = (await opfs.readJson<Record<string, unknown>>(path)) ?? {};
      await opfs.writeJson(path, { ...manifest, label: next });
      await refresh();
      showNotification(`Renamed checkpoint to '${next}'`, 'info');
    } catch {
      showNotification('Rename failed', 'error');
    }
  }

  // A checkpoint the user created is self-contained: its file is the
  // download.  A machine's background checkpoint refers to disk deltas that
  // only make sense in this browser, so for the running machine the
  // download is a Save State (a self-contained copy made now); any other
  // machine has to be resumed first.
  async function doDownload(row: CheckpointEntry) {
    if (row.saved) {
      try {
        const blob = await opfs.readFile(`${row.path}/state.checkpoint`);
        saveBlob(blob, `${sanitizeName(row.label) || 'checkpoint'}.bin`);
      } catch {
        showNotification(`Could not read '${row.label}'`, 'error');
      }
      return;
    }
    const me = getOrCreateMachine();
    const running = machine.status === 'running' || machine.status === 'paused';
    if (!running || row.dirName !== `${me.id}-${me.created}`) {
      showNotification(
        `Load '${row.label}' first, then use Save State to save it to your computer`,
        'warning',
      );
      return;
    }
    const res = await saveCheckpoint();
    if (res.ok) showNotification(`State saved (${res.name})`, 'info');
    else showNotification(`Save State failed (${res.step}): ${res.message}`, 'error');
  }

  async function doDelete(row: CheckpointEntry) {
    const ok = await askConfirm({
      title: 'Delete',
      message: `Delete checkpoint '${row.label}'?`,
      confirmText: 'Delete',
      danger: true,
    });
    if (!ok) return;
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
