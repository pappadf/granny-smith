<script lang="ts">
  import TextInput from '@/components/ui/TextInput.svelte';
  import NumberInput from '@/components/ui/NumberInput.svelte';
  import InlineValueInput from '@/components/ui/InlineValueInput.svelte';
  import Select from '@/components/ui/Select.svelte';
  import Checkbox from '@/components/ui/Checkbox.svelte';
  import RadioGroup from '@/components/ui/RadioGroup.svelte';

  // Every field in every state.
  let radio = $state('b');
</script>

<div class="grid">
  <div class="row">
    <TextInput value="text" aria-label="sm" />
    <TextInput placeholder="placeholder" aria-label="placeholder" />
    <TextInput value="0040028E" hex widthCh={10} aria-label="hex" />
    <TextInput value="bad/name" invalid aria-label="invalid" />
    <TextInput value="disabled" disabled aria-label="disabled" />
    <TextInput value="200%" bare style="width: 48px" aria-label="bare" />
  </div>
  <div class="row">
    <TextInput size="lg" value="System 7.5.3.dsk" aria-label="lg" />
    <TextInput size="inline" mono value="inline" aria-label="inline" />
    <NumberInput value={5} min="0" max="9" style="width: 56px" aria-label="number" />
  </div>
  <div class="row">
    <InlineValueInput value="0000BEEF" widthCh={8} aria-label="reg" />
    <InlineValueInput value="00001001" widthCh={8} changed aria-label="changed" />
    <InlineValueInput value="ZZZZ" widthCh={8} invalid aria-label="invalid reg" />
    <InlineValueInput value="2700" widthCh={4} readonly aria-label="readonly" />
  </div>
  <div class="row">
    <Select aria-label="md" value="b">
      <option value="a">Macintosh Plus</option>
      <option value="b">Macintosh IIcx</option>
    </Select>
    <Select size="sm" aria-label="sm" value="w"><option value="w">write</option></Select>
    <Select size="inline" mono aria-label="inline select" value="accel">
      <option>accel</option>
    </Select>
    <Select aria-label="invalid select" invalid value="x"><option>x</option></Select>
    <Select aria-label="disabled select" disabled value="disabled"><option>disabled</option></Select
    >
  </div>
  <div class="row">
    <Checkbox label="Advanced" checked />
    <Checkbox label="unchecked" />
    <Checkbox label="disabled" disabled />
    <Checkbox size="sm" label="autoscroll" checked />
  </div>
  <RadioGroup
    name="g"
    label="Capacity"
    bind:value={radio}
    options={[
      { value: 'a', label: '800 KB (double density)' },
      { value: 'b', label: '1.4 MB (high density)' },
      { value: 'c', label: 'disabled', disabled: true },
    ]}
  />
</div>

<style>
  .grid {
    display: flex;
    flex-direction: column;
    gap: var(--gs-space-3);
    padding: var(--gs-space-3);
  }
  .row {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: var(--gs-space-2);
  }
</style>
