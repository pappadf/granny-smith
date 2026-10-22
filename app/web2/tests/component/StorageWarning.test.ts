import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { bridge } from '../helpers/bridgeMock';

vi.mock('@/bus/emulator', async () => (await import('../helpers/bridgeMock')).emulatorModule());

const { checkStoragePersistence } = await import('@/bus/storageHealth');
const { storageHealth } = await import('@/state/storageHealth.svelte');
const { toasts, _resetForTests } = await import('@/state/toasts.svelte');
const StorageWarning = (await import('@/components/common/StorageWarning.svelte')).default;

// S1018-01: when the worker could not open the browser's storage it puts
// /opfs on memory.  The page must say so at once, and keep saying so, before
// the user adds anything that would then be lost.
describe('storage that will not persist', () => {
  beforeEach(() => {
    bridge.reset();
    _resetForTests();
    storageHealth.volatile = false;
  });

  it('shows a banner that stays, and an error toast, when files.persistent is false', async () => {
    bridge.reply('files.persistent', false);
    const { queryByTestId } = render(StorageWarning);
    expect(queryByTestId('storage-warning')).toBeNull();

    expect(await checkStoragePersistence()).toBe(false);
    flushSync();
    const banner = queryByTestId('storage-warning')!;
    expect(banner).not.toBeNull();
    expect(banner.getAttribute('role')).toBe('alert');
    expect(banner.textContent).toMatch(/nothing will be saved/i);
    // Nothing to dismiss it with.
    expect(banner.querySelector('button')).toBeNull();
    expect(
      toasts.active.some((t) => t.severity === 'error' && /could not be opened/.test(t.msg)),
    ).toBe(true);
  });

  it('says nothing when the storage is persistent, or the core does not know', async () => {
    const { queryByTestId } = render(StorageWarning);
    bridge.reply('files.persistent', true);
    expect(await checkStoragePersistence()).toBe(true);
    // A core without the attribute answers an error: not taken as volatile.
    bridge.reset();
    expect(await checkStoragePersistence()).toBe(true);
    flushSync();
    expect(queryByTestId('storage-warning')).toBeNull();
    expect(toasts.active).toHaveLength(0);
  });
});
