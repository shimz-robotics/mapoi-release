/**
 * UI 表示設定: 半透明化 (#323) と UI オン/オフトグル (#324)。
 *
 * 提供する 3 つの設定 (いずれも localStorage 永続、既定は現行の見た目を維持):
 *  - hidden : UI (header / #poi-panel / resizer) を一括で表示/非表示。地図を
 *             全画面で見たい / フィールドで片手で素早く隠したい時用 (#324)。
 *  - overlay: #poi-panel を地図の上にフロート表示する opt-in モード。docked
 *             (既定) では横に並ぶだけで地図は透けないが、overlay + 半透明で
 *             初めて「地図が UI 越しに透けて見える」(#323) が成立する。
 *  - alpha  : パネル/ヘッダ背景の不透明度 (0.3〜1.0)。frosted glass
 *             (backdrop-filter) と組み合わせる。既定 1.0 = 不透明 = 現行。
 *
 * UI 配置 (PR #325 レビューで再設計): 地図上のフローティングは UI トグル
 * (eye SVG) 1 個のみ。overlay / alpha の設定はパネル内 Display section に置く
 * (UI が見えている時にしか意味がない設定のため)。◑/⚙ グリフはダークモード/
 * アプリ設定と誤読されるため不採用。
 *
 * Browser からは `MapoiUiSettings.*` のグローバルとして使い、Node (vitest)
 * からは `module.exports` 経由で import する dual export 形式。pure helper
 * (clampAlpha / read/write/applySettings 等) は DOM/window 非依存。DOM 配線
 * (initDom) は #ui-controls が存在する実ページでのみ走り、unit test 環境
 * (jsdom, #ui-controls 無し) では no-op になる。
 *
 * localStorage 読み書きは private mode 等で例外を投げ得るため全て try/catch。
 */
