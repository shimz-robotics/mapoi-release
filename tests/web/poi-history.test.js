// vitest unit tests for the pure undo/redo history-stack helpers in
// mapoi_webui/web/js/poi-history.js (#300).
//
// 対象: MapoiPoiHistory.{recordChange, stepUndo, stepRedo}
//
// テスト方針: Undo/Redo の正しさ (LIFO 順序 / redo 無効化 / 容量制限 / 空 stack no-op) は
//   編集データの取り違え・ロストに直結する致命核。DOM 非依存の純 stack ロジックに切り出した
//   ここを厚く pin し、poi-editor.js 側の配線 (capture 点 / restore 再描画) は手動 / e2e に委ねる。
import { describe, it, expect } from 'vitest';
import * as history from '../../web/js/poi-history.js';

describe('recordChange', () => {
  it('pushes the snapshot onto undoStack', () => {
    const undo = [];
    const redo = [];
    history.recordChange(undo, redo, 's0');
    expect(undo).toEqual(['s0']);
    expect(redo).toEqual([]);
  });

  it('clears the redo stack (a new edit invalidates the redo future)', () => {
    const undo = ['a'];
    const redo = ['x', 'y'];
    history.recordChange(undo, redo, 'b');
    expect(undo).toEqual(['a', 'b']);
    expect(redo).toEqual([]);
  });

  it('enforces the cap by dropping the oldest entries', () => {
    const undo = [];
    const redo = [];
    history.recordChange(undo, redo, 's0', 2);
    history.recordChange(undo, redo, 's1', 2);
    history.recordChange(undo, redo, 's2', 2);
    // s0 (oldest) dropped, newest kept
    expect(undo).toEqual(['s1', 's2']);
  });

  it('is unbounded when cap is omitted or non-positive', () => {
    const undo = [];
    const redo = [];
    for (let i = 0; i < 5; i += 1) history.recordChange(undo, redo, `s${i}`);
    expect(undo).toHaveLength(5);
    const undo0 = [];
    for (let i = 0; i < 5; i += 1) history.recordChange(undo0, [], `s${i}`, 0);
    expect(undo0).toHaveLength(5);
  });
});

describe('stepUndo', () => {
  it('returns null and does not mutate when undoStack is empty', () => {
    const undo = [];
    const redo = [];
    expect(history.stepUndo(undo, redo, 'current')).toBeNull();
    expect(undo).toEqual([]);
    expect(redo).toEqual([]);
  });

  it('moves current onto redoStack and returns the popped snapshot', () => {
    const undo = ['s0', 's1'];
    const redo = [];
    const restored = history.stepUndo(undo, redo, 's2');
    expect(restored).toBe('s1');
    expect(undo).toEqual(['s0']);
    expect(redo).toEqual(['s2']);
  });
});

describe('stepRedo', () => {
  it('returns null and does not mutate when redoStack is empty', () => {
    const undo = ['s0'];
    const redo = [];
    expect(history.stepRedo(undo, redo, 'current')).toBeNull();
    expect(undo).toEqual(['s0']);
    expect(redo).toEqual([]);
  });

  it('moves current onto undoStack and returns the popped snapshot (mirror of undo)', () => {
    const undo = ['s0'];
    const redo = ['s2'];
    const restored = history.stepRedo(undo, redo, 's1');
    expect(restored).toBe('s2');
    expect(undo).toEqual(['s0', 's1']);
    expect(redo).toEqual([]);
  });
});

describe('integration: capture-before-mutate editor sequence', () => {
  // poi-editor.js の使い方を再現する小さなドライバ。state は変異後の値、record は
  // 「変異直前の state」を積む。undo/redo は返ってきた snapshot を state に適用する。
  function makeDriver(cap) {
    const undo = [];
    const redo = [];
    let state = 's0';
    return {
      undo, redo,
      get state() { return state; },
      edit(next) { history.recordChange(undo, redo, state, cap); state = next; },
      stepBack() { const s = history.stepUndo(undo, redo, state); if (s !== null) state = s; return s; },
      stepFwd() { const s = history.stepRedo(undo, redo, state); if (s !== null) state = s; return s; },
    };
  }

  it('undo reverts edits one step at a time, then no-ops at the baseline', () => {
    const d = makeDriver();
    d.edit('s1');
    d.edit('s2');
    expect(d.state).toBe('s2');
    d.stepBack();
    expect(d.state).toBe('s1');
    d.stepBack();
    expect(d.state).toBe('s0');
    expect(d.stepBack()).toBeNull(); // nothing left to undo
    expect(d.state).toBe('s0');
  });

  it('redo re-applies undone edits, then no-ops', () => {
    const d = makeDriver();
    d.edit('s1');
    d.edit('s2');
    d.stepBack();
    d.stepBack();
    expect(d.state).toBe('s0');
    d.stepFwd();
    expect(d.state).toBe('s1');
    d.stepFwd();
    expect(d.state).toBe('s2');
    expect(d.stepFwd()).toBeNull(); // nothing left to redo
    expect(d.state).toBe('s2');
  });

  it('a new edit after undo discards the redo future', () => {
    const d = makeDriver();
    d.edit('s1');
    d.edit('s2');
    d.stepBack(); // back to s1, redo future = [s2]
    expect(d.state).toBe('s1');
    expect(d.redo).toEqual(['s2']);
    d.edit('s9'); // new edit diverges
    expect(d.state).toBe('s9');
    expect(d.redo).toEqual([]); // s2 future gone
    expect(d.stepFwd()).toBeNull();
    // undo now returns to the divergence point, not the discarded s2 line
    d.stepBack();
    expect(d.state).toBe('s1');
  });
});
