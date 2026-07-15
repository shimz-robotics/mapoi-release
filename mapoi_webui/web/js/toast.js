/**
 * Minimal toast (auto-dismissing corner notification) helper (#354).
 *
 * Used to surface `mapoi/nav/command_rejected` SSE events without touching the
 * latched `mapoi/nav/status` snapshot: a rejected command (e.g. a typo goal
 * sent while the robot is already navigating) shows a small message in the
 * corner of the screen for a few seconds instead of only appearing in the
 * ROS logs.
 *
 * Minimal stacking model: each call appends a new toast element to a shared
 * container (`#toast-container`, created lazily); multiple toasts simply pile
 * up and each removes itself independently after its own timer expires. There
 * is no dedup / replace-in-place logic — a burst of rejects shows a burst of
 * toasts, which is acceptable for the operator-facing use case here.
 *
 * Browser からは `MapoiToast.*` のグローバルとして使い、Node (vitest) からは
 * `module.exports` 経由で import する dual export 形式 (ui-settings.js と同じ pattern)。
 */
(function () {
  'use strict';

  const DEFAULT_DURATION_MS = 5000;
  const CONTAINER_ID = 'toast-container';

  /** `#toast-container` を取得、無ければ作って `doc.body` に追加する (idempotent)。 */
  function ensureContainer(doc) {
    if (!doc || !doc.body) return null;
    let container = doc.getElementById(CONTAINER_ID);
    if (!container) {
      container = doc.createElement('div');
      container.id = CONTAINER_ID;
      doc.body.appendChild(container);
    }
    return container;
  }

  /**
   * Append a toast with `message` to the container and remove it again after
   * `durationMs` (default 5s). Returns the created element (mainly useful for
   * tests); returns `null` as a no-op if `doc` is falsy.
   */
  function showToast(doc, message, durationMs) {
    if (!doc) return null;
    const container = ensureContainer(doc);
    if (!container) return null;
    const duration = typeof durationMs === 'number' ? durationMs : DEFAULT_DURATION_MS;

    const el = doc.createElement('div');
    el.className = 'toast';
    // toast が唯一の通知経路なのでスクリーンリーダーにも届くようにする (#354 review low)。
    el.setAttribute('role', 'status');
    el.setAttribute('aria-live', 'polite');
    el.textContent = message;
    container.appendChild(el);

    if (typeof setTimeout === 'function') {
      setTimeout(() => {
        if (el.parentNode) el.parentNode.removeChild(el);
      }, duration);
    }
    return el;
  }

  const api = { DEFAULT_DURATION_MS, ensureContainer, showToast };

  if (typeof window !== 'undefined') {
    window.MapoiToast = window.MapoiToast || {};
    Object.assign(window.MapoiToast, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