(function () {
  'use strict';

  const STORAGE_KEYS = {
    hidden: 'mapoi_ui_hidden',
    overlay: 'mapoi_ui_overlay',
    alpha: 'mapoi_ui_panel_alpha',
  };

  const ALPHA_MIN = 0.3;
  const ALPHA_MAX = 1.0;
  const ALPHA_DEFAULT = 1.0;

  function clampAlpha(value) {
    const n = Number(value);
    if (!Number.isFinite(n)) return ALPHA_DEFAULT;
    return Math.max(ALPHA_MIN, Math.min(ALPHA_MAX, n));
  }

  function alphaToPercent(alpha) {
    return Math.round(clampAlpha(alpha) * 100);
  }

  function percentToAlpha(percent) {
    const p = Number(percent);
    if (!Number.isFinite(p)) return ALPHA_DEFAULT;
    return clampAlpha(p / 100);
  }

  function toBool(raw) {
    return raw === true || raw === 'true' || raw === '1';
  }

  /** 任意の raw オブジェクトを正規化した settings に落とす (defaults 補完)。 */
  function normalizeSettings(raw) {
    const src = raw || {};
    return {
      hidden: toBool(src.hidden),
      overlay: toBool(src.overlay),
      alpha: clampAlpha(src.alpha === undefined || src.alpha === null ? ALPHA_DEFAULT : src.alpha),
    };
  }

  /** localStorage-like から設定を読む。null/失敗時は defaults。 */
  function readSettings(storage) {
    const out = { hidden: false, overlay: false, alpha: ALPHA_DEFAULT };
    if (!storage) return out;
    try {
      const h = storage.getItem(STORAGE_KEYS.hidden);
      if (h !== null) out.hidden = toBool(h);
      const o = storage.getItem(STORAGE_KEYS.overlay);
      if (o !== null) out.overlay = toBool(o);
      const a = storage.getItem(STORAGE_KEYS.alpha);
      if (a !== null) out.alpha = clampAlpha(parseFloat(a));
    } catch (_) {
      // private mode / cookies blocked: defaults のまま
    }
    return out;
  }

  /** settings を localStorage-like に書く。失敗は無視。 */
  function writeSettings(storage, settings) {
    if (!storage) return;
    const s = normalizeSettings(settings);
    try {
      storage.setItem(STORAGE_KEYS.hidden, s.hidden ? 'true' : 'false');
      storage.setItem(STORAGE_KEYS.overlay, s.overlay ? 'true' : 'false');
      storage.setItem(STORAGE_KEYS.alpha, String(s.alpha));
    } catch (_) {
      // 永続化できない環境では skip
    }
  }

  /**
   * settings を body 要素に反映する:
   *  - `ui-hidden` / `ui-overlay` class を toggle
   *  - CSS 変数 `--ui-panel-alpha` を設定 (body 配下の header/panel に cascade)
   */
  function applySettings(bodyEl, settings) {
    if (!bodyEl || !bodyEl.classList) return;
    const s = normalizeSettings(settings);
    bodyEl.classList.toggle('ui-hidden', s.hidden);
    bodyEl.classList.toggle('ui-overlay', s.overlay);
    if (bodyEl.style && typeof bodyEl.style.setProperty === 'function') {
      bodyEl.style.setProperty('--ui-panel-alpha', String(s.alpha));
    }
  }

  const api = {
    STORAGE_KEYS,
    ALPHA_MIN,
    ALPHA_MAX,
    ALPHA_DEFAULT,
    clampAlpha,
    alphaToPercent,
    percentToAlpha,
    normalizeSettings,
    readSettings,
    writeSettings,
    applySettings,
  };

  // ---- DOM 配線 (実ページのみ) --------------------------------------------

  function safeLocalStorage() {
    try {
      return typeof localStorage !== 'undefined' ? localStorage : null;
    } catch (_) {
      return null;
    }
  }

  function initDom(doc, storage) {
    if (!doc || !doc.body) return;
    // #ui-controls が無い環境 (unit test の jsdom など) では何もしない。
    const controls = doc.getElementById('ui-controls');
    if (!controls) return;

    const body = doc.body;
    const toggleBtn = doc.getElementById('btn-ui-toggle');
    const displayToggleBtn = doc.getElementById('btn-display-toggle');
    const displayBody = doc.getElementById('display-body');
    const overlayChk = doc.getElementById('ui-overlay-toggle');
    const alphaSlider = doc.getElementById('ui-alpha-slider');
    const alphaValue = doc.getElementById('ui-alpha-value');

    const win = doc.defaultView || (typeof window !== 'undefined' ? window : null);

    const settings = readSettings(storage);
    applySettings(body, settings);

    function persist() {
      writeSettings(storage, settings);
    }

    // hidden / overlay の切替は #map-container のサイズを変える。Leaflet は
    // window resize を listen して invalidateSize するので (sidebar-resizer と同様)、
    // 露出した領域が gray tile のまま残らないよう resize を明示 dispatch する。
    function notifyMapResize() {
      if (win && typeof win.dispatchEvent === 'function' && typeof Event === 'function') {
        try {
          win.dispatchEvent(new Event('resize'));
        } catch (_) {
          // Event 構築不可な環境では skip
        }
      }
    }

    // アイコンは固定の ☰ (#390、index.html 側)。状態は aria-pressed (青ハイライト) と
    // title だけを同期し、icon swap はしない — Gmail/YouTube の sidebar トグルと同じ流儀。
    // pressed は「UI 表示中」(#449): ☰ は「UI を出す」記号として読まれるため
    // 表示中 = 青が直感に合い、ロックボタンの「既定状態で点灯」とも揃う。
    function syncToggleBtn() {
      if (!toggleBtn) return;
      toggleBtn.setAttribute('aria-pressed', settings.hidden ? 'false' : 'true');
      toggleBtn.title = settings.hidden ? 'Show UI (U)' : 'Hide UI (U)';
    }

    function syncAlphaControls() {
      const pct = alphaToPercent(settings.alpha);
      if (alphaSlider) alphaSlider.value = String(pct);
      if (alphaValue) alphaValue.textContent = pct + '%';
    }

    // Display section の開閉 (既定折畳)。app.js setupSectionToggle と同じ
    // display:none + ▼/▶ glyph の挙動を module 独立で再現する。開閉状態は
    // 他 section と同様に永続しない。
    let displayOpen = false;
    function applyDisplayOpen() {
      if (displayBody) displayBody.style.display = displayOpen ? '' : 'none';
      if (displayToggleBtn) displayToggleBtn.innerHTML = displayOpen ? '▼' : '▶';
    }

    // 初期反映
    if (overlayChk) overlayChk.checked = settings.overlay;
    syncAlphaControls();
    syncToggleBtn();
    applyDisplayOpen();

    if (toggleBtn) {
      toggleBtn.addEventListener('click', () => {
        settings.hidden = !settings.hidden;
        applySettings(body, settings);
        persist();
        syncToggleBtn();
        notifyMapResize();
      });
    }

    if (displayToggleBtn && displayBody) {
      displayToggleBtn.addEventListener('click', () => {
        displayOpen = !displayOpen;
        applyDisplayOpen();
      });
    }

    if (overlayChk) {
      overlayChk.addEventListener('change', () => {
        settings.overlay = overlayChk.checked;
        applySettings(body, settings);
        persist();
        notifyMapResize();
      });
    }

    if (alphaSlider) {
      alphaSlider.addEventListener('input', () => {
        settings.alpha = percentToAlpha(alphaSlider.value);
        applySettings(body, settings);
        syncAlphaControls();
      });
      // input 連発中は書かず、確定 (change) で 1 回永続化
      alphaSlider.addEventListener('change', persist);
    }
  }

  if (typeof window !== 'undefined') {
    window.MapoiUiSettings = window.MapoiUiSettings || {};
    Object.assign(window.MapoiUiSettings, api);

    if (typeof document !== 'undefined' && document.addEventListener) {
      const boot = () => initDom(document, safeLocalStorage());
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
