import { render } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import DisplayContent from '@/components/display/DisplayContent.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';
import { setOpfsBackend, MockOpfs } from '@/bus/opfs';

beforeEach(() => {
  machine.status = 'no-machine';
  setOpfsBackend(new MockOpfs());
  stopDriveActivityMock();
});

describe('DisplayContent routing', () => {
  // Phase 3 always mounts ScreenView (so bus.emulator.bootstrap can hand
  // Emscripten a stable canvas reference). Welcome is layered on top via
  // `.welcome-layer` until a machine is running.

  it('layers the Welcome view on top when no machine is running', () => {
    const { container } = render(DisplayContent);
    expect(container.querySelector('.welcome-layer')).not.toBeNull();
    expect(container.querySelector('.screen-view')).not.toBeNull();
  });

  it('hides the Welcome layer when a machine is running', () => {
    machine.status = 'running';
    const { container } = render(DisplayContent);
    expect(container.querySelector('.welcome-layer')).toBeNull();
    expect(container.querySelector('.screen-view')).not.toBeNull();
  });

  it('layers Welcome again after Shut Down (status=stopped)', () => {
    machine.status = 'stopped';
    const { container } = render(DisplayContent);
    expect(container.querySelector('.welcome-layer')).not.toBeNull();
    expect(container.querySelector('.screen-view')).not.toBeNull();
  });
});
