<script lang="ts">
  // The machine-state dot: one colour per state (idle, running, paused,
  // stopped, crashed).  Decorative; the state's name sits next to it.
  interface Props {
    state: 'idle' | 'running' | 'paused' | 'stopped' | 'crashed';
    class?: string;
  }
  let { state, class: cls = '' }: Props = $props();
</script>

<span class="gs-status-dot {cls}" data-state={state} aria-hidden="true"></span>

<style>
  .gs-status-dot {
    display: inline-block;
    flex: none;
    width: var(--gs-size-dot);
    height: var(--gs-size-dot);
    border-radius: var(--gs-radius-round);
    background: var(--gs-statusbar-dot-idle);
  }
  .gs-status-dot[data-state='running'] {
    background: var(--gs-statusbar-dot-running);
  }
  .gs-status-dot[data-state='paused'] {
    background: var(--gs-statusbar-dot-paused);
  }
  .gs-status-dot[data-state='stopped'] {
    background: var(--gs-statusbar-dot-stopped);
  }
  .gs-status-dot[data-state='crashed'] {
    background: var(--gs-statusbar-dot-crashed);
  }
</style>
