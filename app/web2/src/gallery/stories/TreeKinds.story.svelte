<script lang="ts">
  import TreeItem from '@/components/ui/TreeItem.svelte';
  import ValueEditor from '@/components/common/ValueEditor.svelte';

  // The SYSTEM tree's and the command browser's rows, drawn with TreeItem as
  // the views draw them: SYSTEM's compact rows with their value column, the
  // command browser's member kinds, a category row and the filter states.
</script>

<div class="cols" role="tree" tabindex="0">
  <ul class="sys">
    <li class="kind-object">
      <TreeItem depth={0} density="compact" hover={false} hasChildren open kind="object">
        {#snippet content()}<span class="name">machine</span>{/snippet}
      </TreeItem>
    </li>
    <li class="kind-attr">
      <TreeItem depth={1} density="compact" hover={false} kind="attr" selected>
        {#snippet content()}<span class="name attr">model</span><span class="value"
            >"Macintosh IIcx"</span
          >{/snippet}
      </TreeItem>
    </li>
    <li class="kind-attr">
      <TreeItem depth={1} density="compact" hover={false} kind="attr">
        {#snippet content()}<span class="name attr">sound</span><span class="value"
            ><ValueEditor
              type={{ kind: 'bool', width: 1, presentation: null, enum: null }}
              value="true"
              label="sound"
            /></span
          >{/snippet}
      </TreeItem>
    </li>
    <li class="kind-attr">
      <TreeItem depth={1} density="compact" hover={false} kind="attr">
        {#snippet content()}<span class="name attr">ram</span><span class="value"
            ><ValueEditor
              type={{ kind: 'uint', width: 32, presentation: null, enum: null }}
              value="8388608"
              label="ram"
            /></span
          >{/snippet}
      </TreeItem>
    </li>
  </ul>
  <ul class="cmd">
    <li>
      <TreeItem depth={0} variant="category" kind="section" hasChildren open label="Commands" />
    </li>
    <li>
      <TreeItem depth={0} kind="method" filter="match">
        {#snippet content()}<span class="gs-tree-item__label name method">objects</span><span
            class="doc">List child objects</span
          >{/snippet}
      </TreeItem>
    </li>
    <li>
      <TreeItem depth={0} kind="attr" selected>
        {#snippet content()}<span class="gs-tree-item__label name attr">pc</span><span class="doc"
            >Program counter</span
          >{/snippet}
      </TreeItem>
    </li>
    <li>
      <TreeItem depth={0} kind="alias" filter="dim">
        {#snippet content()}<span class="gs-tree-item__label name alias">ls</span><span class="doc"
            >files.ls</span
          >{/snippet}
      </TreeItem>
    </li>
    <li>
      <TreeItem depth={0} kind="keyword">
        {#snippet content()}<span class="gs-tree-item__label name keyword">let</span><span
            class="doc">Declare a variable</span
          >{/snippet}
      </TreeItem>
    </li>
  </ul>
</div>

<style>
  .cols {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: var(--gs-space-4);
    padding: var(--gs-space-2) 0;
    font-size: var(--gs-font-size-sm);
  }
  .cols:focus-visible {
    outline: none;
  }
  ul {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .name {
    flex: none;
  }
  .attr {
    color: var(--gs-syntax-attribute);
    font-family: var(--gs-font-mono);
  }
  .method {
    color: var(--gs-syntax-method);
    font-family: var(--gs-font-mono);
  }
  .alias {
    color: var(--gs-syntax-alias);
    font-family: var(--gs-font-mono);
  }
  .keyword {
    color: var(--gs-syntax-keyword);
    font-family: var(--gs-font-mono);
  }
  .value {
    font-family: var(--gs-font-mono);
    display: inline-flex;
    align-items: center;
  }
  .doc {
    color: var(--gs-syntax-dim);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
</style>
