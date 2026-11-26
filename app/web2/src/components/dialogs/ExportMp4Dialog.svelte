<script lang="ts">
  // Export MP4: a finished recording to H.264 (and AAC, or Opus where the
  // browser has no AAC encoder) through WebCodecs, in a worker.  One
  // choice, the size -- the recording's own by default, or 2x to 4x
  // nearest-neighbour -- then progress with Cancel.  Hide leaves the export
  // running; a toast offers the file when it is done.  Mounted once, in
  // App.svelte; driven by recorder/mp4Export.ts.
  import Modal from '@/components/common/Modal.svelte';
  import Button from '@/components/ui/Button.svelte';
  import Hint from '@/components/ui/Hint.svelte';
  import RadioGroup from '@/components/ui/RadioGroup.svelte';
  import ProgressBar from '@/components/ui/ProgressBar.svelte';
  import Spinner from '@/components/ui/Spinner.svelte';
  import { mp4Export } from '@/state/mp4Export.svelte';
  import { cancelExport, closeExportDialog, mp4PathFor, startExport } from '@/recorder/mp4Export';

  const basename = (p: string): string => p.slice(p.lastIndexOf('/') + 1);

  const first = $derived(mp4Export.files[0]);
  const many = $derived(mp4Export.paths.length > 1);
  const options = $derived(
    (first?.sizes ?? []).map((s) => ({
      value: s.scale,
      label:
        `${s.scale}× — ${s.width} × ${s.height}` +
        (s.scale === 1 ? ' (the recording’s own size)' : '') +
        (s.ok ? '' : ' — this browser cannot encode it'),
      disabled: !s.ok,
    })),
  );
  const hasSound = $derived(mp4Export.files.some((f) => (f.info?.audioRate ?? 0) > 0));
  const audio = $derived(first?.audio ?? null);
  const truncated = $derived(mp4Export.files.some((f) => f.info?.truncated));
  const fraction = $derived(mp4Export.total ? mp4Export.done / mp4Export.total : null);
  const title = $derived(many ? `Export MP4 — ${mp4Export.paths.length} files` : 'Export MP4');
</script>

<Modal open={mp4Export.open && mp4Export.phase !== 'idle'} {title} onClose={closeExportDialog}>
  <div class="export-body" data-phase={mp4Export.phase}>
    {#if mp4Export.phase === 'loading'}
      <div class="export-loading">
        <Spinner size="sm" />
        <span>Reading the recording…</span>
      </div>
    {:else if mp4Export.phase === 'choose'}
      <Hint class="export-help">
        {#if many}
          Each of the {mp4Export.paths.length} files becomes its own MP4 beside it ({basename(
            mp4PathFor(mp4Export.paths[0]),
          )}, …).
        {:else}
          Saved as {basename(mp4PathFor(mp4Export.paths[0] ?? ''))} beside the recording.
        {/if}
      </Hint>
      <RadioGroup name="export-size" label="Picture size" bind:value={mp4Export.scale} {options} />
      <Hint class="export-note">
        H.264 stores colour at half resolution: at 1× a one-pixel coloured detail is softened
        (black-and-white screens are not affected); at 2× and above nothing is lost.
      </Hint>
      <Hint class="export-note">
        YouTube gives small videos its lowest quality: for an upload, 2× to 4× looks markedly better
        — or upload the .mkv itself.
      </Hint>
      {#if hasSound}
        {#if audio === 'aac'}
          <Hint class="export-note export-audio">Sound: AAC, 48 kHz.</Hint>
        {:else if audio === 'opus'}
          <Hint class="export-note export-audio">
            Sound: Opus, 48 kHz — this browser has no AAC encoder. Opus in MP4 plays in Chrome,
            Edge, Firefox and current Safari.
          </Hint>
        {:else}
          <Hint class="export-note export-audio">
            Video only: this browser has no AAC or Opus encoder, so the sound is left out.
          </Hint>
        {/if}
      {/if}
      {#if truncated}
        <Hint class="export-note">
          This recording was not closed properly; the export ends at its last complete part.
        </Hint>
      {/if}
    {:else if mp4Export.phase === 'running'}
      <ProgressBar class="export-progress" value={fraction} label="Export progress" />
      <Hint class="export-status">
        {#if mp4Export.total}
          Frame {mp4Export.done} of {mp4Export.total}{many
            ? ` — file ${mp4Export.file + 1} of ${mp4Export.paths.length}`
            : ''}
        {:else}
          Starting…
        {/if}
      </Hint>
    {:else if mp4Export.phase === 'failed'}
      <Hint as="div" tone="error" class="export-error">The export failed: {mp4Export.error}</Hint>
    {/if}
  </div>

  {#snippet actions()}
    {#if mp4Export.phase === 'running'}
      <Button size="lg" onclick={closeExportDialog}>Hide</Button>
      <Button size="lg" variant="danger" onclick={cancelExport}>Cancel export</Button>
    {:else if mp4Export.phase === 'failed'}
      <Button size="lg" variant="primary" onclick={closeExportDialog}>Close</Button>
    {:else}
      <Button size="lg" onclick={closeExportDialog}>Cancel</Button>
      <Button
        size="lg"
        variant="primary"
        onclick={startExport}
        disabled={mp4Export.phase !== 'choose'}
      >
        Export
      </Button>
    {/if}
  {/snippet}
</Modal>

<style>
  .export-body {
    min-width: 320px;
    max-width: 460px;
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-3);
  }
  .export-loading {
    display: flex;
    align-items: center;
    gap: var(--gs-space-2);
  }
  .export-body :global(.export-help),
  .export-body :global(.export-note),
  .export-body :global(.export-status) {
    margin: 0;
  }
</style>
