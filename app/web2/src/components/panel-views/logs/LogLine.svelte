<script lang="ts">
  import type { LogEntry } from '@/state/logs.svelte';

  interface Props {
    entry: LogEntry;
  }
  let { entry }: Props = $props();

  // Color band per level — low numbers are louder (per docs/log.md
  // "smaller means more important"). Bucket coarsely to map onto the
  // existing toast severity tokens.
  const severity = $derived(entry.level <= 1 ? 'high' : entry.level <= 3 ? 'mid' : 'low');
</script>

<div class="log-line" data-sev={severity}>
  <span class="cat">[{entry.cat}]</span>
  <span class="lvl">{entry.level}</span>
  <span class="msg">{entry.msg}</span>
</div>

<style>
  .log-line {
    display: flex;
    align-items: baseline;
    gap: var(--gs-space-1-5);
    padding: var(--gs-space-px) var(--gs-space-2);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
    line-height: var(--gs-line-height-relaxed);
    color: var(--gs-log-fg);
    white-space: pre-wrap;
    word-break: break-word;
  }
  .cat {
    color: var(--gs-log-meta-fg);
    flex: 0 0 auto;
  }
  .lvl {
    color: var(--gs-log-meta-fg);
    flex: 0 0 auto;
    min-width: 1.5ch;
    text-align: right;
  }
  .msg {
    flex: 1 1 auto;
    min-width: 0;
  }
  .log-line[data-sev='high'] .lvl {
    color: var(--gs-log-high-fg);
  }
  .log-line[data-sev='mid'] .lvl {
    color: var(--gs-log-mid-fg);
  }
  .log-line[data-sev='low'] .msg {
    color: var(--gs-log-low-fg);
  }
</style>
