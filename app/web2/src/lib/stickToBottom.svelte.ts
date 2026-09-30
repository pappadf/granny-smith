// Auto-scroll for a growing log: while the element is scrolled to the
// bottom, new content keeps it there; a scroll the user makes away from
// the bottom unpins it until they come back (or follow() is called).
//
// Content that skips layout off screen (content-visibility) can grow after
// a jump to the bottom without any scroll event, so a jump is repeated each
// frame until the height settles; a resize of the element (a growing
// neighbour, the panel) re-pins too.  Only a scroll the element did not
// make itself can unpin it.

import { tick } from 'svelte';
import type { Attachment } from 'svelte/attachments';

export interface StickToBottom {
  // For {@attach}: on the scrolling element.
  readonly attach: Attachment<HTMLElement>;
  // Back to the bottom, pinned, whatever the user scrolled.
  follow(): void;
}

// Frames a jump is repeated for while the height keeps changing.
const SETTLE_FRAMES = 6;
// Closer than this to the bottom counts as at the bottom (px).
const BOTTOM_SLACK = 8;

// `content` is read reactively: each change re-pins while stuck.
export function stickToBottom(content: () => unknown = () => undefined): StickToBottom {
  let el: HTMLElement | null = null;
  let stick = true;
  let pinnedTop = -1; // scrollTop after the element's own last jump
  let frame = 0;

  function pin(): void {
    if (frame) cancelAnimationFrame(frame);
    frame = 0;
    let frames = SETTLE_FRAMES;
    let lastHeight = -1;
    const jump = () => {
      frame = 0;
      if (!el || !stick) return;
      el.scrollTop = el.scrollHeight;
      pinnedTop = el.scrollTop;
      // Again next frame, until the height stops changing.
      if (el.scrollHeight !== lastHeight && frames-- > 0) {
        lastHeight = el.scrollHeight;
        frame = requestAnimationFrame(jump);
      }
    };
    jump();
  }

  function onScroll(): void {
    if (!el) return;
    if (el.scrollHeight - el.scrollTop - el.clientHeight < BOTTOM_SLACK) stick = true;
    else if (Math.abs(el.scrollTop - pinnedTop) > 1) stick = false;
  }

  const attach: Attachment<HTMLElement> = (node) => {
    el = node;
    node.addEventListener('scroll', onScroll);
    const ro =
      typeof ResizeObserver === 'undefined'
        ? null
        : new ResizeObserver(() => {
            if (stick) pin();
          });
    ro?.observe(node);
    $effect(() => {
      content();
      if (stick) void tick().then(pin);
    });
    return () => {
      node.removeEventListener('scroll', onScroll);
      ro?.disconnect();
      if (frame) cancelAnimationFrame(frame);
      frame = 0;
      if (el === node) el = null;
    };
  };

  return {
    attach,
    follow() {
      stick = true;
      pin();
    },
  };
}
