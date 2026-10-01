<script lang="ts">
  import ValueEditor from '@/components/common/ValueEditor.svelte';
  import type { TypeDescriptor } from '@/bus/systemTree';

  // One editor per kind: text, number, enum and the boolean switch.
  const td = (kind: string, en: string[] | null = null): TypeDescriptor => ({
    kind,
    width: 0,
    presentation: null,
    enum: en,
  });
  const text = td('string');
  const num = td('int');
  const en = td('enum', ['live', 'accel', 'turbo']);
  const bool = td('bool');
</script>

<div class="grid">
  <span>text</span><ValueEditor type={text} value="Macintosh HD" label="name" />
  <span>number</span><ValueEditor type={num} value="42" label="count" />
  <span>enum</span><ValueEditor type={en} value="accel" label="mode" />
  <span>bool on</span><ValueEditor type={bool} value="true" label="enabled" />
  <span>bool off</span><ValueEditor type={bool} value="false" label="disabled" />
  <span>readonly</span><ValueEditor type={text} value="read-only" readonly label="ro" />
</div>

<style>
  .grid {
    display: grid;
    grid-template-columns: 80px 1fr;
    gap: 6px 12px;
    align-items: center;
    padding: 12px;
  }
</style>
