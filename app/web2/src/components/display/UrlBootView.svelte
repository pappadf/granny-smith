<script lang="ts">
  // What a page opened to boot from its URL (?rom=…&hd0=…) shows until the
  // machine runs: the Granny Smith headline, what is being fetched, and a
  // progress bar per file.  It replaces Welcome for that load; nothing asks
  // the user anything (bus/urlMedia.ts drives the state, state/urlBoot).
  import Icon from '../common/Icon.svelte';
  import type { IconName } from '@/lib/icons';
  import { urlBoot, dismissUrlBoot, type UrlFile } from '@/state/urlBoot.svelte';

  function iconFor(slot: string): IconName {
    if (slot.startsWith('fd')) return 'floppy';
    if (slot.startsWith('hd')) return 'hd';
    if (slot === 'cd') return 'cd';
    return 'chip';
  }

  // "512 KB", "25.0 MB", "1.20 GB".
  function size(bytes: number): string {
    if (bytes < 1024 * 1024) return `${Math.max(0, Math.round(bytes / 1024))} KB`;
    if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  }

  // The file's completed fraction, or null when it cannot be known (no
  // length from the server, or a compressed response that outgrew it).
  function fraction(f: UrlFile): number | null {
    if (f.status === 'done' || f.status === 'unpacking') return 1;
    if (f.status === 'queued' || f.status === 'skipped') return 0;
    if (f.total && f.received <= f.total) return f.received / f.total;
    return null;
  }

  function amount(f: UrlFile): string {
    switch (f.status) {
      case 'queued':
        return 'Waiting';
      case 'unpacking':
        return 'Unpacking…';
      case 'done':
        return size(f.received);
      case 'failed':
        return 'Failed';
      case 'skipped':
        return 'Not needed';
      default: {
        const fr = fraction(f);
        if (fr === null) return size(f.received);
        return `${size(f.received)} of ${size(f.total ?? 0)} · ${Math.floor(fr * 100)}%`;
      }
    }
  }

  // Files finished with, for "File N of M": fetched or given up on.
  const doneCount = $derived(
    urlBoot.files.filter((f) => f.status === 'done' || f.status === 'failed').length,
  );
  const current = $derived(urlBoot.files.find((f) => f.status === 'downloading') ?? null);

  const headline = $derived(
    urlBoot.stage === 'failed'
      ? 'The machine could not be started'
      : urlBoot.stage === 'booting'
        ? 'Starting the machine…'
        : 'Downloading the machine’s ROM and disks…',
  );

  const detail = $derived.by(() => {
    if (urlBoot.stage === 'failed') return urlBoot.error ?? 'See the messages for what went wrong.';
    if (urlBoot.stage === 'booting') return 'Everything is in. The Mac is being switched on.';
    const n = urlBoot.files.length;
    if (!n) return 'Getting ready…';
    const at = Math.min(doneCount + 1, n);
    return `File ${at} of ${n} — the machine starts by itself once everything is in.`;
  });
</script>

