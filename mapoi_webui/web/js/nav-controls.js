/**
 * Navigation 操作 UI の状態派生 / DOM 適用ヘルパ (#199 frontend)。
 *
 * app.js の `updateNavControlsAvailability` / `requestNavigationMapSwitch` /
 * mobile initial state から「純粋な状態派生・分岐ロジック」と「DOM 適用」を切り出し、
 * 単体テスト (mapoi_webui/tests/web/nav-controls.test.js) で回帰検知できるようにする。
 * backend 側の契約は #279 (test_get_navigation_capabilities.py) で固めており、
 * ここはその frontend 投影 (backend_status 優先 / legacy subscriber fallback /
 * localization 独立 3-state) を対象とする。
 *
 * Browser からは `MapoiNavControls.*` のグローバルとして使い、
 * Node (vitest) からは `module.exports` 経由で import する dual export 形式
 * (api.js / poi-interactions.js と同じ #129 パターン)。
 * 純粋ヘルパ (resolveCapabilities / deriveNavControlsState / decideMapSwitchOutcome /
 * initialSectionOpenStates) は DOM / window 非依存。applyNavControlsState のみ document を取る。
 */
(function () {
  'use strict';

  /**
   * navigation payload の「前回値保持」契約 (#199)。
   * pollNavStatus が navigation を持たない応答を返した時、直前の capabilities を
   * 維持して UI をちらつかせない。app.js の `navigation || currentCapabilities` 相当。
   *
   * @param {object|null} incoming  今回届いた navigation payload
   * @param {object|null} previous  直前まで保持していた capabilities
   * @returns {object|null} 採用する capabilities
   */
  function resolveCapabilities(incoming, previous) {
    return incoming || previous || null;
  }

  function buildBackendTooltip(status, topics) {
    const lines = [];
    if (status) {
      lines.push(`backend_type: ${status.backend_type || 'unknown'}`);
      lines.push(`backend_ready: ${status.backend_ready}`);
      if (status.reason) {
        lines.push(`reason: ${status.reason}`);
      }
    } else {
      const t = topics || {};
      Object.values(t).forEach((topic) => {
        lines.push(`${topic.topic}: ${topic.subscribers}`);
      });
    }
    return lines.join('\n');
  }

  function buildLocalizationTooltip(status) {
    const lines = [];
    if (status) {
      lines.push(`backend_type: ${status.backend_type || 'unknown'}`);
      lines.push(`backend_ready: ${status.backend_ready}`);
      if (status.reason) {
        lines.push(`reason: ${status.reason}`);
      }
    } else {
      lines.push('mapoi/localization/backend_status not received yet');
    }
    return lines.join('\n');
  }

  /**
   * capabilities から navigation 操作 UI の表示状態を派生する純関数 (#199)。
   *
   * gate ルール (app.js の minimal contract / #205 review を踏襲):
   * - navOperationsEnabled: backend_status があれば backend_ready、無ければ
   *   command_available (subscriber 数) で代用 (後方互換)。navigation 操作 UI を一括 gate。
   * - localizationReady: localization_status.backend_ready のみで判定 (#209, navigation と独立)。
   * - navigationAvailable: capabilities.navigation_available をそのまま表示色に使う。
   *
   * @param {object|null} capabilities
   * @returns {{
   *   navOperationsEnabled: boolean, navigationAvailable: boolean, localizationReady: boolean,
   *   navClass: string, navLabel: string, navTooltip: string,
   *   locClass: string, locLabel: string, locTooltip: string,
   * }}
   */
  function deriveNavControlsState(capabilities) {
    const caps = capabilities || {};
    const backendStatus = caps.backend_status || null;
    const localizationStatus = caps.localization_status || null;
    const commandAvailable = !!caps.command_available;
    const navigationAvailable = !!caps.navigation_available;

    const navOperationsEnabled = backendStatus
      ? !!backendStatus.backend_ready
      : commandAvailable;
    const localizationReady = !!(localizationStatus && localizationStatus.backend_ready);

    const navClass = 'navigation-availability '
      + (navigationAvailable ? 'navigation-available' : 'navigation-unavailable');
    const navLabel = navigationAvailable ? 'Navigation connected' : 'Navigation unavailable';
    const navTooltip = buildBackendTooltip(backendStatus, caps.topics);

    // localization は navigation の class 名を流用しつつ unknown を含む 3-state。
    let locClass = 'navigation-availability ';
    let locLabel;
    if (!localizationStatus) {
      locClass += 'navigation-unknown';
      locLabel = 'Localization unknown';
    } else if (localizationStatus.backend_ready) {
      locClass += 'navigation-available';
      locLabel = 'Localization connected';
    } else {
      locClass += 'navigation-unavailable';
      locLabel = 'Localization unavailable';
    }
    const locTooltip = buildLocalizationTooltip(localizationStatus);

    return {
      navOperationsEnabled,
      navigationAvailable,
      localizationReady,
      navClass,
      navLabel,
      navTooltip,
      locClass,
      locLabel,
      locTooltip,
    };
  }

  // navOperationsEnabled で一括 enable/disable する操作ボタン群。
  const NAV_OPERATION_BUTTON_IDS = [
    'btn-nav-go',
    'btn-nav-run',
    'btn-nav-pause',
    'btn-nav-resume',
    'btn-nav-stop',
  ];

  /**
   * deriveNavControlsState の結果を DOM へ適用する (#199)。
   * document を引数に取り、jsdom 上で最小 DOM を組んで検証できるようにする。
   *
   * @param {ReturnType<typeof deriveNavControlsState>} state
   * @param {Document} doc
   */
  function applyNavControlsState(state, doc) {
    const d = doc || (typeof document !== 'undefined' ? document : null);
    if (!d) return;

    const navBody = d.getElementById('nav-body');
    if (navBody) {
      navBody.classList.toggle('navigation-unavailable-state', !state.navigationAvailable);
    }

    const navAvail = d.getElementById('navigation-availability');
    const navAvailText = d.getElementById('navigation-availability-text');
    if (navAvail && navAvailText) {
      navAvail.className = state.navClass;
      navAvailText.textContent = state.navLabel;
      navAvail.title = state.navTooltip;
    }

    const locAvail = d.getElementById('localization-availability');
    const locAvailText = d.getElementById('localization-availability-text');
    if (locAvail && locAvailText) {
      locAvail.className = state.locClass;
      locAvailText.textContent = state.locLabel;
      locAvail.title = state.locTooltip;
    }

    NAV_OPERATION_BUTTON_IDS.forEach((id) => {
      const btn = d.getElementById(id);
      if (btn) btn.disabled = !state.navOperationsEnabled;
    });

    // Set Initial Pose 系は localization 側 (#209) で gate。
    const initialPoseBtn = d.getElementById('btn-nav-setpose');
    if (initialPoseBtn) initialPoseBtn.disabled = !state.localizationReady;
    const initialPoseSelect = d.getElementById('nav-initialpose-select');
    if (initialPoseSelect) initialPoseSelect.disabled = !state.localizationReady;

    // Operator map switch も bridge 経由なので navigation 操作 gate に従う。
    const navMapSelect = d.getElementById('navigation-map-select');
    if (navMapSelect) navMapSelect.disabled = !state.navOperationsEnabled;
  }

  /**
   * navSwitchMap の応答から後続アクションを決める純関数 (#199)。
   * - error: warning 表示 + 両 select を current map に戻す
   * - warning: warning 表示 + navigation 側 select のみ戻す (header の Map: は据え置き)
   * - switching: 受理。status を map_switching に進める
   *
   * @param {{error?: string, warning?: string}|null} result
   * @returns {{kind: 'error'|'warning'|'switching', message: string, revert: 'both'|'nav'|'none'}}
   */
  function decideMapSwitchOutcome(result) {
    const r = result || {};
    if (r.error) {
      return { kind: 'error', message: 'Map switch failed: ' + r.error, revert: 'both' };
    }
    if (r.warning) {
      return { kind: 'warning', message: r.warning, revert: 'nav' };
    }
    return { kind: 'switching', message: '', revert: 'none' };
  }

  /**
   * compact viewport 初期表示でどの section を開くか (#199)。
   * - Navigation: 常に open (操作系は最優先で見せる)
   * - Routes / POIs: desktop は open、compact は閉じて地図領域を確保
   * - Tags: 常に閉じる (編集時のみ開く副次 UI)
   *
   * @param {boolean} isCompact compactMediaQuery.matches
   * @returns {{nav: boolean, route: boolean, poi: boolean, tag: boolean}}
   */
  function initialSectionOpenStates(isCompact) {
    return {
      nav: true,
      route: !isCompact,
      poi: !isCompact,
      tag: false,
    };
  }

  const api = {
    resolveCapabilities,
    deriveNavControlsState,
    applyNavControlsState,
    decideMapSwitchOutcome,
    initialSectionOpenStates,
  };

  if (typeof window !== 'undefined') {
    window.MapoiNavControls = window.MapoiNavControls || {};
    Object.assign(window.MapoiNavControls, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
