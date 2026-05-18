import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import Table, { type TableColumn } from '@/components/common/Table.svelte';

interface Row {
  name: string;
  size: number;
}

// The component's generic type parameter can't flow through @testing-library's
// render() — we erase to `unknown` so the test can drive the same logic.
const COLUMNS = [
  { key: 'name', label: 'Name', width: '1fr', text: (r: Row) => r.name },
  {
    key: 'size',
    label: 'Size',
    width: '80px',
    cmp: (a: Row, b: Row) => a.size - b.size,
    text: (r: Row) => String(r.size),
  },
] as unknown as TableColumn<unknown>[];

const ROWS: Row[] = [
  { name: 'gamma', size: 30 },
  { name: 'alpha', size: 10 },
  { name: 'beta', size: 20 },
];

describe('Table', () => {
  it('renders headers and rows', () => {
    const { container } = render(Table, {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: ((r: Row) => r.name) as unknown as (r: unknown) => string,
    });
    expect(container.querySelectorAll('.th').length).toBe(2);
    expect(container.querySelectorAll('.tr').length).toBe(3);
  });

  it('sorts ascending by the active column', () => {
    const { container } = render(Table, {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: ((r: Row) => r.name) as unknown as (r: unknown) => string,
      sortColumn: 'name',
      sortDir: 'asc',
    });
    const firstCell = container.querySelector('.tr .td')?.textContent;
    expect(firstCell).toBe('alpha');
  });

  it('clicking a sortable header calls onSort with the column key', async () => {
    const onSort = vi.fn();
    const { container } = render(Table, {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: ((r: Row) => r.name) as unknown as (r: unknown) => string,
      onSort,
    });
    const sizeHeader = Array.from(container.querySelectorAll('.th')).find((h) =>
      h.textContent?.trim().startsWith('Size'),
    ) as HTMLElement;
    await fireEvent.click(sizeHeader);
    expect(onSort).toHaveBeenCalledWith('size');
  });

  it('double-click on a row calls onActivate with the row key', async () => {
    const onActivate = vi.fn();
    const { container } = render(Table, {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: ((r: Row) => r.name) as unknown as (r: unknown) => string,
      onActivate,
    });
    const firstRow = container.querySelector('.tr') as HTMLElement;
    await fireEvent.dblClick(firstRow);
    expect(onActivate).toHaveBeenCalled();
  });

  it('right-click on a row calls onContextMenu', async () => {
    const onContextMenu = vi.fn();
    const { container } = render(Table, {
      columns: COLUMNS,
      rows: ROWS,
      rowKey: ((r: Row) => r.name) as unknown as (r: unknown) => string,
      onContextMenu,
    });
    const firstRow = container.querySelector('.tr') as HTMLElement;
    await fireEvent.contextMenu(firstRow);
    expect(onContextMenu).toHaveBeenCalled();
  });
});
