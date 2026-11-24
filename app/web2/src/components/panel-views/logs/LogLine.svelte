<script lang="ts">
  import type { LogEntry } from '@/state/logs.svelte';
  import { splitLogpoint } from '@/lib/logView';

  interface Props {
    entry: LogEntry;
  }
  let { entry }: Props = $props();

  // Color band per level — low numbers are louder (per docs/log.md
  // "smaller means more important"), bucketed onto the --gs-log-high/
  // mid/low-fg tokens.
  const severity = $derived(entry.level <= 1 ? 'high' : entry.level <= 3 ? 'mid' : 'low');

  // A logpoint's line: its message up front, where it fired (the PC, or the
  // access) after it in the muted meta colour, instead of the core's long
  // "logpoint $002E005C:" prefix.
  const lp = $derived(splitLogpoint(entry.msg));

  // Wall-clock time the page saw the line, to the millisecond.
  const time = $derived.by(() => {
    const d = new Date(entry.ts);
    const p = (n: number, w = 2) => String(n).padStart(w, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
  });
</script>

<div class="log-line" data-sev={severity} data-logpoint={lp ? '' : undefined}>
  <span class="time">{time}</span>
  <span class="cat">[{entry.cat}]</span>
  <span class="lvl">{entry.level}</span>
  {#if lp}
    <span class="msg">{lp.message}</span>
    <span class="where">{lp.where}</span>
  {:else}
    <span class="msg">{entry.msg}</span>
  {/if}
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
    /* A full buffer is thousands of rows: lay out only those in view. */
    content-visibility: auto;
    contain-intrinsic-size: auto 1.5em;
  }
  .time,
  .cat,
  .where {
    color: var(--gs-log-meta-fg);
    flex: 0 0 auto;
  }
  .time {
    font-size: var(--gs-font-size-xs);
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
  .where {
    font-size: var(--gs-font-size-xs);
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
