import { render } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import DisplayContent from '@/components/display/DisplayContent.svelte';
import { machine, stopDriveActivityMock } from '@/state/machine.svelte';

beforeEach(() => {
  machine.status = 'no-machine';
  stopDriveActivityMock();
});

describe('DisplayContent routing', () => {
  it('shows the Welcome view when no machine is running', () => {
    const { container } = render(DisplayContent);
    expect(container.querySelector('.welcome-view')).not.toBeNull();
    expect(container.querySelector('.screen-view')).toBeNull();
  });

  it('shows the Screen view when a machine is running', () => {
    machine.status = 'running';
    const { container } = render(DisplayContent);
    expect(container.querySelector('.screen-view')).not.toBeNull();
    expect(container.querySelector('.welcome-view')).toBeNull();
  });

  it('shows the Welcome view again after Shut Down (status=stopped)', () => {
    machine.status = 'stopped';
    const { container } = render(DisplayContent);
    expect(container.querySelector('.welcome-view')).not.toBeNull();
    expect(container.querySelector('.screen-view')).toBeNull();
  });
});
