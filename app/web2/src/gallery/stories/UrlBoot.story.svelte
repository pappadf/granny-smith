<script lang="ts">
  import UrlBootView from '@/components/display/UrlBootView.svelte';
  import { urlBoot, type UrlFile } from '@/state/urlBoot.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // One row per download status.
  const files: UrlFile[] = [
    {
      slot: 'rom',
      label: 'ROM',
      name: 'Mac-IIcx.rom',
      received: 262144,
      total: 262144,
      status: 'done',
      error: null,
    },
    {
      slot: 'hd0',
      label: 'Hard disk 1',
      name: 'System7.hda',
      received: 12582912,
      total: 41943040,
      status: 'downloading',
      error: null,
    },
    {
      slot: 'hd1',
      label: 'Hard disk 2',
      name: 'Apps.hda',
      received: 5242880,
      total: null,
      status: 'downloading',
      error: null,
    },
    {
      slot: 'cd',
      label: 'CD-ROM',
      name: 'Disc.iso.zip',
      received: 734003200,
      total: 734003200,
      status: 'unpacking',
      error: null,
    },
    {
      slot: 'fd0',
      label: 'Floppy 1',
      name: 'Tools.dsk',
      received: 0,
      total: null,
      status: 'queued',
      error: null,
    },
    {
      slot: 'fd1',
      label: 'Floppy 2',
      name: 'Missing.dsk',
      received: 0,
      total: null,
      status: 'failed',
      error: 'HTTP 404 Not Found',
    },
    {
      slot: 'vrom',
      label: 'Video ROM',
      name: 'card.vrom',
      received: 0,
      total: null,
      status: 'skipped',
      error: null,
    },
  ];
  urlBoot.requested = true;
  urlBoot.showProgress = true;
  urlBoot.model = 'Macintosh IIcx';
  urlBoot.files = files;
  // svelte-ignore state_referenced_locally
  if (variant === 'failed') {
    urlBoot.stage = 'failed';
    urlBoot.error = 'The ROM could not be downloaded.';
  } else {
    urlBoot.stage = 'downloading';
  }
</script>

<div class="layer"><UrlBootView /></div>

<style>
  .layer {
    position: absolute;
    inset: 0;
    background: var(--gs-bg);
  }
</style>
