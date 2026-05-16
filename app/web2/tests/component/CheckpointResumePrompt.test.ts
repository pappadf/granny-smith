import { render, fireEvent } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import CheckpointResumePrompt from '@/components/dialogs/CheckpointResumePrompt.svelte';
import {
  checkpointPrompt,
  showCheckpointPrompt,
  resolveCheckpointPrompt,
} from '@/state/checkpointPrompt.svelte';

beforeEach(() => {
  checkpointPrompt.shown = false;
});

describe('CheckpointResumePrompt', () => {
  it('renders nothing when not shown', () => {
    const { container } = render(CheckpointResumePrompt);
    expect(container.querySelector('.modal-backdrop')).toBeNull();
  });

  it('renders modal when checkpointPrompt.shown is true', () => {
    checkpointPrompt.shown = true;
    const { container } = render(CheckpointResumePrompt);
    expect(container.querySelector('.modal-card')).not.toBeNull();
    expect(container.querySelector('.modal-title')?.textContent).toContain('checkpoint');
    const buttons = container.querySelectorAll('button');
    expect(buttons.length).toBe(2);
  });

  it('Resume button resolves the prompt with true', async () => {
    const promise = showCheckpointPrompt();
    const { container } = render(CheckpointResumePrompt);
    const resumeBtn = container.querySelector('.btn-primary') as HTMLButtonElement;
    await fireEvent.click(resumeBtn);
    await expect(promise).resolves.toBe(true);
  });

  it('Start-fresh button resolves the prompt with false', async () => {
    const promise = showCheckpointPrompt();
    const { container } = render(CheckpointResumePrompt);
    const freshBtn = container.querySelector('.btn-secondary') as HTMLButtonElement;
    await fireEvent.click(freshBtn);
    await expect(promise).resolves.toBe(false);
  });

  it('resolveCheckpointPrompt(false) hides the modal', () => {
    checkpointPrompt.shown = true;
    resolveCheckpointPrompt(false);
    expect(checkpointPrompt.shown).toBe(false);
  });
});
