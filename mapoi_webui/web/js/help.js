/**
 * Help overlay の開閉配線 (#391)。
 *
 * 本文は index.html の静的 modal (`#help-overlay`)。ショートカット registry からの
 * 自動生成は現段階では過剰なのでしない (必要になったら別 issue)。ここは
 * 開く (`#btn-help` click) / 閉じる (`#btn-help-close` click・背景 click) と
 * open 状態の公開 (`isOpen`) だけを持つ。
 *
 * キーボード配線 (`?` で開く / Escape で閉じる) は app.js の document keydown
 * funnel 側 (#300/#374/#375/#390 と同じ)。特に Escape は「help open が最優先」で
 * フォーム Cancel 等の段階挙動より先に判定する必要があり、優先順位を 1 箇所で
 * 見渡せるよう funnel に置く。help.js は状態 (`isOpen`) を提供するだけ。
 *
 * Browser からは `MapoiHelp.*` のグローバル、Node (vitest) からは `module.exports`
 * の dual export 形式 (toast.js / ui-settings.js と同じ pattern)。
 */
(function () {
  'use strict';

  const OVERLAY_ID = 'help-overlay';

  function overlayEl(doc) {
    return (doc || document).getElementById(OVERLAY_ID);
  }

  /** overlay が存在し表示中か。app.js の Escape 優先判定が毎 keydown で呼ぶ。 */
  function isOpen(doc) {
    const el = overlayEl(doc);
    return !!el && !el.classList.contains('hidden');
  }

  function open(doc) {
    const el = overlayEl(doc);
    if (el) el.classList.remove('hidden');
  }

  function close(doc) {
    const el = overlayEl(doc);
    if (el) el.classList.add('hidden');
  }

  /**
   * `#btn-help` (開く) / `#btn-help-close` (閉じる) / 背景 click (閉じる) を配線する。
   * `#help-overlay` が無い環境 (unit test の jsdom など) では何もしない。
   */
  function initDom(doc) {
    const overlay = overlayEl(doc);
    if (!overlay) return;

    const btnOpen = doc.getElementById('btn-help');
    if (btnOpen) btnOpen.addEventListener('click', () => open(doc));

    const btnClose = doc.getElementById('btn-help-close');
    if (btnClose) btnClose.addEventListener('click', () => close(doc));

    // 背景 click で閉じる。dialog 内の click は overlay まで bubble しても
    // target が overlay 自身でないため閉じない (click-through 誤閉じ防止)。
    overlay.addEventListener('click', (e) => {
      if (e.target === overlay) close(doc);
    });
  }

  const api = { isOpen, open, close, initDom };

  if (typeof window !== 'undefined') {
    window.MapoiHelp = window.MapoiHelp || {};
    Object.assign(window.MapoiHelp, api);

    if (typeof document !== 'undefined' && document.addEventListener) {
      const boot = () => initDom(document);
      if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', boot);
      } else {
        boot();
      }
    }
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
