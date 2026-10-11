import { render, fireEvent } from '@testing-library/svelte';
import { flushSync } from 'svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import UrlBootView from '@/components/display/UrlBootView.svelte';
import DisplayContent from '@/components/display/DisplayContent.svelte';
import PreviewNoticeDialog from '@/components/dialogs/PreviewNoticeDialog.svelte';
import { machine } from '@/state/machine.svelte';
import {
  urlBoot,
  beginUrlBoot,
  queueUrlFile,
  updateUrlFile,
  setUrlBootStage,
  skipQueuedUrlFiles,
  slotLabel,
  urlMediaNameFor,
  _resetUrlBootForTests,
} from '@/state/urlBoot.svelte';
import { setOpfsBackend } from '@/bus/opfs';
import { MockOpfs } from '../helpers/mockOpfs';

beforeEach(() => {
  _resetUrlBootForTests();
  machine.status = 'no-machine';
  setOpfsBackend(new MockOpfs());
  try {
    localStorage.clear();
  } catch {
    // no storage in this environment
  }
});

describe('urlBoot state', () => {
  it('names slots and files the way the view shows them', () => {
    expect(slotLabel('rom')).toBe('ROM');
    expect(slotLabel('rom2')).toBe('ROM (second chip)');
    expect(slotLabel('vrom')).toBe('Video card ROM');
    expect(slotLabel('hd0')).toBe('Hard disk 1');
    expect(slotLabel('fd1')).toBe('Floppy disk 2');
    expect(slotLabel('cd')).toBe('CD-ROM');
  });

  it('names a listed file by its slot and time, and keeps that name', () => {
    beginUrlBoot('iici');
    queueUrlFile('hd0');
    const name = urlBoot.files[0].name;
    expect(name).toMatch(/^hd0_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}$/);
    expect(urlMediaNameFor('hd0')).toBe(name);
    expect(urlMediaNameFor('fd0')).toMatch(/^fd0_/);
  });

  it('lists files only while the page is booting from its URL', () => {
    queueUrlFile('rom');
    expect(urlBoot.files).toHaveLength(0);
    beginUrlBoot('iici');
    queueUrlFile('rom');
    expect(urlBoot.files.map((f) => f.slot)).toEqual(['rom']);
  });
});

