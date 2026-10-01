<script lang="ts">
  import Button from '@/components/ui/Button.svelte';
  import IconButton from '@/components/ui/IconButton.svelte';
  import SegmentedControl from '@/components/ui/SegmentedControl.svelte';
  import ToggleChip from '@/components/ui/ToggleChip.svelte';
  import Link from '@/components/ui/Link.svelte';
  import ActionRow from '@/components/ui/ActionRow.svelte';

  // Every button family, size and state.
  const variants = ['primary', 'secondary', 'danger', 'ghost'] as const;
  const noop = () => undefined;
  let mode = $state<'live' | 'accel' | 'turbo'>('accel');
  let framed = $state(true);
</script>

<div class="grid">
  {#each ['sm', 'lg'] as const as size (size)}
    <div class="row">
      {#each variants as v (v)}
        <Button variant={v} {size} class="b-{v}-{size}">{v}</Button>
      {/each}
      <Button {size} disabled>disabled</Button>
      <Button {size} variant="primary" busy>busy</Button>
      <Button {size} icon="download">icon</Button>
      <Button {size} href="#x">link</Button>
    </div>
  {/each}
  <div class="row">
    <IconButton icon="play" label="Toolbar" />
    <IconButton icon="pause" label="Pressed" pressed />
    <IconButton icon="camera" label="Live" live />
    <IconButton icon="stop" label="Disabled" disabled />
    <IconButton icon="restart" label="Panel" tone="panel" iconSize="md" />
    <IconButton icon="plus" label="Small faded" size="sm" rest="faded" iconSize="md" />
  </div>
  <div class="row">
    <SegmentedControl
      label="Plain"
      value={mode}
      onChange={(v) => (mode = v)}
      options={[
        { value: 'live', label: 'real-time' },
        { value: 'accel', label: 'accelerated' },
        { value: 'turbo', label: 'fast-forward' },
      ]}
    />
    <SegmentedControl
      label="Framed"
      framed
      value={framed}
      onChange={(v) => (framed = v)}
      options={[
        { value: true, label: 'S' },
        { value: false, label: 'U' },
      ]}
    />
    <SegmentedControl
      label="Disabled"
      disabled
      value="a"
      onChange={noop}
      options={[
        { value: 'a', label: 'one' },
        { value: 'b', label: 'two' },
      ]}
    />
    <ToggleChip pressed={false} label="Off" onToggle={noop}>⇪</ToggleChip>
    <ToggleChip pressed label="On" onToggle={noop}>⇪</ToggleChip>
  </div>
  <div class="row">
    <Link href="#back">← Back</Link>
    <Link href="https://example.org" external>External link</Link>
  </div>
  <div class="col">
    <ActionRow icon="mac" label="New Machine..." onclick={noop} />
    <ActionRow icon="clock" label="Open Checkpoint..." description="from a file" onclick={noop} />
  </div>
</div>

<style>
  .grid {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-3);
    padding: var(--gs-space-3);
  }
  .row {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--gs-space-2);
  }
  .col {
    display: flex;
    flex-direction: column;
    width: 320px;
  }
</style>
