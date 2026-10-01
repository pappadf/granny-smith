<script lang="ts">
  // The emulated LaserWriter's output: each finished document opens here, in
  // the browser's own PDF viewer (a frame onto the PDF's object URL), with a
  // download under the document's name and a way to open it in a tab.  Both
  // are links the user clicks, so no popup blocker stands in the way -- the
  // document itself arrives long after the user's last click
  // (printer/platen.ts).  Mounted once, in App.svelte.
  import Modal from '@/components/common/Modal.svelte';
  import { printer, closePrintedDocument } from '@/state/printer.svelte';

  const doc = $derived(printer.document);
  const title = $derived(
    doc
      ? `LaserWriter: ${doc.title || doc.name} (${doc.pages} page${doc.pages === 1 ? '' : 's'})`
      : '',
  );
</script>

<Modal open={printer.viewerOpen && doc !== null} {title} onClose={closePrintedDocument} wide>
  {#if doc}
    <iframe class="pdf-frame" src={doc.url} title={doc.name}></iframe>
  {/if}
  {#snippet actions()}
    {#if doc}
      <a class="btn" href={doc.url} target="_blank" rel="noopener">Open in new tab</a>
      <a class="btn" href={doc.url} download={doc.name}>Download</a>
    {/if}
    <button type="button" class="btn primary" onclick={closePrintedDocument}>Close</button>
  {/snippet}
</Modal>

<style>
  .pdf-frame {
    flex: 1 1 auto;
    width: 100%;
    min-height: 0;
    border: 1px solid var(--gs-border);
    background: var(--gs-surface-document);
  }
  .btn {
    background: transparent;
    color: var(--gs-fg);
    border: 1px solid var(--gs-border);
    border-radius: 2px;
    padding: 4px 12px;
    font-size: 13px;
    cursor: pointer;
    text-decoration: none;
  }
  .btn:hover {
    background: var(--gs-row-hover);
  }
  .btn.primary {
    background: var(--gs-primary-bg);
    color: var(--gs-primary-fg);
    border-color: transparent;
  }
  .btn.primary:hover {
    background: var(--gs-primary-hover);
  }
</style>
