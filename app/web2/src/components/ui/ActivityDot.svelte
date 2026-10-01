<script lang="ts">
  // A small pulsing dot: work in progress with no measurable amount (an
  // upload, a print job).  Static under reduced motion.  Decorative.
  interface Props {
    class?: string;
  }
  let { class: cls = '' }: Props = $props();
</script>

<span class="gs-activity-dot {cls}" aria-hidden="true"></span>

<style>
  .gs-activity-dot {
    display: inline-block;
    flex: none;
    width: var(--gs-activity-dot-size);
    height: var(--gs-activity-dot-size);
    border-radius: var(--gs-radius-round);
    background: var(--gs-activity-dot-fg);
    opacity: var(--gs-activity-dot-opacity-max);
    animation: gs-activity-pulse var(--gs-duration-pulse) var(--gs-ease-in-out) infinite;
  }
  @keyframes gs-activity-pulse {
    0%,
    100% {
      opacity: var(--gs-activity-dot-opacity-min);
    }
    50% {
      opacity: 1;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .gs-activity-dot {
      animation: none;
    }
  }
</style>
