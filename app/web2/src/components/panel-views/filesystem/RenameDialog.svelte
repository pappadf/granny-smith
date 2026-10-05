<script lang="ts" module>
  // A rename target is a single path component. A '/' would silently turn
  // the rename into a move (worst case into the item's own subtree), and
  // '.'/'..' resolve to other directories entirely.
  export function validateName(v: string): string {
    if (v.includes('/') || v.includes('\\')) return 'Name cannot contain slashes';
    if (v === '.' || v === '..') return 'Invalid name';
    return '';
  }
</script>

<script lang="ts">
  import PromptDialog from '@/components/dialogs/PromptDialog.svelte';

  // The Filesystem tab's rename: a PromptDialog that takes one path
  // component.
  interface Props {
    open: boolean;
    title?: string;
    initial: string;
    onSubmit: (newName: string) => void;
    onClose: () => void;
  }
  let { open, title = 'Rename', initial, onSubmit, onClose }: Props = $props();
</script>

<PromptDialog
  {open}
  {title}
  label="New name"
  {initial}
  submitText="Rename"
  validate={validateName}
  inputId="rename-input"
  {onSubmit}
  {onClose}
/>
