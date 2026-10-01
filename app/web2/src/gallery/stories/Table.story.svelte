<script lang="ts">
  import Table, { type TableColumn } from '@/components/common/Table.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  interface Row {
    id: string;
    label: string;
    machine: string;
    created: string;
  }
  const columns: TableColumn<Row>[] = [
    { key: 'label', label: 'Label', text: (r) => r.label },
    { key: 'machine', label: 'Machine', width: '140px', text: (r) => r.machine },
    { key: 'created', label: 'Created', width: '150px', text: (r) => r.created },
  ];
  const rows: Row[] = [
    { id: 'a', label: 'Before installing', machine: 'Macintosh IIcx', created: '2026-09-28 10:12' },
    { id: 'b', label: 'Clean System 7', machine: 'Macintosh Plus', created: '2026-09-29 18:40' },
    { id: 'c', label: 'Mid-game save', machine: 'Macintosh SE/30', created: '2026-09-30 21:05' },
  ];
</script>

<Table
  {columns}
  rows={variant === 'empty' ? [] : rows}
  rowKey={(r) => r.id}
  sortColumn="created"
  sortDir="desc"
  selectedKey="b"
>
  {#snippet empty()}No checkpoints yet.{/snippet}
</Table>
