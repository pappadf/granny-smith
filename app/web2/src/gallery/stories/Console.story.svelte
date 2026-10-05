<script lang="ts">
  import ConsoleView from '@/components/panel-views/terminal/ConsoleView.svelte';
  import { createConsole } from '@/state/console.svelte';
  import type { StoryProps } from '../registry';

  let { variant }: StoryProps = $props();

  // A console of the gallery's own, with one entry of every kind.
  const con = createConsole();
  const m = con.model;
  m.command('machine.cpu.pc', [
    { from: 0, to: 7, cls: 'object' },
    { from: 8, to: 11, cls: 'object' },
    { from: 12, to: 14, cls: 'attribute' },
  ]);
  m.push({ kind: 'job_start', job: 1 });
  m.push({ kind: 'output', text: '0x0040028E\n', job: 1 });
  m.push({ kind: 'job_end', job: 1 });
  m.command('let n = 42 # the answer', [
    { from: 0, to: 3, cls: 'decl' },
    { from: 4, to: 5, cls: 'variable' },
    { from: 8, to: 10, cls: 'number' },
    { from: 11, to: 23, cls: 'comment' },
  ]);
  m.command('files.images', [
    { from: 0, to: 5, cls: 'object' },
    { from: 6, to: 12, cls: 'attribute' },
  ]);
  m.push({ kind: 'job_start', job: 2 });
  m.push({ kind: 'value_begin', job: 2 });
  m.push({ kind: 'output', text: '[3 items]\n', job: 2 });
  m.push({
    kind: 'value',
    job: 2,
    json: [{ object: 'image', name: 'hd0', path: 'files.images[0]' }, 'System.dsk', 42],
  });
  m.push({ kind: 'job_end', job: 2 });
  m.command('machine.cpu.frob', [
    { from: 0, to: 11, cls: 'object' },
    { from: 12, to: 16, cls: 'unknown' },
  ]);
  m.push({ kind: 'job_start', job: 3 });
  m.push({ kind: 'error', job: 3, lines: ["error: 'frob' is not a member of machine.cpu"] });
  m.push({ kind: 'job_end', job: 3 });
  m.push({ kind: 'stderr', line: 'scsi: target 2 timed out' });
  m.echo('machine.cpu.d0 = 0x1234');
  m.push({ kind: 'print', line: 'Machine paused at $0040028E' });
  m.flush();
  con.state.prompt = 'paused›';
  // svelte-ignore state_referenced_locally
  const find = variant === 'find';

  // Open the find bar (Ctrl+F) and search the output.
  $effect(() => {
    if (!find) return;
    const t = setTimeout(() => {
      const root = document.querySelector('.console');
      root?.dispatchEvent(new KeyboardEvent('keydown', { key: 'f', ctrlKey: true, bubbles: true }));
      setTimeout(() => {
        const q = document.querySelector<HTMLInputElement>('.find-input');
        if (!q) return;
        q.value = 'machine';
        q.dispatchEvent(new Event('input', { bubbles: true }));
      }, 30);
    }, 30);
    return () => clearTimeout(t);
  });
</script>

<!-- The find variant's focus belongs to its find bar: the input's own
     focus-on-open would race the bar's (CodeMirror loads asynchronously). -->
<ConsoleView console={con} autofocus={!find} />
