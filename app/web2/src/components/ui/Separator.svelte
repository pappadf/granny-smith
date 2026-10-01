<script lang="ts">
  // A hairline between groups: vertical in a toolbar, horizontal in a form
  // or menu.  `class` carries legacy hooks (sep, form-divider).
  interface Props {
    orientation?: 'vertical' | 'horizontal';
    class?: string;
  }
  let { orientation = 'vertical', class: cls = '' }: Props = $props();
</script>

<div
  class="gs-separator {cls}"
  data-orientation={orientation}
  role="separator"
  aria-orientation={orientation}
></div>

<style>
  .gs-separator {
    flex: none;
    background: var(--gs-separator);
  }
  .gs-separator[data-orientation='vertical'] {
    width: var(--gs-border-width);
    height: var(--gs-separator-length);
    margin: 0 var(--gs-space-2);
  }
  .gs-separator[data-orientation='horizontal'] {
    height: var(--gs-border-width);
    margin: var(--gs-space-1-5) 0;
  }
</style>
