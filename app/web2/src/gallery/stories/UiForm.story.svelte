<script lang="ts">
  import FormGrid from '@/components/ui/FormGrid.svelte';
  import Field from '@/components/ui/Field.svelte';
  import Select from '@/components/ui/Select.svelte';
  import TextInput from '@/components/ui/TextInput.svelte';
  import Separator from '@/components/ui/Separator.svelte';
  import Button from '@/components/ui/Button.svelte';
</script>

<div class="frame">
  <FormGrid>
    <Field label="Machine Model" for="g-model">
      <Select id="g-model" value="a"><option value="a">Macintosh IIcx</option></Select>
    </Field>
    <Field
      help="The monitor is plugged into the card, so the built-in video port is left unconnected."
    />
    <Field label="RAM" for="g-ram">
      <Select id="g-ram" value="8"><option value="8">8 MB</option></Select>
    </Field>
    <Separator orientation="horizontal" />
    <Field label="Name" for="g-name" error="Name cannot contain slashes">
      <TextInput id="g-name" value="bad/name" invalid />
    </Field>
    <Field label="New name" for="g-stacked" stacked>
      <TextInput id="g-stacked" size="lg" value="System 7.5.3.dsk" />
    </Field>
    <div class="actions"><Button size="lg" variant="primary">Start Machine</Button></div>
  </FormGrid>
</div>

<style>
  .frame {
    width: 520px;
    padding: var(--gs-space-4);
  }
  .actions {
    display: flex;
    justify-content: flex-end;
  }
</style>