describe('UrlBootView', () => {
  it('says a file was already stored instead of downloaded', () => {
    beginUrlBoot('iici');
    queueUrlFile('hd0');
    updateUrlFile('hd0', {
      name: 'hd0_2026-10-01_10-00-00.dmg',
      status: 'done',
      reused: true,
      received: 7864320,
      total: 7864320,
    });
    const { container } = render(UrlBootView);
    const hd = container.querySelector('[data-slot="hd0"]');
    expect(hd?.textContent).toContain('Already stored');
    expect(hd?.textContent).toContain('hd0_2026-10-01_10-00-00.dmg');
  });

  it('shows the headline and one progress bar per file', () => {
    beginUrlBoot('iici');
    queueUrlFile('rom');
    queueUrlFile('hd0');
    updateUrlFile('rom', { status: 'done', received: 524288, total: 524288 });
    updateUrlFile('hd0', { status: 'downloading', received: 13107200, total: 26214400 });
    const { container } = render(UrlBootView);
    expect(container.querySelector('.title')?.textContent).toBe('Granny Smith');
    expect(container.querySelector('.headline')?.textContent).toMatch(/Downloading/);
    const bars = container.querySelectorAll('[role="progressbar"]');
    expect(bars).toHaveLength(2);
    expect(bars[0].getAttribute('aria-valuenow')).toBe('100');
    expect(bars[1].getAttribute('aria-valuenow')).toBe('50');
    const hd = container.querySelector('[data-slot="hd0"]');
    expect(hd?.textContent).toContain(urlBoot.files[1].name);
    expect(hd?.textContent).toContain('12.5 MB of 25.0 MB · 50%');
    expect(container.querySelector('[data-slot="rom"]')?.getAttribute('data-state')).toBe('done');
  });

  it('uses an indeterminate bar when the server sends no length', () => {
    beginUrlBoot(null);
    queueUrlFile('hd0');
    updateUrlFile('hd0', { status: 'downloading', received: 3 * 1024 * 1024, total: null });
    const { container } = render(UrlBootView);
    const bar = container.querySelector('[role="progressbar"]');
    expect(bar?.hasAttribute('data-indeterminate')).toBe(true);
    expect(bar?.hasAttribute('aria-valuenow')).toBe(false);
    expect(container.querySelector('.amount')?.textContent).toContain('3.0 MB');
  });

  it('shows a downloaded disk being compressed into the store, then stored', () => {
    beginUrlBoot(null);
    queueUrlFile('hd0');
    updateUrlFile('hd0', {
      status: 'storing',
      received: 40 * 1024 * 1024,
      total: 40 * 1024 * 1024,
      stored: { done: 10 * 1024 * 1024, total: 40 * 1024 * 1024 },
    });
    const { container } = render(UrlBootView);
    const bar = container.querySelector('[role="progressbar"]');
    expect(bar?.getAttribute('aria-valuenow')).toBe('25');
    expect(container.querySelector('.amount')?.textContent).toContain('Compressing · 25%');
    // Past the last byte: checked and moved in, with nothing to report.
    updateUrlFile('hd0', { stored: { done: 40 * 1024 * 1024, total: 40 * 1024 * 1024 } });
    flushSync();
    expect(bar?.hasAttribute('data-indeterminate')).toBe(true);
    expect(container.querySelector('.amount')?.textContent).toContain('Storing…');
  });

  it('says why a failed boot stopped and offers the start screen', async () => {
    beginUrlBoot(null);
    queueUrlFile('rom');
    updateUrlFile('rom', { status: 'failed', error: 'mac_rom_archive.zip: not found' });
    setUrlBootStage('failed', 'There is no ROM to boot.');
    const { container, getByRole } = render(UrlBootView);
    expect(container.querySelector('.headline')?.textContent).toMatch(/could not be started/);
    expect(container.querySelector('.file-error')?.textContent).toContain('not found');
    await fireEvent.click(getByRole('button', { name: 'Go to the start screen' }));
    expect(urlBoot.showProgress).toBe(false);
  });

  it('marks the disks still waiting as not needed when the ROM fails', () => {
    beginUrlBoot(null);
    queueUrlFile('rom');
    queueUrlFile('hd0');
    updateUrlFile('rom', { status: 'failed', error: 'not found' });
    skipQueuedUrlFiles();
    setUrlBootStage('failed', 'There is no ROM to boot.');
    const { container } = render(UrlBootView);
    const hd = container.querySelector('[data-slot="hd0"]');
    expect(hd?.getAttribute('data-state')).toBe('skipped');
    expect(hd?.textContent).toContain('Not needed');
  });
});

describe('DisplayContent with a URL boot', () => {
  it('shows the progress view, not Welcome, until the machine runs', () => {
    beginUrlBoot('iici');
    const { container } = render(DisplayContent);
    expect(container.querySelector('[data-testid="url-boot-view"]')).not.toBeNull();
    expect(container.querySelector('.welcome-view')).toBeNull();
    machine.status = 'running';
    flushSync();
    expect(container.querySelector('[data-testid="url-boot-view"]')).toBeNull();
    expect(urlBoot.showProgress).toBe(false);
    // A later Shut Down goes back to the ordinary Welcome.
    machine.status = 'stopped';
    flushSync();
    expect(container.querySelector('.welcome-view')).not.toBeNull();
  });

  it('falls back to Welcome when a failed boot is dismissed', async () => {
    beginUrlBoot(null);
    setUrlBootStage('failed', 'There is no ROM to boot.');
    const { container, getByRole } = render(DisplayContent);
    await fireEvent.click(getByRole('button', { name: 'Go to the start screen' }));
    expect(container.querySelector('.welcome-view')).not.toBeNull();
  });
});

describe('PreviewNoticeDialog with a URL boot', () => {
  it('stays closed when the page was opened to boot from its URL', () => {
    beginUrlBoot('iici');
    const { queryByText } = render(PreviewNoticeDialog);
    expect(queryByText(/preview build/)).toBeNull();
  });

  it('opens on an ordinary first visit', () => {
    const { queryByText } = render(PreviewNoticeDialog);
    expect(queryByText(/preview build/)).not.toBeNull();
  });

  it('marks an unpacking file with its own bar state', () => {
    beginUrlBoot(null);
    queueUrlFile('hd0');
    updateUrlFile('hd0', { status: 'unpacking', received: 1024, total: 1024 });
    const { container } = render(UrlBootView);
    const bar = container.querySelector('[data-slot="hd0"] [role="progressbar"]');
    expect(bar?.getAttribute('data-state')).toBe('unpacking');
  });
});
