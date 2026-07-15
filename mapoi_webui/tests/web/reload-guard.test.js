// vitest unit tests for the SSE config_changed reload dirty guard (#373).
//
// 対象: MapoiReloadGuard.{collectReloadBlockers, decideReloadAction, buildReloadConfirmMessage}
//
// テスト方針: 「未保存編集・undo 履歴の無警告破棄」の防止は編集ロストに直結する致命核。
//   判定を DOM 非依存の pure モジュール (reload-guard.js) に切り出してここで厚く pin し、
//   app.js 側の配線 (SSE handler / debounce / 再開 hook) は手動 / e2e に委ねる。
//   あわせて「編集続行 (Cancel) 選択後の Save が #343 の 409 安全網 (conflict ダイアログ)
//   に落ちる」連鎖を thin instance の PoiEditor.save() で pin する
//   (poi-editor-history-dirty.test.js と同じ constructor 非経由パターン)。
import { describe, it, expect, vi, afterEach } from 'vitest';
import * as guard from '../../web/js/reload-guard.js';
import PoiEditor from '../../web/js/poi-editor.js';

const CLEAN_STATE = {
  poiDirty: false,
  routeDirty: false,
  tagDirty: false,
  poiFormOpen: false,
  routeFormOpen: false,
};

describe('collectReloadBlockers', () => {
  it('returns no blockers when every editor is clean and closed', () => {
    expect(guard.collectReloadBlockers(CLEAN_STATE)).toEqual([]);
  });

  it('collects each dirty editor as a blocker', () => {
    expect(guard.collectReloadBlockers({ ...CLEAN_STATE, poiDirty: true, tagDirty: true }))
      .toEqual(['POI', 'Tag']);
    expect(guard.collectReloadBlockers({ ...CLEAN_STATE, routeDirty: true }))
      .toEqual(['Route']);
  });

  it('treats an open edit form as a blocker even when not dirty yet', () => {
    // form 内の入力は [OK] を押すまで dirty にならないが、reload は form を閉じて
    // 入力を失わせる (#334 の loadRoutes guard と同じ扱い)。
    expect(guard.collectReloadBlockers({ ...CLEAN_STATE, poiFormOpen: true }))
      .toEqual(['POI']);
    expect(guard.collectReloadBlockers({ ...CLEAN_STATE, routeFormOpen: true }))
      .toEqual(['Route']);
  });
});

describe('shouldBlockUnload (#380)', () => {
  it('blocks unload while any blocker exists (collectReloadBlockers と同じ定義)', () => {
    expect(guard.shouldBlockUnload(
      guard.collectReloadBlockers({ ...CLEAN_STATE, poiDirty: true }),
    )).toBe(true);
    expect(guard.shouldBlockUnload(
      guard.collectReloadBlockers({ ...CLEAN_STATE, routeFormOpen: true }),
    )).toBe(true);
  });

  it('passes through when clean (常時ブロックはブラウザに抑止される)', () => {
    expect(guard.shouldBlockUnload(guard.collectReloadBlockers(CLEAN_STATE))).toBe(false);
    expect(guard.shouldBlockUnload([])).toBe(false);
    expect(guard.shouldBlockUnload(null)).toBe(false);
  });
});

