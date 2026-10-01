<script module lang="ts">
  export interface TableColumn<R> {
    key: string;
    label: string;
    width?: string; // CSS width (e.g. "110px" or "1fr")
    sortable?: boolean;
    /** Sort comparator. Default: alphabetical on row[key] toString. */
    cmp?: (a: R, b: R) => number;
    /** Plain text rendered into the cell. */
    text: (row: R) => string;
  }
</script>

<script lang="ts" generics="Row">
  import type { Snippet } from 'svelte';

  interface Props {
    columns: TableColumn<Row>[];
    rows: Row[];
    rowKey: (row: Row) => string;
    sortColumn?: string | null;
    sortDir?: 'asc' | 'desc';
    onSort?: (key: string) => void;
    selectedKey?: string | null;
    onSelect?: (key: string) => void;
    onActivate?: (key: string) => void;
    onContextMenu?: (key: string, ev: MouseEvent) => void;
    /** Empty-state body shown when rows.length === 0. */
    empty?: Snippet;
  }
  let {
    columns,
    rows,
    rowKey,
    sortColumn = null,
    sortDir = 'asc',
    onSort,
    selectedKey = null,
    onSelect,
    onActivate,
    onContextMenu,
    empty,
  }: Props = $props();

  const sortedRows = $derived.by(() => {
    if (!sortColumn) return rows;
    const col = columns.find((c) => c.key === sortColumn);
    if (!col) return rows;
    const cmp = col.cmp ?? ((a: Row, b: Row) => col.text(a).localeCompare(col.text(b)));
    const sorted = [...rows].sort(cmp);
    if (sortDir === 'desc') sorted.reverse();
    return sorted;
  });

  const gridTemplate = $derived(columns.map((c) => c.width ?? '1fr').join(' '));

  function headerClick(col: TableColumn<Row>) {
    if (col.sortable === false) return;
    onSort?.(col.key);
  }
</script>

<div class="table" role="table">
  <div class="thead" role="row" style="grid-template-columns: {gridTemplate};">
    {#each columns as col (col.key)}
      {#if col.sortable === false}
        <div class="th" role="columnheader">{col.label}</div>
      {:else}
        <!-- svelte-ignore a11y_click_events_have_key_events -->
        <div
          class="th sortable"
          role="columnheader"
          tabindex="-1"
          onclick={() => headerClick(col)}
          aria-sort={sortColumn === col.key
            ? sortDir === 'asc'
              ? 'ascending'
              : 'descending'
            : 'none'}
        >
          {col.label}
          {#if sortColumn === col.key}
            <span class="sort-marker">{sortDir === 'asc' ? '▲' : '▼'}</span>
          {/if}
        </div>
      {/if}
    {/each}
  </div>
  <div class="tbody">
    {#if sortedRows.length === 0 && empty}
      <div class="empty">{@render empty()}</div>
    {/if}
    {#each sortedRows as row (rowKey(row))}
      {@const key = rowKey(row)}
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <div
        class="tr"
        class:selected={selectedKey === key}
        role="row"
        tabindex="-1"
        style="grid-template-columns: {gridTemplate};"
        onclick={() => onSelect?.(key)}
        ondblclick={() => onActivate?.(key)}
        oncontextmenu={onContextMenu ? (ev) => onContextMenu(key, ev) : undefined}
      >
        {#each columns as col (col.key)}
          <div class="td" role="cell">{col.text(row)}</div>
        {/each}
      </div>
    {/each}
  </div>
</div>

<style>
  .table {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    min-height: 0;
    overflow: hidden;
    color: var(--gs-text);
  }
  .thead {
    display: grid;
    align-items: center;
    height: var(--gs-size-control-md);
    border-bottom: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-surface-app);
    flex-shrink: 0;
  }
  .th {
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
    text-transform: var(--gs-caps-transform);
    letter-spacing: var(--gs-caps-tracking);
    color: var(--gs-text-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .th.sortable {
    cursor: pointer;
    user-select: none;
  }
  .th.sortable:hover {
    color: var(--gs-text-strong);
  }
  .sort-marker {
    font-size: var(--gs-font-size-3xs);
    margin-left: var(--gs-space-1);
  }
  .tbody {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
  }
  .tr {
    display: grid;
    align-items: center;
    height: var(--gs-size-row);
    cursor: pointer;
    user-select: none;
  }
  .tr:hover {
    background: var(--gs-row-hover);
  }
  .tr.selected {
    background: var(--gs-row-selected);
  }
  .td {
    padding: 0 var(--gs-space-2);
    font-size: var(--gs-font-size-base);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .empty {
    padding: var(--gs-space-4);
    color: var(--gs-text-muted);
    font-size: var(--gs-font-size-base);
  }
</style>
