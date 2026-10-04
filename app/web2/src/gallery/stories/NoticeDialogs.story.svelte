<script lang="ts">
  import CheckpointResumePrompt from '@/components/dialogs/CheckpointResumePrompt.svelte';
  import PreviewNoticeDialog from '@/components/dialogs/PreviewNoticeDialog.svelte';
  import PrintViewerDialog from '@/components/dialogs/PrintViewerDialog.svelte';
  import { checkpointPrompt } from '@/state/checkpointPrompt.svelte';
  import { printer } from '@/state/printer.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  const v = variant;
  if (v === 'resume') checkpointPrompt.shown = true;
  if (v === 'print') {
    printer.document = {
      printer: 'LaserWriter',
      name: '00042-Read Me.pdf',
      title: 'Read Me',
      pages: 3,
      url: 'about:blank',
    };
    printer.viewerOpen = true;
  }
  // The preview notice shows itself unless a past visit dismissed it.
  if (v === 'preview') {
    try {
      localStorage.removeItem('gs-preview-notice-dismissed-v1');
    } catch {
      // No storage: the notice shows anyway.
    }
  }
</script>

{#if v === 'resume'}
  <CheckpointResumePrompt />
{:else if v === 'preview'}
  <PreviewNoticeDialog />
{:else}
  <PrintViewerDialog />
{/if}
