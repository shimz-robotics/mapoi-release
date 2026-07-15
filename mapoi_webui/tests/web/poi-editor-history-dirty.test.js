// Dirty-state regression tests for PoiEditor undo/redo restore (#300).
//
// _applySnapshot() は DOM 再描画も呼ぶが、今回 pin したいのは history cap と
// dirty 判定の関係だけ。constructor を通さず、必要な field / method だけを持つ
// thin instance で検証する。
import { describe, it, expect, vi } from 'vitest';
import PoiEditor from '../../web/js/poi-editor.js';

function poi(name, x = 0) {
  return {
    name,
    pose: { x, y: 0, yaw: 0 },
    tolerance: { xy: 0.5, yaw: 0.7854 },
    tags: ['waypoint'],
    description: '',
  };
}

function makeEditor({ pois, originalPois, undoLength = 0 }) {
  const editor = Object.create(PoiEditor.prototype);
  editor.pois = JSON.parse(JSON.stringify(pois));
  editor.originalPois = JSON.parse(JSON.stringify(originalPois));
  editor.selectedIndex = 0;
  editor.visiblePois = new Set([0]);
  editor.editingIndex = 0;
  editor.undoStack = Array.from({ length: undoLength }, (_, i) => ({ i }));
  editor.redoStack = [];
  editor.hideForm = vi.fn();
  editor.renderList = vi.fn();
  editor.setDirty = vi.fn((isDirty) => { editor.dirty = isDirty; });
  editor.onSelectionChange = vi.fn();
  editor.onVisibilityChange = vi.fn();
  return editor;
}

describe('PoiEditor undo/redo dirty restore', () => {
  it('marks clean when restored POI content matches the saved baseline, regardless of stack depth', () => {
    const saved = [poi('saved')];
    const editor = makeEditor({
      pois: [poi('edited', 1)],
      originalPois: saved,
      undoLength: 49,
    });

    editor._applySnapshot({
      pois: saved,
      selectedIndex: 0,
      visiblePois: [0],
    });

    expect(editor.setDirty).toHaveBeenCalledWith(false);
    expect(editor.dirty).toBe(false);
  });

  it('keeps dirty when restored POI content differs, even if history is at the cap length', () => {
    const saved = [poi('saved')];
    const editor = makeEditor({
      pois: saved,
      originalPois: saved,
      undoLength: 50,
    });

    editor._applySnapshot({
      pois: [poi('edited', 1)],
      selectedIndex: 0,
      visiblePois: [0],
    });

    expect(editor.undoStack).toHaveLength(50);
    expect(editor.setDirty).toHaveBeenCalledWith(true);
    expect(editor.dirty).toBe(true);
  });
});
