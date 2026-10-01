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
    font-size: 13px;
    line-height: 16px;
  }
  .content {
    max-width: 560px;
    width: 100%;
    padding: 48px 32px 32px;
  }
  .title {
    font-size: 28px;
    font-weight: 200;
    color: var(--gs-text-strong);
    margin: 0 0 8px 0;
  }
  .subtitle {
    color: var(--gs-text);
    opacity: 0.7;
    margin: 0 0 28px 0;
    font-size: 14px;
  }
  .card {
    background: var(--gs-card-bg);
    border: 1px solid var(--gs-border-card);
    border-radius: 6px;
    padding: 16px;
    margin-bottom: 16px;
  }
  .head {
    display: flex;
    gap: 12px;
    align-items: flex-start;
  }
  .head-text {
    min-width: 0;
  }
  .headline {
    font-size: 15px;
    font-weight: 600;
    color: var(--gs-text-strong);
    margin: 0 0 4px 0;
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
    width: 18px;
    height: 18px;
    margin-top: 1px;
    border-radius: 50%;
    border: 2px solid var(--gs-border-subtle);
    border-top-color: var(--gs-accent);
    animation: spin 0.9s linear infinite;
  }
  .files {
    list-style: none;
    margin: 16px 0 0 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 6px;
  }
  .row :global(.icon) {
    flex: none;
    width: 16px;
    height: 16px;
    color: var(--gs-text);
  }
  .names {
    display: flex;
    flex-direction: column;
    min-width: 0;
    flex: 1 1 auto;
  }
  .label {
    font-size: 11px;
    font-weight: 600;
    text-transform: uppercase;
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
    font-size: 12px;
    font-variant-numeric: tabular-nums;
    opacity: 0.85;
    white-space: nowrap;
  }
  .check {
    color: var(--gs-success-fg);
    font-weight: 700;
    margin-right: 4px;
  }
  .bar {
    position: relative;
    height: 6px;
    border-radius: 3px;
    background: var(--gs-border-subtle);
    overflow: hidden;
  }
  .fill {
    height: 100%;
    width: 0;
    border-radius: 3px;
    background: var(--gs-accent);
    transition: width 200ms ease-out;
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
    animation: slide 1.3s ease-in-out infinite;
  }
  .file.skipped {
    opacity: 0.55;
  }
  .file-error {
    margin: 6px 0 0 26px;
    color: var(--gs-danger-fg);
    word-break: break-word;
  }
  .btn-primary {
    font-family: inherit;
    font-size: 13px;
    padding: 6px 14px;
    height: 30px;
    border-radius: 2px;
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
