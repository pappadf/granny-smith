<script lang="ts">
  // A determinate or indeterminate bar.  `value` is 0..1; without it the
  // bar slides (indeterminate).  `state` picks the fill: active, queued,
  // unpacking (a striped, moving fill), done, failed, skipped.  The
  // classes done / failed / skipped / indeterminate are kept as aliases.
  export type ProgressState = 'queued' | 'active' | 'unpacking' | 'done' | 'failed' | 'skipped';
  interface Props {
    value?: number | null;
    state?: ProgressState;
    label: string;
    class?: string;
  }
  let { value = null, state = 'active', label, class: cls = '' }: Props = $props();

  const indeterminate = $derived(value === null && state !== 'failed');
  const pct = $derived(value === null ? undefined : Math.floor(value * 100));
</script>

<div
  class="gs-progress {cls}"
  class:indeterminate
  class:done={state === 'done'}
  class:failed={state === 'failed'}
  class:skipped={state === 'skipped'}
  data-state={state}
  data-indeterminate={indeterminate || undefined}
  role="progressbar"
  aria-label={label}
  aria-valuemin="0"
  aria-valuemax="100"
  aria-valuenow={pct}
>
  <div
    class="gs-progress__fill fill"
    style:width={value === null ? undefined : `${value * 100}%`}
  ></div>
</div>

<style>
  .gs-progress {
    position: relative;
    height: var(--gs-progress-height);
    border-radius: var(--gs-progress-radius);
    background: var(--gs-progress-track);
    overflow: hidden;
  }
  .gs-progress__fill {
    height: 100%;
    width: 0;
    border-radius: var(--gs-progress-radius);
    background: var(--gs-progress-fill);
    transition: width var(--gs-duration-base) var(--gs-ease-out);
  }
  .gs-progress[data-state='done'] .gs-progress__fill {
    background: var(--gs-progress-fill-done);
  }
  .gs-progress[data-state='failed'] .gs-progress__fill {
    width: 100%;
    background: var(--gs-progress-fill-failed);
  }
  /* Unpacking: the download is in, the archive is being expanded. */
  .gs-progress[data-state='unpacking'] .gs-progress__fill {
    background-color: var(--gs-progress-fill-unpacking);
    background-image: linear-gradient(
      -45deg,
      var(--gs-progress-stripe) 25%,
      transparent 25%,
      transparent 50%,
      var(--gs-progress-stripe) 50%,
      var(--gs-progress-stripe) 75%,
      transparent 75%,
      transparent
    );
    background-size: var(--gs-progress-stripe-size) var(--gs-progress-stripe-size);
    animation: gs-progress-stripes var(--gs-duration-pulse) var(--gs-ease-linear) infinite;
  }
  .gs-progress[data-indeterminate] .gs-progress__fill {
    position: absolute;
    width: 35%;
    animation: gs-progress-slide var(--gs-duration-indeterminate) var(--gs-ease-in-out) infinite;
  }
  @keyframes gs-progress-slide {
    from {
      left: -35%;
    }
    to {
      left: 100%;
    }
  }
  @keyframes gs-progress-stripes {
    to {
      background-position: var(--gs-progress-stripe-size) 0;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .gs-progress[data-indeterminate] .gs-progress__fill {
      animation: none;
      left: 0;
      width: 100%;
      opacity: var(--gs-progress-indeterminate-opacity);
    }
  }
</style>