<div class="url-boot" data-testid="url-boot-view">
  <div class="content">
    <h1 class="title">Granny Smith</h1>
    <p class="subtitle">A classic Macintosh emulator in the browser.</p>

    <section class="card" class:failed={urlBoot.stage === 'failed'} aria-live="polite">
      <div class="head">
        {#if urlBoot.stage !== 'failed'}
          <span class="spinner" aria-hidden="true"></span>
        {/if}
        <div class="head-text">
          <h2 class="headline">{headline}</h2>
          <p class="detail">{detail}</p>
        </div>
      </div>

      {#if urlBoot.files.length}
        <ul class="files">
          {#each urlBoot.files as f (f.slot)}
            {@const fr = fraction(f)}
            <li
              class="file"
              class:done={f.status === 'done'}
              class:failed={f.status === 'failed'}
              class:skipped={f.status === 'skipped'}
              class:active={f === current}
              data-slot={f.slot}
            >
              <div class="row">
                <Icon name={iconFor(f.slot)} />
                <div class="names">
                  <span class="label">{f.label}</span>
                  <span class="name" title={f.name}>{f.name}</span>
                </div>
                <span class="amount">
                  {#if f.status === 'done'}<span class="check" aria-hidden="true">✓</span>{/if}
                  {amount(f)}
                </span>
              </div>
              <div
                class="bar"
                class:indeterminate={fr === null && f.status !== 'failed'}
                role="progressbar"
                aria-label={`${f.label}: ${f.name}`}
                aria-valuemin="0"
                aria-valuemax="100"
                aria-valuenow={fr === null ? undefined : Math.floor(fr * 100)}
              >
                <div class="fill" style:width={fr === null ? undefined : `${fr * 100}%`}></div>
              </div>
              {#if f.error}<p class="file-error">{f.error}</p>{/if}
            </li>
          {/each}
        </ul>
      {/if}
    </section>

    {#if urlBoot.stage === 'failed'}
      <button type="button" class="btn-primary" onclick={dismissUrlBoot}>
        Go to the start screen
      </button>
    {/if}
  </div>
</div>

<style>
  .url-boot {
    position: absolute;
    inset: 0;
    display: flex;
    justify-content: center;
    align-items: flex-start;
    overflow: auto;
    color: var(--gs-text);
    font-size: var(--gs-font-size-base);
    line-height: 16px;
  }
  .content {
    max-width: 560px;
    width: 100%;
    padding: 48px var(--gs-space-8) var(--gs-space-8);
  }
  .title {
    font-size: var(--gs-font-size-4xl);
    font-weight: var(--gs-font-weight-light);
    color: var(--gs-text-strong);
    margin: 0 0 var(--gs-space-2) 0;
  }
  .subtitle {
    color: var(--gs-text);
    opacity: 0.7;
    margin: 0 0 var(--gs-space-7) 0;
    font-size: var(--gs-font-size-md);
  }
  .card {
    background: var(--gs-card-bg);
    border: var(--gs-border-width) solid var(--gs-border-card);
    border-radius: var(--gs-radius-lg);
    padding: var(--gs-space-4);
    margin-bottom: var(--gs-space-4);
  }
  .head {
    display: flex;
    gap: var(--gs-space-3);
    align-items: flex-start;
  }
  .head-text {
    min-width: 0;
  }
  .headline {
    font-size: var(--gs-font-size-lg);
    font-weight: var(--gs-font-weight-semibold);
    color: var(--gs-text-strong);
    margin: 0 0 var(--gs-space-1) 0;
  }
  .detail {
    margin: 0;
    color: var(--gs-text);
    opacity: 0.75;
  }
  .card.failed .detail {
    color: var(--gs-danger-fg);
    opacity: 1;
  }
  .spinner {
    flex: none;
    width: var(--gs-size-spinner);
    height: var(--gs-size-spinner);
    margin-top: var(--gs-space-px);
    border-radius: var(--gs-radius-round);
    border: var(--gs-border-width-strong) solid var(--gs-border-subtle);
    border-top-color: var(--gs-accent);
    animation: spin var(--gs-duration-spin) var(--gs-ease-linear) infinite;
  }
  .files {
    list-style: none;
    margin: var(--gs-space-4) 0 0 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-3-5);
  }
  .row {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2-5);
    margin-bottom: var(--gs-space-1-5);
  }
  .row :global(.icon) {
    flex: none;
    width: var(--gs-size-icon);
    height: var(--gs-size-icon);
    color: var(--gs-text);
  }
  .names {
    display: flex;
    flex-direction: column;
    min-width: 0;
    flex: 1 1 auto;
  }
  .label {
    font-size: var(--gs-font-size-xs);
    font-weight: var(--gs-font-weight-semibold);
    text-transform: var(--gs-caps-transform);
    letter-spacing: 0.5px;
    opacity: 0.7;
  }
  .name {
    color: var(--gs-text-strong);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .amount {
    flex: none;
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
    font-variant-numeric: var(--gs-numeric);
    opacity: 0.85;
    white-space: nowrap;
  }
  .check {
    color: var(--gs-success-fg);
    font-weight: var(--gs-font-weight-bold);
    margin-right: var(--gs-space-1);
  }
  .bar {
    position: relative;
    height: var(--gs-size-progress);
    border-radius: var(--gs-radius-sm);
    background: var(--gs-border-subtle);
    overflow: hidden;
  }
  .fill {
    height: 100%;
    width: 0;
    border-radius: var(--gs-radius-sm);
    background: var(--gs-accent);
    transition: width var(--gs-duration-base) var(--gs-ease-out);
  }
  .file.done .fill {
    background: var(--gs-success-solid);
  }
  .file.failed .fill {
    width: 100%;
    background: var(--gs-danger-solid);
  }
  .bar.indeterminate .fill {
    position: absolute;
    width: 35%;
    animation: slide var(--gs-duration-indeterminate) var(--gs-ease-in-out) infinite;
  }
  .file.skipped {
    opacity: 0.55;
  }
  .file-error {
    margin: var(--gs-space-1-5) 0 0 calc(var(--gs-size-icon) + var(--gs-space-2-5));
    color: var(--gs-danger-fg);
    word-break: break-word;
  }
  .btn-primary {
    font-family: inherit;
    font-size: var(--gs-font-size-base);
    padding: var(--gs-space-1-5) var(--gs-space-3-5);
    height: var(--gs-size-control-lg);
    border-radius: var(--gs-radius-xs);
    border: none;
    cursor: pointer;
    background: var(--gs-accent);
    color: var(--gs-text-on-accent);
  }
  .btn-primary:hover {
    background: var(--gs-accent-hover);
  }
  @keyframes spin {
    to {
      transform: rotate(360deg);
    }
  }
  @keyframes slide {
    from {
      left: -35%;
    }
    to {
      left: 100%;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .spinner {
      animation: none;
    }
    .bar.indeterminate .fill {
      animation: none;
      left: 0;
      width: 100%;
      opacity: 0.45;
    }
    .fill {
      transition: none;
    }
  }
</style>
