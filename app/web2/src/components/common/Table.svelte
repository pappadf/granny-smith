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
    /** Zebra rows (--gs-row-alt). */
    striped?: boolean;
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
    striped = false,
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

<!-- Legacy hooks: table, thead, th, sortable, sort-marker, tbody, tr, td,
     empty, selected. -->
<div class="gs-table table" class:gs-table--striped={striped} role="grid" aria-readonly="true">
  <div class="gs-table__head thead" role="row" style="grid-template-columns: {gridTemplate};">
    {#each columns as col (col.key)}
      {#if col.sortable === false}
        <div class="gs-table__th th" role="columnheader">{col.label}</div>
      {:else}
        <!-- svelte-ignore a11y_click_events_have_key_events -->
        <div
          class="gs-table__th th sortable"
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
            <span class="gs-table__sort sort-marker">{sortDir === 'asc' ? '▲' : '▼'}</span>
          {/if}
        </div>
      {/if}
    {/each}
  </div>
  <div class="gs-table__body tbody" role="rowgroup">
    {#if sortedRows.length === 0 && empty}
      <div class="gs-table__empty empty">{@render empty()}</div>
    {/if}
    {#each sortedRows as row (rowKey(row))}
      {@const key = rowKey(row)}
      <!-- svelte-ignore a11y_click_events_have_key_events -->
      <div
        class="gs-table__row tr"
        class:selected={selectedKey === key}
        role="row"
        tabindex="-1"
        aria-selected={selectedKey === key}
        style="grid-template-columns: {gridTemplate};"
        onclick={() => onSelect?.(key)}
        ondblclick={() => onActivate?.(key)}
        oncontextmenu={onContextMenu ? (ev) => onContextMenu(key, ev) : undefined}
      >
        {#each columns as col (col.key)}
          <div class="gs-table__cell td" role="gridcell">{col.text(row)}</div>
        {/each}
      </div>
    {/each}
  </div>
</div>

<style>
  .gs-table {
    display: flex;
    flex-direction: column;
    width: 100%;
    height: 100%;
    min-height: 0;
    overflow: hidden;
    color: var(--gs-text);
  }
  .gs-table__head {
    display: grid;
    align-items: center;
    height: var(--gs-table-header-height);
    border-bottom: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-table-header-bg);
    flex-shrink: 0;
  }
  .gs-table__th {
    padding: 0 var(--gs-row-padding-x);
    font-size: var(--gs-heading-font-size);
    font-weight: var(--gs-heading-weight);
    text-transform: var(--gs-heading-transform);
    letter-spacing: var(--gs-heading-tracking);
    color: var(--gs-heading-fg);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .gs-table__th.sortable {
    cursor: pointer;
    user-select: none;
  }
  .gs-table__th.sortable:hover {
    color: var(--gs-text-strong);
  }
  .gs-table__sort {
    font-size: var(--gs-table-sort-size);
    margin-left: var(--gs-space-1);
  }
  .gs-table__body {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
  }
  .gs-table__row {
    display: grid;
    align-items: center;
    height: var(--gs-row-height);
    cursor: pointer;
    user-select: none;
  }
  .gs-table--striped .gs-table__row:nth-child(even) {
    background: var(--gs-row-alt);
  }
  .gs-table__row:hover {
    background: var(--gs-row-hover);
  }
  /* The selected row: the focused-list colour while focus is in the table. */
  .gs-table__row[aria-selected='true'] {
    background: var(--gs-row-selected-inactive);
    color: var(--gs-row-selected-inactive-fg);
  }
  .gs-table:focus-within .gs-table__row[aria-selected='true'] {
    background: var(--gs-row-selected);
    color: var(--gs-row-selected-fg);
  }
  .gs-table__cell {
    padding: 0 var(--gs-row-padding-x);
    font-size: var(--gs-row-font-size);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .gs-table__empty {
    padding: var(--gs-hint-padding-view);
    color: var(--gs-hint-fg);
    font-size: var(--gs-row-font-size);
  }
</style>
