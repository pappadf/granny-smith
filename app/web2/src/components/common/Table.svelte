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
    color: var(--gs-fg);
  }
  .thead {
    display: grid;
    align-items: center;
    height: 26px;
    border-bottom: 1px solid var(--gs-border);
    background: var(--gs-bg);
    flex-shrink: 0;
  }
  .th {
    padding: 0 8px;
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
    letter-spacing: 0.04em;
    color: var(--gs-fg-muted);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .th.sortable {
    cursor: pointer;
    user-select: none;
  }
  .th.sortable:hover {
    color: var(--gs-fg-bright);
  }
  .sort-marker {
    font-size: 9px;
    margin-left: 4px;
  }
  .tbody {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
  }
  .tr {
    display: grid;
    align-items: center;
    height: 22px;
    cursor: pointer;
    user-select: none;
  }
  .tr:hover {
    background: var(--gs-row-hover, rgba(255, 255, 255, 0.05));
  }
  .tr.selected {
    background: var(--gs-row-selected, rgba(80, 140, 220, 0.25));
  }
  .td {
    padding: 0 8px;
    font-size: 13px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .empty {
    padding: 16px;
    color: var(--gs-fg-muted);
    font-size: 13px;
  }
</style>
