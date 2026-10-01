<script lang="ts">
  import Badge from '@/components/ui/Badge.svelte';
  import ProgressBar from '@/components/ui/ProgressBar.svelte';
  import Spinner from '@/components/ui/Spinner.svelte';
  import ActivityDot from '@/components/ui/ActivityDot.svelte';
  import StatusDot from '@/components/ui/StatusDot.svelte';
  import DriveLight from '@/components/ui/DriveLight.svelte';
  import Callout from '@/components/ui/Callout.svelte';
  import Card from '@/components/ui/Card.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import SectionHeading from '@/components/ui/SectionHeading.svelte';
  import Switch from '@/components/ui/Switch.svelte';

  // The feedback primitives in every state: badges, progress, spinners,
  // dots, drive lights, callouts, a card, headings, hints and switches.
  const intents = ['neutral', 'info', 'success', 'warning', 'danger'] as const;
  const states = ['queued', 'active', 'unpacking', 'done', 'failed', 'skipped'] as const;
  const dots = ['idle', 'running', 'paused', 'stopped', 'crashed'] as const;
</script>

<div class="grid">
  <div class="row">
    {#each intents as i (i)}<Badge intent={i}>{i}</Badge>{/each}
    <Badge variant="count">12</Badge>
  </div>
  <div class="bars">
    {#each states as s (s)}
      <span class="lbl">{s}</span>
      <ProgressBar
        state={s}
        value={s === 'queued' || s === 'skipped' ? 0 : s === 'active' ? 0.4 : 1}
        label={s}
      />
    {/each}
    <span class="lbl">indeterminate</span>
    <ProgressBar state="active" label="indeterminate" />
  </div>
  <div class="row">
    <Spinner />
    <Spinner size="sm" />
    <span class="current"><Spinner size="sm" tone="current" /></span>
    <ActivityDot />
    {#each dots as d (d)}<StatusDot state={d} />{/each}
  </div>
  <div class="row bar">
    <DriveLight label="HD" title="idle" activity="idle" />
    <DriveLight label="FD" title="read" activity="read" />
    <DriveLight label="CD" title="write" activity="write" />
  </div>
  <div class="row">
    <Switch checked={false} label="off" />
    <Switch checked label="on" />
    <Switch checked disabled label="disabled" />
  </div>
  <div class="row">
    {#each ['info', 'success', 'warning', 'danger'] as const as i (i)}
      <Callout intent={i} as="div" class="callout">{i}</Callout>
    {/each}
  </div>
  <Card heading="Card heading">
    <SectionHeading level="h4">h4 heading</SectionHeading>
    <Hint>A muted hint.</Hint>
    <Hint tone="error">An error hint.</Hint>
  </Card>
</div>

<style>
  .grid {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-2-5);
    padding: var(--gs-space-3);
  }
  .row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2-5);
  }
  .bar {
    height: var(--gs-size-statusbar);
    background: var(--gs-state-running-bg);
    color: var(--gs-state-active-fg);
    width: max-content;
  }
  .current {
    color: var(--gs-text-link);
    display: inline-flex;
  }
  .bars {
    display: grid;
    grid-template-columns: 110px 280px;
    align-items: center;
    gap: var(--gs-space-1-5) var(--gs-space-3);
  }
  .lbl {
    font-size: var(--gs-font-size-xs);
    color: var(--gs-text-muted);
  }
  .row :global(.callout) {
    padding: var(--gs-space-1-5) var(--gs-space-3);
  }
</style>
