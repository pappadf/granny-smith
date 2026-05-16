import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import DropOverlay from '@/components/display/DropOverlay.svelte';

// We don't drive bus/upload from here — the overlay just needs to not
// crash if dataTransfer.getData isn't a real DataTransfer.
vi.mock('@/bus/upload', () => ({
  processDataTransfer: vi.fn(() => Promise.resolve()),
}));

function makeFilesDT(): DataTransfer {
  // jsdom doesn't fully implement DataTransfer; the overlay only needs
  // `types` to include 'Files' and getData to return something.
  return {
    types: ['Files'],
    getData: () => '',
    files: [] as unknown as FileList,
  } as unknown as DataTransfer;
}

function nonFileDT(): DataTransfer {
  return {
    types: ['text/plain'],
    getData: () => '',
    files: [] as unknown as FileList,
  } as unknown as DataTransfer;
}

describe('DropOverlay', () => {
  it('is hidden by default (idle state)', () => {
    const { container } = render(DropOverlay);
    expect(container.querySelector('.drop-overlay')).toBeNull();
  });

  it('ignores dragenter without Files in the transfer', async () => {
    const { container } = render(DropOverlay);
    await fireEvent.dragEnter(document, { dataTransfer: nonFileDT() });
    expect(container.querySelector('.drop-overlay')).toBeNull();
  });

  it('hides on drop / dragend', async () => {
    const { container } = render(DropOverlay);
    // dragenter then over-display
    await fireEvent.dragEnter(document, { dataTransfer: makeFilesDT() });
    // Simulate dragend
    await fireEvent.dragEnd(document);
    expect(container.querySelector('.drop-overlay')).toBeNull();
  });

  it('hides on out-of-viewport dragover (viewport exit)', async () => {
    const { container } = render(DropOverlay);
    await fireEvent.dragEnter(document, { dataTransfer: makeFilesDT() });
    // Force a dragover with negative client coords → viewport-exit transition.
    await fireEvent.dragOver(document, {
      dataTransfer: makeFilesDT(),
      clientX: -1,
      clientY: -1,
    });
    expect(container.querySelector('.drop-overlay')).toBeNull();
  });
});
