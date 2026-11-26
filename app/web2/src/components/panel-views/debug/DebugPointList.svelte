<script lang="ts" generics="T extends { id: number; enabled: boolean; hits: number }">
  // The list section the Breakpoints and Watchpoints sections share: the
  // header's add button, the add form's address field, the latest-listing
  // guard, the context-menu remove and the row loop.  Each section supplies
  // what differs: the bus calls, the add form's other fields and a row's
  // label.
  import type { Snippet } from 'svelte';
  import { onMount } from 'svelte';
  import Icon from '@/components/common/Icon.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import ListRow from '@/components/ui/ListRow.svelte';
  import CollapsibleSection from '@/components/common/CollapsibleSection.svelte';
  import { openContextMenu, type ContextMenuItem } from '@/components/common/ContextMenu.svelte';
  import { machine } from '@/state/machine.svelte';
  import { showNotification } from '@/state/toasts.svelte';
  import { debug, setSection, toggleSection } from '@/state/debug.svelte';
  import { fmtHex32, parseHex } from '@/lib/hex';
  import Button from '@/components/ui/Button.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';

  interface Props {
    // The section's title ("Breakpoints") and the singular noun its
    // messages use ("breakpoint").
    title: string;
    noun: string;
    section: Parameters<typeof toggleSection>[0];
    // The class on each row (what the tests find rows by).
    rowClass: string;
    list: () => Promise<T[]>;
    // true when added, or the message to show when not.
    add: (addr: number) => Promise<true | string>;
    remove: (id: number) => Promise<boolean>;
    // After this section adds or removes one (another pane may list them).
    onChanged?: () => void;
    // A counter bumped when another pane adds or removes one.
    gen?: () => number;
    // Clears the form's own fields after an add or a cancel.
    onReset?: () => void;
    // The form's fields after the address; `keydown` commits on Enter.
    fields?: Snippet<[(ev: KeyboardEvent) => void]>;
    // A row's text after its enable mark.
    label: Snippet<[T]>;
    // The rows, for the section's own derived state (bindable).
    rows?: T[];
  }

  let {
    title,
    noun,
    section,
    rowClass,
    list,
    add,
    remove,
    onChanged,
    gen,
    onReset,
    fields,
    label,
    rows = $bindable([]),
  }: Props = $props();

  const Noun = $derived(noun.charAt(0).toUpperCase() + noun.slice(1));

  let showAdd = $state(false);
  let addAddr = $state('');

  // Only the latest listing is shown.  A listing is several round trips, so
  // one started before a remove (the refresh after a repeated add, say) can
  // finish after the remove's own refresh and would put the row back.
  let listGen = 0;
  async function refresh() {
    const g = ++listGen;
    const got = await list();
    if (g === listGen) rows = got;
  }

  onMount(() => {
    void refresh();
  });

  $effect(() => {
    void machine.status;
    void gen?.();
    if (debug.sections[section]) void refresh();
  });

  function onAddrKey(ev: KeyboardEvent) {
    if (ev.key === 'Enter') {
      ev.preventDefault();
      void commitAdd();
    } else if (ev.key === 'Escape') {
      cancelAdd();
    }
  }

  async function commitAdd() {
    const v = parseHex(addAddr);
    if (v === null) {
      showNotification(`Invalid ${noun} address`, 'error');
      return;
    }
    const added = await add(v);
    if (added !== true) {
      showNotification(added, 'error');
      return;
    }
    showNotification(`${Noun} set at $${fmtHex32(v)}`, 'info');
    addAddr = '';
    onReset?.();
    showAdd = false;
    onChanged?.();
    await refresh();
  }

  function cancelAdd() {
    addAddr = '';
    onReset?.();
    showAdd = false;
  }

  function onRowContext(row: T, ev: MouseEvent) {
    ev.preventDefault();
    const items: ContextMenuItem[] = [
      {
        label: 'Remove',
        danger: true,
        action: () => void doRemove(row),
      },
    ];
    openContextMenu(items, ev.clientX, ev.clientY);
  }

  async function doRemove(row: T) {
    const ok = await remove(row.id);
    if (!ok) {
      showNotification(`Failed to remove ${noun}`, 'error');
      return;
    }
    onChanged?.();
    await refresh();
  }
</script>

<CollapsibleSection {title} open={debug.sections[section]} onToggle={() => toggleSection(section)}>
  {#snippet actions()}
    <IconButton
      class="add-btn"
      icon="plus"
      size="sm"
      tone="panel"
      rest="faded"
      iconSize="md"
      label={`Add ${noun}`}
      onclick={() => {
        // The form is in the section's body: a collapsed section opens to show it.
        setSection(section, true);
        showAdd = true;
      }}
    />
  {/snippet}
  {#if showAdd}
    <div class="add-row">
      <TextInput
        class="add-addr"
        mono
        widthCh={12}
        placeholder="address ($hex)"
        bind:value={addAddr}
        onkeydown={onAddrKey}
        aria-label={`${Noun} address`}
      />
      {@render fields?.(onAddrKey)}
      <Button variant="primary" class="btn" onclick={commitAdd}>Add</Button>
      <Button class="btn" onclick={cancelAdd}>Cancel</Button>
    </div>
  {/if}
  {#if rows.length === 0 && !showAdd}
    <Hint class="hint" inset="section">No {noun}s. Click + to add one.</Hint>
  {:else}
    {#each rows as r (r.id)}
      <ListRow class={rowClass} density="compact" mono oncontextmenu={(ev) => onRowContext(r, ev)}>
        <span class="enable" data-state={r.enabled ? 'on' : 'off'}>
          <Icon name={r.enabled ? 'circle-filled' : 'circle-outline'} size="xs" />
        </span>
        {@render label(r)}
        {#if r.hits > 0}
          <span class="hits">{r.hits}×</span>
        {/if}
      </ListRow>
    {/each}
  {/if}
</CollapsibleSection>

<style>
  .add-row {
    display: flex;
    gap: var(--gs-space-1-5);
    padding: var(--gs-space-1-5) var(--gs-space-3);
  }
  .enable {
    display: inline-flex;
    color: var(--gs-code-breakpoint-off);
  }
  .enable[data-state='on'] {
    color: var(--gs-code-breakpoint);
  }
  .hits {
    color: var(--gs-text-muted);
  }
</style>
