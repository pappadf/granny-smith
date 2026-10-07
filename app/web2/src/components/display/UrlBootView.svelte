<script lang="ts">
  // What a page opened to boot from its URL (?rom=…&hd0=…) shows until the
  // machine runs: the Granny Smith headline, what is being fetched, and a
  // progress bar per file.  It replaces Welcome for that load; nothing asks
  // the user anything (bus/urlMedia.ts drives the state, state/urlBoot).
  import Icon from '../common/Icon.svelte';
  import Button from '../ui/Button.svelte';
  import Card from '../ui/Card.svelte';
  import Hero from '../ui/Hero.svelte';
  import Hint from '../ui/Hint.svelte';
  import ProgressBar, { type ProgressState } from '../ui/ProgressBar.svelte';
  import SectionHeading from '../ui/SectionHeading.svelte';
  import Spinner from '../ui/Spinner.svelte';
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

  // The bar's look for a file's status.
  function barState(f: UrlFile): ProgressState {
    return f.status === 'downloading' ? 'active' : f.status;
  }

  function amount(f: UrlFile): string {
    switch (f.status) {
      case 'queued':
        return 'Waiting';
      case 'unpacking':
        return 'Unpacking…';
      case 'done':
        // A blank disk's file is a few KB however large the disk: no size.
        if (f.blank) return f.reused ? 'Blank disk · already stored' : 'Blank disk · created';
        return f.reused ? `Already stored · ${size(f.received)}` : size(f.received);
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
    <Hero
      title="Granny Smith"
      subtitle="A Macintosh emulator in the browser."
      titleClass="title"
      subtitleClass="subtitle"
    />

    <Card class="card {urlBoot.stage === 'failed' ? 'failed' : ''}" aria-live="polite">
      <div class="head">
        {#if urlBoot.stage !== 'failed'}
          <Spinner class="spinner" />
        {/if}
        <div class="head-text">
          <h2 class="headline">{headline}</h2>
          <p class="detail" class:failed={urlBoot.stage === 'failed'}>{detail}</p>
        </div>
      </div>

      {#if urlBoot.files.length}
        <ul class="files">
          {#each urlBoot.files as f (f.slot)}
            <li
              class="file"
              class:done={f.status === 'done'}
              class:failed={f.status === 'failed'}
              class:skipped={f.status === 'skipped'}
              data-state={f.status}
              data-slot={f.slot}
            >
              <div class="row">
                <Icon name={iconFor(f.slot)} />
                <div class="names">
                  <SectionHeading as="span" class="label">{f.label}</SectionHeading>
                  <span class="name" title={f.name}>{f.name}</span>
                </div>
                <span class="amount">
                  {#if f.status === 'done'}<Icon name="check" size="xs" class="check" />{/if}
                  {amount(f)}
                </span>
              </div>
              <ProgressBar
                class="bar"
                value={fraction(f)}
                state={barState(f)}
                label={`${f.label}: ${f.name}`}
              />
              {#if f.error}<Hint as="p" tone="error" class="file-error">{f.error}</Hint>{/if}
            </li>
          {/each}
        </ul>
      {/if}
    </Card>

    {#if urlBoot.stage === 'failed'}
      <Button size="lg" variant="primary" class="btn-primary" onclick={dismissUrlBoot}>
        Go to the start screen
      </Button>
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
    padding: var(--gs-space-12) var(--gs-space-8) var(--gs-space-8);
  }
  .head {
    display: flex;
    gap: var(--gs-space-3);
    align-items: flex-start;
  }
  .head :global(.spinner) {
    margin-top: var(--gs-space-px);
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
    color: var(--gs-text-muted);
  }
  .detail.failed {
    color: var(--gs-danger-fg);
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
  .row > :global(.icon) {
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
  .name {
    color: var(--gs-text-strong);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .amount {
    flex: none;
    display: inline-flex;
    align-items: center;
    gap: var(--gs-space-1);
    font-family: var(--gs-font-mono);
    font-size: var(--gs-font-size-sm);
    font-variant-numeric: var(--gs-numeric);
    color: var(--gs-text-muted);
    white-space: nowrap;
  }
  .amount :global(.check) {
    color: var(--gs-success-fg);
  }
  .file.skipped {
    opacity: var(--gs-opacity-skipped);
  }
  /* The error lines up under the file's name, past the slot icon. */
  .file :global(.file-error) {
    margin: var(--gs-space-1-5) 0 0 calc(var(--gs-size-icon) + var(--gs-space-2-5));
    font-size: inherit;
    word-break: break-word;
  }
</style>
