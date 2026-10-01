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
    gap: 6px;
    padding: 1px 8px;
    font-family: var(--gs-font-mono);
    font-size: 12px;
    line-height: 1.5;
    color: var(--gs-text);
    white-space: pre-wrap;
    word-break: break-word;
  }
  .cat {
    color: var(--gs-text-muted);
    flex: 0 0 auto;
  }
  .lvl {
    color: var(--gs-text-muted);
    flex: 0 0 auto;
    min-width: 1.5ch;
    text-align: right;
  }
  .msg {
    flex: 1 1 auto;
    min-width: 0;
  }
  .log-line[data-sev='high'] .lvl {
    color: var(--gs-danger-fg);
  }
  .log-line[data-sev='mid'] .lvl {
    color: var(--gs-warning-fg);
  }
</style>
