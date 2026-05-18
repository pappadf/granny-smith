import { render, waitFor, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import CheckpointsView from '@/components/panel-views/checkpoints/CheckpointsView.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';
import { checkpoints } from '@/state/checkpoints.svelte';
import type { CheckpointEntry } from '@/bus/types';

class CheckpointStubOpfs extends MockOpfs {
  fixtures: CheckpointEntry[] = [];
  async scanCheckpoints(): Promise<CheckpointEntry[]> {
    return this.fixtures;
  }
}

beforeEach(() => {
  checkpoints.selectedDir = null;
  checkpoints.sortColumn = 'date';
  checkpoints.sortDir = 'desc';
});

describe('CheckpointsView', () => {
  it('renders the empty hint when no checkpoints exist', async () => {
    setOpfsBackend(new MockOpfs());
    const { container } = render(CheckpointsView);
    await waitFor(() => {
      expect(container.querySelector('.empty')?.textContent ?? '').toMatch(/No checkpoints yet/i);
    });
  });

  it('renders one row per checkpoint with name/machine/date/size cells', async () => {
    const stub = new CheckpointStubOpfs();
    stub.fixtures = [
      {
        path: '/opfs/checkpoints/aaaa-20260101T000000Z',
        dirName: 'aaaa-20260101T000000Z',
        id: 'aaaa000000000000',
        created: '20260101T000000Z',
        label: 'Before format',
        machine: 'Plus',
        sizeBytes: 4 * 1024 * 1024,
      },
      {
        path: '/opfs/checkpoints/bbbb-20260201T000000Z',
        dirName: 'bbbb-20260201T000000Z',
        id: 'bbbb000000000000',
        created: '20260201T000000Z',
        label: 'After install',
        machine: 'SE/30',
        sizeBytes: 8 * 1024 * 1024,
      },
    ];
    setOpfsBackend(stub);
    const { container } = render(CheckpointsView);
    await waitFor(() => {
      expect(container.querySelectorAll('.tr').length).toBe(2);
    });
    const allText = container.textContent ?? '';
    expect(allText).toContain('Before format');
    expect(allText).toContain('After install');
    expect(allText).toContain('Plus');
    expect(allText).toContain('SE/30');
  });

  it('right-click on a row opens a context menu with Load + Delete', async () => {
    const stub = new CheckpointStubOpfs();
    stub.fixtures = [
      {
        path: '/opfs/checkpoints/aaaa-20260101T000000Z',
        dirName: 'aaaa-20260101T000000Z',
        id: 'aaaa000000000000',
        created: '20260101T000000Z',
        label: 'X',
        machine: 'Plus',
        sizeBytes: 1,
      },
    ];
    setOpfsBackend(stub);
    const { container } = render(CheckpointsView);
    await waitFor(() => {
      expect(container.querySelector('.tr')).not.toBeNull();
    });
    await fireEvent.contextMenu(container.querySelector('.tr') as HTMLElement);
    await waitFor(() => {
      expect(document.querySelector('.context-menu')).not.toBeNull();
    });
    const items = Array.from(document.querySelectorAll('.context-menu .item')).map((e) =>
      e.textContent?.trim(),
    );
    expect(items).toContain('Load');
    expect(items).toContain('Delete');
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  });
});
