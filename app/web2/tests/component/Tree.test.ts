import { render, fireEvent, waitFor } from '@testing-library/svelte';
import { describe, it, expect, vi } from 'vitest';
import Tree, { type TreeNode } from '@/components/common/Tree.svelte';

describe('Tree', () => {
  it('renders one row per top-level node', () => {
    const nodes: TreeNode[] = [
      { id: 'a', label: 'Alpha', leaf: true },
      { id: 'b', label: 'Beta', leaf: true },
    ];
    const { container } = render(Tree, { nodes, expanded: {}, onToggle: () => undefined });
    const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
    expect(labels).toContain('Alpha');
    expect(labels).toContain('Beta');
  });

  it('shows a twistie on branch nodes and not on leaves', () => {
    const nodes: TreeNode[] = [
      { id: 'br', label: 'Branch', children: [{ id: 'leaf', label: 'Leaf', leaf: true }] },
      { id: 'lf', label: 'Loose Leaf', leaf: true },
    ];
    const { container } = render(Tree, { nodes, expanded: {}, onToggle: () => undefined });
    const twisties = container.querySelectorAll('.twistie.has');
    expect(twisties.length).toBe(1);
  });

  it('clicking a branch row calls onToggle with the path', async () => {
    const onToggle = vi.fn();
    const nodes: TreeNode[] = [
      { id: 'br', label: 'Branch', children: [{ id: 'leaf', label: 'Leaf', leaf: true }] },
    ];
    const { container } = render(Tree, { nodes, expanded: {}, onToggle });
    const row = container.querySelector('.tree-row') as HTMLElement;
    await fireEvent.click(row);
    expect(onToggle).toHaveBeenCalledWith(['br']);
  });

  it('renders children of expanded branches', () => {
    const nodes: TreeNode[] = [
      {
        id: 'br',
        label: 'Branch',
        children: [
          { id: 'c1', label: 'Child 1', leaf: true },
          { id: 'c2', label: 'Child 2', leaf: true },
        ],
      },
    ];
    const { container } = render(Tree, {
      nodes,
      expanded: { br: true },
      onToggle: () => undefined,
    });
    const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
    expect(labels).toContain('Child 1');
    expect(labels).toContain('Child 2');
  });

  it('uses loadChildren for lazy expansion', async () => {
    const loader = vi.fn(async (path: string[]) => {
      if (path[path.length - 1] === 'br') {
        return [{ id: 'kid', label: 'Lazy Kid', leaf: true }] as TreeNode[];
      }
      return [];
    });
    const nodes: TreeNode[] = [{ id: 'br', label: 'Branch' }];
    const { container } = render(Tree, {
      nodes,
      expanded: { br: true },
      onToggle: () => undefined,
      loadChildren: loader,
    });
    await waitFor(() => {
      const labels = Array.from(container.querySelectorAll('.label')).map((e) => e.textContent);
      expect(labels).toContain('Lazy Kid');
    });
    expect(loader).toHaveBeenCalled();
  });
});
