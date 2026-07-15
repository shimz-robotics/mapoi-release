/**
 * Pure decision helpers guarding the SSE config_changed full reload (#373).
 *
 * app.js の scheduleConfigChangedReload() → loadMaps() は全 working copy
 * (POI / route / tag) をサーバ最新で置き換える。dirty な editor や開いている
 * edit form がある時に黙って走ると、未保存編集・undo 履歴を無警告で破棄する。
 * 「reload を保留して conflict ダイアログ相当の選択を出す」ための判定を
 * この pure モジュールに集約し、単体テスト
 * (mapoi_webui/tests/web/reload-guard.test.js) で回帰検知する
 * (poi-history.js #300 と同じ dual-export パターン)。
 *
 * 編集続行 (Cancel) を選んだ場合、各 editor の configVersion は据え置かれる
 * ので、後続の Save は #343 の 409 安全網 (version_mismatch conflict
 * ダイアログ) にそのまま落ちる。
 *
 * Browser からは `MapoiReloadGuard.*` のグローバルとして使い、Node (vitest)
 * からは `module.exports` 経由で import する (DOM / window 非依存)。
 */
(function () {
  'use strict';

  /**
   * reload が破棄してしまうユーザー作業 (blocker) を人間可読ラベルで列挙する。
   *
   * dirty (未保存変更) だけでなく、開いている edit form も blocker に含める:
   * form 内の入力は [OK] を押すまで dirty にならないが、reload は form を
   * 閉じて入力を失わせるため (#334 の loadRoutes guard と同じ扱い)。
   *
   * @param {object} state
   * @param {boolean} state.poiDirty POI editor に未保存変更がある
   * @param {boolean} state.routeDirty route editor に未保存変更がある
   * @param {boolean} state.tagDirty tag editor に未保存変更がある
   * @param {boolean} state.poiFormOpen POI edit form が開いている
   * @param {boolean} state.routeFormOpen route edit form が開いている
   * @returns {string[]} confirm メッセージに列挙するラベル (空なら即 reload 可)
   */
  function collectReloadBlockers(state) {
    const blockers = [];
    if (state.poiDirty || state.poiFormOpen) blockers.push('POI');
    if (state.routeDirty || state.routeFormOpen) blockers.push('Route');
    if (state.tagDirty) blockers.push('Tag');
    return blockers;
  }

  /**
   * config_changed 受信 (debounce 後) に取るべき action を決める。
   *
   * - 'reload': blocker なし。即 full reload してよい (保留中だった分の再開を含む)
   * - 'prompt': blocker あり・未保留。confirm でユーザーに選ばせる
   * - 'hold'  : blocker あり・保留中 (一度「編集続行」を選択済)。再 prompt せず
   *             黙って保留を続ける (Save 時に #343 の 409 安全網が改めて警告する)
   *
   * @param {string[]} blockers collectReloadBlockers() の結果
   * @param {boolean} deferred 既に「編集続行」で reload を保留中か
   * @returns {'reload'|'prompt'|'hold'}
   */
  function decideReloadAction(blockers, deferred) {
    if (blockers.length === 0) return 'reload';
    return deferred ? 'hold' : 'prompt';
  }

  /**
   * config_changed が自タブ発 (または取り込み済み) の変更かを判定する (#384)。
   *
   * version は yaml 全体の内容 hash (#241 の config_version)。save 応答か前回 load で
   * このタブが既に知っている値なら working copy は最新で、reload は undo 履歴
   * (loadPois が破棄) と map 視点 (loadMap の fitBounds) を失わせるだけなので skip
   * してよい。version が取れない event (旧 backend / config 不在) は常に false =
   * 従来どおり reload に回す (安全側)。
   *
   * @param {string|null|undefined} version SSE payload の config_version
   * @param {Array<string|null>} knownVersions 各 editor が保持中の configVersion
   * @returns {boolean} true なら reload 不要 (自タブ発 / 取り込み済み)
   */
  function isSelfConfigChange(version, knownVersions) {
    if (!version) return false;
    return (knownVersions || []).indexOf(version) !== -1;
  }

  /**
   * beforeunload (#380) でタブ閉じ / リロードをブロックすべきかの判定。
   *
   * blocker の定義は collectReloadBlockers と同一 — 「reload で失われるもの」と
   * 「unload で失われるもの」は同じ (dirty 3 editor + 開いている edit form)。
   * この同一性は今後も維持する前提: unload だけの別条件が必要になったら、この
   * 関数に分岐を足すのではなく別の判定関数を切ること。
   * clean (blocker なし) 時に必ず素通しさせるのが不変条件: 常時ブロックすると
   * ブラウザがダイアログ自体を抑止するようになる + UX が悪い。
   *
   * @param {string[]} blockers collectReloadBlockers() の結果
   * @returns {boolean} true なら e.preventDefault() + e.returnValue でブロック
   */
  function shouldBlockUnload(blockers) {
    return (blockers || []).length > 0;
  }

  /**
   * prompt 用メッセージ。#343 の 409 conflict ダイアログと同じ言い回しに揃える。
   * blocker は「未保存変更」だけでなく「開いている edit form」も含むため、
   * "unsaved edits" と断定せず "edits in progress" と表現する。
   *
   * @param {string[]} blockers collectReloadBlockers() の結果 (1 件以上)
   * @returns {string} window.confirm に渡すメッセージ
   */
  function buildReloadConfirmMessage(blockers) {
    return (
      'The YAML file was changed outside this page.\n'
      + 'You have edits in progress: ' + blockers.join(', ') + '.\n'
      + '\n[OK] Load the latest file. Edits in progress and undo history will be lost.'
      + '\n[Cancel] Keep editing. Reload stays postponed; Save will warn on conflict.'
    );
  }

  const api = {
    collectReloadBlockers,
    decideReloadAction,
    isSelfConfigChange,
    shouldBlockUnload,
    buildReloadConfirmMessage,
  };

  if (typeof window !== 'undefined') {
    window.MapoiReloadGuard = window.MapoiReloadGuard || {};
    Object.assign(window.MapoiReloadGuard, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
