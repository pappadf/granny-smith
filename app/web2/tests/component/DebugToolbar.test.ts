import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, vi, beforeEach } from 'vitest';
import DebugToolbar from '@/components/panel-views/debug/DebugToolbar.svelte';
import { machine } from '@/state/machine.svelte';

// Mock the bus/debug surface — the toolbar should call these functions.
const calls: Record<string, number> = {};
vi.mock('@/bus/debug', () => ({
  continueExec: vi.fn(async () => {
    calls.continueExec = (calls.continueExec ?? 0) + 1;
  }),
  pauseExec: vi.fn(async () => {
    calls.pauseExec = (calls.pauseExec ?? 0) + 1;
  }),
  stepInto: vi.fn(async () => {
    calls.stepInto = (calls.stepInto ?? 0) + 1;
  }),
  stepOver: vi.fn(async () => {
    calls.stepOver = (calls.stepOver ?? 0) + 1;
  }),
  stopMachine: vi.fn(async () => {
    calls.stopMachine = (calls.stopMachine ?? 0) + 1;
  }),
  restart: vi.fn(async () => {
    calls.restart = (calls.restart ?? 0) + 1;
  }),
}));

beforeEach(() => {
  for (const k of Object.keys(calls)) delete calls[k];
  machine.status = 'paused';
});

describe('DebugToolbar', () => {
  it('renders Continue when machine is paused', () => {
    machine.status = 'paused';
    const { container } = render(DebugToolbar);
    const btn = container.querySelector('button[title="Continue"]');
    expect(btn).not.toBeNull();
    expect(container.querySelector('button[title="Pause"]')).toBeNull();
  });

  it('renders Pause when machine is running', () => {
    machine.status = 'running';
    const { container } = render(DebugToolbar);
    expect(container.querySelector('button[title="Pause"]')).not.toBeNull();
    expect(container.querySelector('button[title="Continue"]')).toBeNull();
  });

  it('Step Into / Step Over are disabled while running', () => {
    machine.status = 'running';
    const { container } = render(DebugToolbar);
    expect(
      (container.querySelector('button[title="Step Into"]') as HTMLButtonElement).disabled,
    ).toBe(true);
    expect(
      (container.querySelector('button[title="Step Over"]') as HTMLButtonElement).disabled,
    ).toBe(true);
  });

  it('Continue click invokes continueExec', async () => {
    machine.status = 'paused';
    const { container } = render(DebugToolbar);
    await fireEvent.click(container.querySelector('button[title="Continue"]') as HTMLElement);
    expect(calls.continueExec).toBe(1);
  });

  it('Stop click invokes stopMachine', async () => {
    const { container } = render(DebugToolbar);
    await fireEvent.click(container.querySelector('button[title="Stop"]') as HTMLElement);
    expect(calls.stopMachine).toBe(1);
  });
});
