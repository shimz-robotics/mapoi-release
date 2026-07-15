/**
 * Pure undo/redo history-stack helpers for the POI editor (#300).
 *
 * poi-editor.js の Undo/Redo コアを DOM / インスタンス非依存の純関数に切り出し、
 * 単体テスト (mapoi_webui/tests/web/poi-history.test.js) で回帰検知できるようにする
 * (poi-interactions.js / poi-filter.js #273 と同じ dual-export パターン)。
 *
 * 扱う state は不透明な「snapshot」(呼び出し側が deep clone した plain object) で、
 * このモジュールは push / pop の順序と redo 無効化・容量制限だけを責務とする。
 * snapshot の中身 (pois / selectedIndex / visiblePois) や clone 方法は poi-editor.js 側。
 *
 * Browser からは `MapoiPoiHistory.*` のグローバルとして使い、Node (vitest) からは
 * `module.exports` 経由で import する (Leaflet / DOM / window 非依存)。
 */
(function () {
  'use strict';

  /**
   * 変異直前の snapshot を記録する (capture-before-mutate)。
   *
   * - undoStack に snapshot を push する
   * - redoStack をクリアする (新しい編集が入った時点で redo の未来は無効化される —
   *   この単一 choke point に集約することで per-op の付け忘れを防ぐ)
   * - cap が正の数なら、それを超えた古い entry を先頭から drop する
   *
   * 両 stack を in-place で破壊的に更新する (poi-editor.js が保持する this.undoStack /
   * this.redoStack をそのまま渡す前提)。
   *
   * @param {Array} undoStack
   * @param {Array} redoStack
   * @param {*} snapshot 変異直前の状態 (呼び出し側で deep clone 済)
   * @param {number} [cap] undoStack の最大長 (省略 / 0 以下なら無制限)
   */
  function recordChange(undoStack, redoStack, snapshot, cap) {
    undoStack.push(snapshot);
    redoStack.length = 0;
    if (typeof cap === 'number' && cap > 0) {
      while (undoStack.length > cap) {
        undoStack.shift();
      }
    }
  }

  /**
   * 1 手戻す。undoStack が空なら null を返し何もしない。
   *
   * currentSnapshot (= 現在の状態) を redoStack に積み、undoStack の先端を pop して
   * 「復元すべき snapshot」として返す。呼び出し側はこの戻り値を state に適用する。
   *
   * @param {Array} undoStack
   * @param {Array} redoStack
   * @param {*} currentSnapshot 現在の状態 (呼び出し側で deep clone 済)
   * @returns {*|null} 復元すべき snapshot、戻せない時は null
   */
  function stepUndo(undoStack, redoStack, currentSnapshot) {
    if (undoStack.length === 0) {
      return null;
    }
    redoStack.push(currentSnapshot);
    return undoStack.pop();
  }

  /**
   * 1 手やり直す。redoStack が空なら null を返し何もしない (stepUndo の鏡像)。
   *
   * @param {Array} undoStack
   * @param {Array} redoStack
   * @param {*} currentSnapshot 現在の状態 (呼び出し側で deep clone 済)
   * @returns {*|null} 復元すべき snapshot、やり直せない時は null
   */
  function stepRedo(undoStack, redoStack, currentSnapshot) {
    if (redoStack.length === 0) {
      return null;
    }
    undoStack.push(currentSnapshot);
    return redoStack.pop();
  }

  const api = {
    recordChange,
    stepUndo,
    stepRedo,
  };

  if (typeof window !== 'undefined') {
    window.MapoiPoiHistory = window.MapoiPoiHistory || {};
    Object.assign(window.MapoiPoiHistory, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
