<script lang="ts">
  // One value's editor, chosen by its type descriptor: a toggle for a bool, a
  // dropdown for an enum with values, a text field otherwise.  Enter (or a
  // pick) commits, Esc cancels.  `onCommit` answers an error message to show
  // under the field (the value then reverts), or null.  Used by SYSTEM's rows
  // and by the method-argument forms.
  import { onMount } from 'svelte';
  import type { TypeDescriptor } from '@/bus/systemTree';
  import Select from '@/components/ui/Select.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';

  interface Props {
    type?: TypeDescriptor;
    // The value as displayed (formatValue).
    value: string;
    readonly?: boolean;
    // Take focus on mount (an inline edit); a form field does not.
    autofocus?: boolean;
    // A form field reports every change instead of committing on Enter.
    onInput?: (text: string) => void;
    onCommit?: (text: string) => Promise<string | null> | string | null | void;
    onCancel?: () => void;
    label?: string;
  }
  let {
    type,
    value,
    readonly = false,
    autofocus = false,
    onInput,
    onCommit,
    onCancel,
    label = 'Value',
  }: Props = $props();

  // svelte-ignore state_referenced_locally
  let text = $state(value);
  let error = $state('');
  let busy = $state(false);
  let textEl = $state<HTMLInputElement | null>(null);
  let selectEl = $state<HTMLSelectElement | null>(null);
  const inputEl = $derived(textEl ?? selectEl);

  const kind = $derived(type?.kind ?? 'any');
  const choices = $derived(kind === 'enum' && type?.enum?.length ? type.enum : null);

  onMount(() => {
    if (!autofocus || !inputEl) return;
    inputEl.focus();
    if (inputEl instanceof HTMLInputElement) inputEl.select();
  });

  async function commit(t: string): Promise<void> {
    if (!onCommit || busy) return;
    busy = true;
    error = '';
    try {
      const err = await onCommit(t);
      if (err) {
        error = err;
        text = value; // revert
      }
    } finally {
      busy = false;
    }
  }

  function onKey(ev: KeyboardEvent): void {
    if (ev.key === 'Enter' && !onInput) {
      ev.preventDefault();
      ev.stopPropagation();
      void commit(text);
    } else if (ev.key === 'Escape') {
      ev.preventDefault();
      ev.stopPropagation();
      text = value;
      error = '';
      onCancel?.();
    }
  }

  function set(t: string): void {
    text = t;
    onInput?.(t);
  }
</script>

<span class="value-editor" class:has-error={!!error}>
  {#if kind === 'bool'}
    <button
      type="button"
      class="toggle"
      class:on={text === 'true'}
      role="switch"
      aria-checked={text === 'true'}
      aria-label={label}
      disabled={readonly || busy}
      onclick={() => {
        const next = text === 'true' ? 'false' : 'true';
        set(next);
        if (!onInput) void commit(next);
      }}
      onkeydown={onKey}
    >
      <span class="knob"></span>
    </button>
  {:else if choices}
    <Select
      bind:ref={selectEl}
      size="inline"
      mono
      invalid={!!error}
      aria-label={label}
      disabled={readonly || busy}
      value={text}
      onchange={(ev) => {
        const t = (ev.currentTarget as HTMLSelectElement).value;
        set(t);
        if (!onInput) void commit(t);
      }}
      onkeydown={onKey}
    >
      {#if !choices.includes(text)}
        <option value={text}>{text}</option>
      {/if}
      {#each choices as c (c)}
        <option value={c}>{c}</option>
      {/each}
    </Select>
  {:else}
    <TextInput
      bind:ref={textEl}
      size="inline"
      mono
      invalid={!!error}
      style="min-width: 8ch; max-width: 100%"
      aria-label={label}
      spellcheck="false"
      autocomplete="off"
      readonly={readonly || busy}
      value={text}
      oninput={(ev) => set((ev.currentTarget as HTMLInputElement).value)}
      onkeydown={onKey}
      onblur={() => {
        if (!onInput && !busy) onCancel?.();
      }}
    />
  {/if}
  {#if error}
    <span class="error" role="alert">{error}</span>
  {/if}
</span>

<style>
  .value-editor {
    display: inline-flex;
    flex-direction: column;
    min-width: 0;
    max-width: 100%;
    vertical-align: middle;
  }
  .error {
    color: var(--gs-syntax-error);
    font-size: var(--gs-font-size-xs);
    white-space: normal;
  }
  .toggle {
    position: relative;
    width: 26px;
    height: 14px;
    border-radius: var(--gs-radius-pill);
    border: var(--gs-border-width) solid var(--gs-border);
    background: var(--gs-surface-app);
    padding: 0;
    cursor: pointer;
  }
  .toggle.on {
    background: var(--gs-focus-ring);
  }
  .toggle:disabled {
    cursor: default;
    opacity: 0.6;
  }
  .knob {
    position: absolute;
    top: 1px;
    left: 1px;
    width: 10px;
    height: 10px;
    border-radius: var(--gs-radius-round);
    background: var(--gs-text-muted);
    transition: left var(--gs-duration-fast);
  }
  .toggle.on .knob {
    left: 13px;
    background: var(--gs-switch-knob-on);
  }
</style>
