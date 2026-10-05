<script lang="ts">
  import { dialogs, answerText, answerConfirm } from '@/state/dialogs.svelte';
  import PromptDialog from './PromptDialog.svelte';
  import ConfirmDialog from './ConfirmDialog.svelte';

  // Renders the open askText / askConfirm question (state/dialogs).
  const q = $derived(dialogs.current);
</script>

{#if q?.kind === 'text'}
  <PromptDialog
    open
    title={q.title}
    label={q.label}
    initial={q.initial}
    submitText={q.submitText}
    validate={q.validate}
    onSubmit={(v) => answerText(v)}
    onClose={() => answerText(null)}
  />
{:else if q?.kind === 'confirm'}
  <ConfirmDialog
    open
    title={q.title}
    message={q.message}
    confirmText={q.confirmText}
    danger={q.danger}
    onConfirm={() => answerConfirm(true)}
    onClose={() => answerConfirm(false)}
  />
{/if}
