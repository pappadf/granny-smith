<script lang="ts">
  import DisplayToolbar from '@/components/display/DisplayToolbar.svelte';
  import { machine } from '@/state/machine.svelte';
  import { camera } from '@/state/camera.svelte';
  import { microphone } from '@/state/microphone.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // svelte-ignore state_referenced_locally
  const v = variant;
  machine.status = v === 'no-machine' ? 'no-machine' : v === 'paused' ? 'paused' : 'running';
  machine.scheduler = v === 'accel' ? 'accel' : 'live';
  // The AV buttons, one of them live.
  if (v !== 'no-machine') {
    machine.videoIn = true;
    machine.audioIn = true;
    camera.enabled = v === 'accel';
    camera.live = v === 'accel';
    microphone.enabled = v === 'paused';
  }
</script>

<DisplayToolbar />