describe('isSelfConfigChange', () => {
  it('recognizes a version any editor already holds as self-change (skip reload)', () => {
    // save 応答で受領済み = 自タブ発。reload は undo 履歴と map 視点を失わせるだけ (#384)。
    expect(guard.isSelfConfigChange('v2', ['v2', 'v1', 'v1'])).toBe(true);
    expect(guard.isSelfConfigChange('v1', ['v2', 'v1', null])).toBe(true);
  });

  it('treats an unknown version as external change (reload)', () => {
    expect(guard.isSelfConfigChange('v9', ['v1', 'v2', 'v3'])).toBe(false);
  });

  it('falls back to reload when the event carries no version (old backend / config missing)', () => {
    expect(guard.isSelfConfigChange(null, ['v1'])).toBe(false);
    expect(guard.isSelfConfigChange(undefined, ['v1'])).toBe(false);
    expect(guard.isSelfConfigChange('', ['v1'])).toBe(false);
  });

  it('never matches editors that have no version yet (null must not equal null)', () => {
    // 初期 load 前 (configVersion=null) に version 無し event が来ても self 判定しない。
    expect(guard.isSelfConfigChange(null, [null, null, null])).toBe(false);
    expect(guard.isSelfConfigChange('v1', [])).toBe(false);
    expect(guard.isSelfConfigChange('v1', undefined)).toBe(false);
  });
});

describe('decideReloadAction', () => {
  it('reloads immediately when there is no blocker', () => {
    expect(guard.decideReloadAction([], false)).toBe('reload');
  });

  it('resumes a deferred reload once all blockers are gone', () => {
    expect(guard.decideReloadAction([], true)).toBe('reload');
  });

  it('prompts on the first config_changed while blockers exist', () => {
    expect(guard.decideReloadAction(['POI'], false)).toBe('prompt');
  });

  it('holds silently (no repeated prompt) after the user chose to keep editing', () => {
    expect(guard.decideReloadAction(['POI'], true)).toBe('hold');
  });
});

describe('buildReloadConfirmMessage', () => {
  it('lists the blocked editors and offers both choices', () => {
    const msg = guard.buildReloadConfirmMessage(['POI', 'Route']);
    expect(msg).toContain('POI, Route');
    expect(msg).toContain('[OK]');
    expect(msg).toContain('[Cancel]');
  });

  it('says "edits in progress", not "unsaved edits" (open form は未 dirty でも blocker)', () => {
    const msg = guard.buildReloadConfirmMessage(['POI']);
    expect(msg).toContain('edits in progress');
    expect(msg).not.toContain('unsaved edits');
  });
});

// 編集続行 (Cancel) を選んで reload が保留されている間、poiEditor.configVersion は
// 外部 save 前の古い token のまま残る。その状態での Save が #343 の 409 安全網に
// 落ちる連鎖 (古い token 送信 → conflict → ダイアログ → 続行なら working copy 無傷)。
describe('keep-editing falls into the #343 conflict net on save', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  function makeDirtyEditor() {
    const editor = Object.create(PoiEditor.prototype);
    editor.pois = [{ name: 'edited', pose: { x: 1, y: 0, yaw: 0 } }];
    editor.dirty = true;
    editor.configVersion = 'stale-token-before-external-save';
    editor.btnSave = {};
    editor.setDirty = vi.fn((isDirty) => { editor.dirty = isDirty; });
    editor.onConflictReload = vi.fn();
    return editor;
  }

  it('sends the stale version token, gets 409, and keeps edits on Cancel', async () => {
    const editor = makeDirtyEditor();
    const savePois = vi.fn(async () => ({ ok: false, status: 409, conflict: true }));
    const confirm = vi.fn(() => false); // 409 ダイアログでも編集続行
    vi.stubGlobal('MapoiApi', { savePois });
    vi.stubGlobal('window', { confirm });

    await editor.save();

    expect(savePois).toHaveBeenCalledWith(editor.pois, 'stale-token-before-external-save');
    expect(confirm).toHaveBeenCalled();
    expect(editor.onConflictReload).not.toHaveBeenCalled();
    expect(editor.dirty).toBe(true);
  });

  it('runs the full reload via onConflictReload when the user accepts the dialog', async () => {
    const editor = makeDirtyEditor();
    vi.stubGlobal('MapoiApi', {
      savePois: vi.fn(async () => ({ ok: false, status: 409, conflict: true })),
    });
    vi.stubGlobal('window', { confirm: vi.fn(() => true) });

    await editor.save();

    expect(editor.onConflictReload).toHaveBeenCalled();
  });
});
