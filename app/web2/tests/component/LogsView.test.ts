import { render, waitFor } from '@testing-library/svelte';
import { describe, it, expect, beforeEach } from 'vitest';
import LogsView from '@/components/panel-views/logs/LogsView.svelte';
import { logs, appendLog, clearLogs } from '@/state/logs.svelte';

beforeEach(() => {
  clearLogs();
  logs.autoscroll = true;
  logs.catLevels = {};
});

describe('LogsView', () => {
  it('shows the empty hint when no log lines exist', () => {
    const { container } = render(LogsView);
    expect(container.querySelector('.logs-empty')).not.toBeNull();
  });

  it('renders one row per entry after appendLog', async () => {
    const { container } = render(LogsView);
    appendLog({ ts: 1, cat: 'cpu', level: 3, msg: 'A-trap' });
    appendLog({ ts: 2, cat: 'scsi', level: 1, msg: 'reset' });
    await waitFor(() => {
      const rows = container.querySelectorAll('.log-line');
      expect(rows.length).toBe(2);
    });
    const txt = container.textContent ?? '';
    expect(txt).toContain('[cpu]');
    expect(txt).toContain('A-trap');
    expect(txt).toContain('[scsi]');
    expect(txt).toContain('reset');
  });

  it('clearLogs empties the view back to the hint', async () => {
    appendLog({ ts: 1, cat: 'cpu', level: 3, msg: 'A-trap' });
    const { container } = render(LogsView);
    await waitFor(() => {
      expect(container.querySelectorAll('.log-line').length).toBe(1);
    });
    clearLogs();
    await waitFor(() => {
      expect(container.querySelectorAll('.log-line').length).toBe(0);
      expect(container.querySelector('.logs-empty')).not.toBeNull();
    });
  });

  it('scrolling up with autoscroll on unpins the list (S1018-35)', async () => {
    const { container } = render(LogsView);
    const el = container.querySelector('.logs-scroll') as HTMLElement;
    // jsdom has no layout: a scroll box whose height grows with the rows.
    let top = 0;
    const height = () => 20 * container.querySelectorAll('.log-line').length;
    Object.defineProperty(el, 'scrollHeight', { get: height, configurable: true });
    Object.defineProperty(el, 'clientHeight', { value: 100, configurable: true });
    Object.defineProperty(el, 'scrollTop', {
      get: () => top,
      set: (v: number) => (top = Math.max(0, Math.min(v, height() - 100))),
      configurable: true,
    });
    for (let i = 0; i < 20; i++) appendLog({ ts: i, cat: 'via', level: 9, msg: `line ${i}` });
    await waitFor(() => expect(top).toBe(300));
    // The user scrolls up: new lines leave the list where it is.
    el.scrollTop = 40;
    el.dispatchEvent(new Event('scroll'));
    for (let i = 20; i < 30; i++) appendLog({ ts: i, cat: 'via', level: 9, msg: `line ${i}` });
    await waitFor(() => expect(container.querySelectorAll('.log-line').length).toBe(30));
    await new Promise((r) => setTimeout(r, 50));
    expect(top).toBe(40);
    // Back at the bottom: pinned again.
    el.scrollTop = 500;
    el.dispatchEvent(new Event('scroll'));
    appendLog({ ts: 99, cat: 'via', level: 9, msg: 'last' });
    await waitFor(() => expect(top).toBe(520));
  });

  it('the status strip reflects line + category counts and autoscroll', async () => {
    appendLog({ ts: 1, cat: 'cpu', level: 3, msg: 'one' });
    appendLog({ ts: 2, cat: 'cpu', level: 3, msg: 'two' });
    appendLog({ ts: 3, cat: 'scsi', level: 1, msg: 'three' });
    const { container } = render(LogsView);
    await waitFor(() => {
      const status = container.querySelector('.logs-status')?.textContent ?? '';
      expect(status).toContain('3 lines');
      expect(status).toContain('2 categories');
      expect(status).toContain('autoscroll: on');
    });
  });
});
