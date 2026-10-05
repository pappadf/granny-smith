<script lang="ts">
  // The emulated printers' output: each finished document opens here, in
  // the browser's own PDF viewer (a frame onto the PDF's object URL), with a
  // download under the document's name and a way to open it in a tab.  Both
  // are links the user clicks, so no popup blocker stands in the way -- the
  // document itself arrives long after the user's last click
  // (printer/platen.ts, bus/download.ts).  Mounted once, in App.svelte.
  import Modal from '@/components/common/Modal.svelte';
  import Button from '@/components/ui/Button.svelte';
  import { printer, closePrintedDocument } from '@/state/printer.svelte';

  const doc = $derived(printer.document);
  const title = $derived(
    doc
      ? `${doc.printer}: ${doc.title || doc.name} (${doc.pages} page${doc.pages === 1 ? '' : 's'})`
      : '',
  );
</script>

<Modal
  open={printer.viewerOpen && doc !== null}
  {title}
  onClose={closePrintedDocument}
  variant="wide"
>
  {#if doc}
    <iframe class="pdf-frame" src={doc.url} title={doc.name}></iframe>
  {/if}
  {#snippet actions()}
    {#if doc}
      <Button size="lg" class="btn" href={doc.url} target="_blank" rel="noopener"
        >Open in new tab</Button
      >
      <Button size="lg" class="btn" href={doc.url} download={doc.name}>Download</Button>
    {/if}
    <Button size="lg" variant="primary" class="btn primary" onclick={closePrintedDocument}
      >Close</Button
    >
  {/snippet}
</Modal>

<style>
  .pdf-frame {
    flex: 1 1 auto;
    width: 100%;
    min-height: 0;
    border: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-surface-document);
  }
</style>
