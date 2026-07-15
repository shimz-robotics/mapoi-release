/**
 * REST API client for mapoi_webui backend.
 */
const MapoiApi = {
  async getMaps() {
    const res = await fetch('/api/maps');
    return res.json();
  },

  async getMapMetadata(mapName) {
    const res = await fetch(`/api/maps/${encodeURIComponent(mapName)}/metadata`);
    return res.json();
  },

  getMapImageUrl(mapName) {
    return `/api/maps/${encodeURIComponent(mapName)}/image`;
  },

  async selectMap(mapName) {
    const res = await fetch('/api/editor/select-map', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ map_name: mapName }),
    });
    return res.json();
  },

  async navSwitchMap(mapName) {
    const res = await fetch('/api/nav/switch-map', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ map_name: mapName }),
    });
    return res.json();
  },

  async getPois() {
    const res = await fetch('/api/pois');
    return res.json();
  },

  /**
   * POST /api/pois (#241): 楽観的競合検出のため expected_version を必ず送る。
   * 受信側 (mapoi_webui_node) は yaml の sha256 と比較し、不一致なら 409 を返す。
   * 返値は backend JSON に { ok, status, conflict } を付加して呼び出し側に渡す:
   *   - ok === true: 通常 success (status 200)
   *   - conflict === true: version 不一致 (status 409)、reload して再試行を促す
   *   - ok === false かつ conflict !== true: その他のエラー (validation 400 / 500 等)
   * 呼び出し側は ok=false 時に `result.error` をユーザーに表示する。
   */
  async savePois(pois, expectedVersion) {
    const res = await fetch('/api/pois', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ pois, expected_version: expectedVersion }),
    });
    const body = await res.json().catch(() => ({}));
    return {
      ok: res.ok,
      status: res.status,
      conflict: res.status === 409,
      ...body,
    };
  },

  async getTagDefinitions() {
    const res = await fetch('/api/tag-definitions');
    return res.json();
  },

  /**
   * POST /api/custom-tags (#343: #241 の楽観的競合検出を custom_tags にも展開)。
   * 返値の形は savePois と同じ { ok, status, conflict, ...body } (#343)。
   */
  async saveCustomTags(customTags, expectedVersion) {
    const res = await fetch('/api/custom-tags', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ custom_tags: customTags, expected_version: expectedVersion }),
    });
    const body = await res.json().catch(() => ({}));
    return {
      ok: res.ok,
      status: res.status,
      conflict: res.status === 409,
      ...body,
    };
  },

  async getRoutes() {
    const res = await fetch('/api/routes');
    return res.json();
  },

  /**
   * POST /api/routes (#343: #241 の楽観的競合検出を routes にも展開)。
   * 返値の形は savePois と同じ { ok, status, conflict, ...body } (#343)。
   */
  async saveRoutes(routes, expectedVersion) {
    const res = await fetch('/api/routes', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ routes, expected_version: expectedVersion }),
    });
    const body = await res.json().catch(() => ({}));
    return {
      ok: res.ok,
      status: res.status,
      conflict: res.status === 409,
      ...body,
    };
  },

  async navGoal(poiName) {
    const res = await fetch('/api/nav/goal', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ poi_name: poiName }),
    });
    return res.json();
  },

  async navRoute(routeName) {
    const res = await fetch('/api/nav/route', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ route_name: routeName }),
    });
    return res.json();
  },

  async navCancel() {
    const res = await fetch('/api/nav/cancel', { method: 'POST' });
    return res.json();
  },

  async navPause() {
    const res = await fetch('/api/nav/pause', { method: 'POST' });
    return res.json();
  },

  async navResume() {
    const res = await fetch('/api/nav/resume', { method: 'POST' });
    return res.json();
  },

  async navInitialPose(poiName) {
    const res = await fetch('/api/nav/initial-pose', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ poi_name: poiName }),
    });
    return res.json();
  },

  async navStatus() {
    const res = await fetch('/api/nav/status');
    return res.json();
  },

  async mode() {
    const res = await fetch('/api/mode');
    return res.json();
  },
};

// Browser は <script> の global として `MapoiApi` を参照する。
// Node (vitest) からは `module.exports` 経由で require し、fetch 契約を検証する
// (#199 / geometry.js・poi-interactions.js と同じ #129 dual export パターン)。
if (typeof module !== 'undefined' && module.exports) {
  module.exports = MapoiApi;
}
