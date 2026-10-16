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
  it('creates a blank floppy (800 KB default) via files.fd_create', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'files.fd_create' ? true : null));
    const onCreated = vi.fn();
    const { getByText } = render(CreateImageDialog, {
      open: true,
      kind: 'fd',
      onClose: () => {},
      onCreated,
    });
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('files.fd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/fd\/blank_800K_\d+\.dsk$/);
    expect(cp[1][1]).toBe(false); // 800K → not high-density
    expect(onCreated).toHaveBeenCalledWith(expect.stringMatching(/^blank_800K_\d+\.dsk$/));
  });

  it('creates a 1.4 MB floppy when high density is selected', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'files.fd_create' ? true : null));
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
    const cp = callTo('files.fd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/fd\/blank_1440K_\d+\.dsk$/);
    expect(cp[1][1]).toBe(true);
  });

  const scsiDisks = [
    {
      label: '20 MB (HD20SC)',
      method: 'files.hd_create',
      arg: '21411840',
      name: 'blank_20MB',
      ext: '.dmg',
    },
    {
      label: '38 MB (HD40SC)',
      method: 'files.hd_create',
      arg: '40061952',
      name: 'blank_38MB',
      ext: '.dmg',
    },
  ];

  it("lists the bus's blank disks and creates the chosen one with its method", async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'files.hd_create' ? true : null));
    const onCreated = vi.fn();
    const { getByText, container } = render(CreateImageDialog, {
      open: true,
      kind: 'hd',
      disks: scsiDisks,
      onClose: () => {},
      onCreated,
    });
    expect(container.textContent).toContain('38 MB (HD40SC)');
    await fireEvent.click(container.querySelector('input[value="1"]') as HTMLElement);
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('files.hd_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/hd\/blank_38MB_\d+\.dmg$/);
    expect(cp[1][1]).toBe('40061952');
  });

  it('creates a ProFile image when that is what the bus takes', async () => {
    gsEvalMock.mockImplementation(async (p: string) =>
      p === 'files.profile_create' ? true : null,
    );
    const onCreated = vi.fn();
    const { getByText } = render(CreateImageDialog, {
      open: true,
      kind: 'hd',
      disks: [
        {
          label: '5 MB ProFile (9728 blocks)',
          method: 'files.profile_create',
          arg: '9728',
          name: 'blank_profile_5MB',
          ext: '.image',
        },
      ],
      onClose: () => {},
      onCreated,
    });
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(onCreated).toHaveBeenCalled());
    const cp = callTo('files.profile_create')!;
    expect(cp[1][0]).toMatch(/^\/opfs\/images\/hd\/blank_profile_5MB_\d+\.image$/);
    expect(cp[1][1]).toBe('9728');
  });

  it('says so for a device that takes no hard disk', () => {
    const { container, getByText } = render(CreateImageDialog, {
      open: true,
      kind: 'hd',
      onClose: () => {},
      onCreated: () => {},
    });
    expect(container.textContent).toContain('This device takes no hard disk');
    expect((getByText('Create').closest('button') as HTMLButtonElement).disabled).toBe(true);
  });

  it('shows an error and does not fire onCreated when creation fails', async () => {
    gsEvalMock.mockImplementation(async (p: string) => (p === 'files.fd_create' ? false : null));
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

  it("shows the core's reason for a failed create", async () => {
    gsEvalMock.mockImplementation(async (p: string) =>
      p === 'files.fd_create' ? { error: 'quota exceeded' } : null,
    );
    const { getByText, container } = render(CreateImageDialog, {
      open: true,
      kind: 'fd',
      onClose: () => {},
      onCreated: () => {},
    });
    await fireEvent.click(getByText('Create'));
    await waitFor(() => expect(container.textContent).toContain('quota exceeded'));
  });
});
