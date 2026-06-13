import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';

const { gsEvalMock } = vi.hoisted(() => ({ gsEvalMock: vi.fn() }));
vi.mock('@/bus/emulator', async (importOriginal) => {
  const actual = await importOriginal<typeof import('@/bus/emulator')>();
  return { ...actual, gsEval: (p: string, a?: unknown[]) => gsEvalMock(p, a) };
});

import CreateImageDialog from '@/components/display/CreateImageDialog.svelte';

beforeEach(() => {
  gsEvalMock.mockReset();
});

function callTo(name: string) {
  return gsEvalMock.mock.calls.find((c) => c[0] === name);
}

describe('CreateImageDialog', () => {
  it('creates a blank floppy (800 KB default) via storage.fd_create', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'storage.fd_create' ? true : null));
    const onCreated = vi.fn();
    const { getByText } = render(CreateImageDialog, {
      open: true,
      kind: 'fd',
      onClose: () => {},
      onCreated,
    });
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('storage.fd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/fd\/blank_800K_\d+\.dsk$/);
    expect(cp[1][1]).toBe(false); // 800K → not high-density
    expect(onCreated).toHaveBeenCalledWith(expect.stringMatching(/^blank_800K_\d+\.dsk$/));
  });

  it('creates a 1.4 MB floppy when high density is selected', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'storage.fd_create' ? true : null));
    const onCreated = vi.fn();
    const { getByText, container } = render(CreateImageDialog, {
      open: true,
      kind: 'fd',
      onClose: () => {},
      onCreated,
    });
    await fireEvent.click(container.querySelector('input[value="1440K"]') as HTMLElement);
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('storage.fd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/fd\/blank_1440K_\d+\.dsk$/);
    expect(cp[1][1]).toBe(true);
  });

  it('lists HD sizes from scsi.hd_models and creates via storage.hd_create', async () => {
    gsEvalMock.mockImplementation(async (p: string) => {
      if (p === 'scsi.hd_models') {
        return [
          JSON.stringify({ label: 'HD20SC', vendor: 'X', product: 'Y', size: 21411840 }),
          JSON.stringify({ label: 'HD40SC', vendor: 'X', product: 'Y', size: 40061952 }),
        ];
      }
      if (p === 'storage.hd_create') return true;
      return null;
    });
    const onCreated = vi.fn();
    const { getByText, container } = render(CreateImageDialog, {
      open: true,
      kind: 'hd',
      onClose: () => {},
      onCreated,
    });
    await waitFor(() => expect(container.textContent).toContain('HD40SC'));
    // Pick the 40 MB drive, then create.
    await fireEvent.click(container.querySelector('input[value="40061952"]') as HTMLElement);
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('storage.hd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/hd\/blank_38MB_\d+\.img$/); // 40061952 ≈ 38 MiB
    expect(cp[1][1]).toBe('40061952');
  });

  it('shows an error with Retry — and does not spin — when the catalog is unavailable', async () => {
    // gsEval returns null while the module is still starting. The old
    // hdModels.length-keyed $effect re-triggered itself on every reassignment
    // and froze the tab in a microtask loop; the modelsState machine must
    // settle in 'error' instead (this test completing at all proves no spin).
    gsEvalMock.mockResolvedValue(null);
    const { container, getByText } = render(CreateImageDialog, {
      open: true,
      kind: 'hd',
      onClose: () => {},
      onCreated: () => {},
    });
    await waitFor(() => expect(container.textContent).toContain('Could not load drive sizes'));
    // Retry with the module now "ready" loads the catalog.
    gsEvalMock.mockImplementation(async (p: string) =>
      p === 'scsi.hd_models' ? [JSON.stringify({ label: 'HD20SC', size: 21411840 })] : null,
    );
    await fireEvent.click(getByText('Retry'));
    await waitFor(() => expect(container.textContent).toContain('HD20SC'));
  });

  it('shows an error and does not fire onCreated when creation fails', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'storage.fd_create' ? false : null));
    const onCreated = vi.fn();
    const { getByText, container } = render(CreateImageDialog, {
      open: true,
      kind: 'fd',
      onClose: () => {},
      onCreated,
    });
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(container.textContent).toContain('Failed to create'));
    expect(onCreated).not.toHaveBeenCalled();
  });
});
