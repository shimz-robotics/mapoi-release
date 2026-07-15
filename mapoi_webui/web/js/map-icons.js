/**
 * map-viewer.js から切り出したマーカー/アイコン生成ヘルパー (#346)。
 *
 * POI 矢印 / ロボット / route 方向 / route landmark バッジの SVG アイコン生成と、
 * タグ色・route 色の解決を集約する。MapViewer インスタンスの state (this.map,
 * this.poiMarkers 等) には一切依存せず、呼び出し側 (map-viewer.js) が必要な値
 * (tagColors 等) を引数で渡す。
 *
 * `getPoiColor` / `getRouteColor` / `routeDirectionDeg` / `createRouteDirectionSvg` は
 * Leaflet 非依存の純関数。`createArrowIcon` / `createRobotIcon` / `createRouteLandmarkIcon`
 * は `L.divIcon` (Leaflet) を呼ぶため、ブラウザ (Leaflet ロード後) でのみ実行できる —
 * import 自体は Node (vitest) からも安全に行えるが、これらの関数呼び出しは jsdom 環境では
 * `L` 未定義でエラーになる (geometry.js / poi-filter.js 等の既存 dual-export モジュールとの
 * 違い)。
 *
 * Browser からは `MapoiMapIcons.*` のグローバルとして使い、Node (vitest) からは
 * `module.exports` 経由で import する dual export 形式 (#129 パターン踏襲)。
 */
(function () {
  'use strict';

  /**
   * Tag-based color for POI markers.
   * @param {Object} tagColors tag_name -> color map (MapViewer#tagColors)
   * @param {Object} poi POI object ({ tags: [...] } 想定)
   */
  function getPoiColor(tagColors, poi) {
    const tags = poi.tags || [];
    for (const tag of tags) {
      if (tagColors[tag]) return tagColors[tag];
    }
    return '#3498db';
  }

  /**
   * Get the color for a route by its index.
   */
  function getRouteColor(routeIdx) {
    const palette = ['#2980b9', '#8e44ad', '#16a085', '#d35400', '#c0392b', '#7f8c8d'];
    if (routeIdx < palette.length) return palette[routeIdx];
    // HSL fallback: distribute hues avoiding palette hues
    const usedHues = [207, 282, 168, 24, 4, 210]; // approx hues of palette colors
    const n = routeIdx - palette.length;
    const hue = (n * 47 + 30) % 360; // step by golden-angle-ish offset, start at 30
    return `hsl(${hue}, 55%, 45%)`;
  }

  /**
   * Compute CSS rotation (degrees) for a route direction marker (teardrop pointing UP by default).
   * from/to は { lat, lng } を持つオブジェクト (Leaflet LatLng でなくてもよい)。
   */
  function routeDirectionDeg(from, to) {
    const screenAngle = Math.atan2(to.lat - from.lat, to.lng - from.lng) * (180 / Math.PI);
    return 90 - screenAngle;
  }

  /**
   * Create an SVG chevron (>) direction marker.
   * Default orientation: tip points UP (^). Rotated by rotDeg.
   * Open V-shape with no fill — lightweight and unambiguous.
   */
  function createRouteDirectionSvg(color, rotDeg, size) {
    const s = size || 18;
    return `<svg width="${s}" height="${s}" viewBox="0 0 16 16" style="transform: rotate(${rotDeg}deg);">` +
      `<path d="M4 12 L8 4 L12 12" fill="none" stroke="${color}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>` +
      `<path d="M4 12 L8 4 L12 12" fill="none" stroke="#fff" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" opacity="0.4"/>` +
      `<path d="M4 12 L8 4 L12 12" fill="none" stroke="${color}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>` +
      `</svg>`;
  }

  /**
   * Create an SVG arrow icon for a POI marker.
   * The arrow points in the yaw direction (ROS: 0=+x, pi/2=+y).
   * Leaflet 依存 (`L.divIcon`)。
   */
  function createArrowIcon(color, yaw, highlight) {
    const strokeColor = highlight ? '#e67e22' : '#fff';
    const strokeWidth = highlight ? 2.5 : 1.5;
    // SVG arrow pointing UP; CSS rotation converts yaw to visual direction.
    // yaw=0 → right(+x) → rotate 90° CW from up
    const rotDeg = 90 - (yaw * 180 / Math.PI);
    const svg = `<svg width="32" height="32" viewBox="0 0 32 32" style="transform: rotate(${rotDeg}deg);">` +
      `<path d="M16 2 L27 24 L16 18 L5 24 Z" fill="${color}" fill-opacity="0.85" stroke="${strokeColor}" stroke-width="${strokeWidth}" stroke-linejoin="round"/>` +
      `</svg>`;
    return L.divIcon({
      className: 'poi-arrow-icon',
      html: svg,
      iconSize: [32, 32],
      iconAnchor: [16, 18],  // anchor at the notch (POI position)
    });
  }

  /**
   * Create an SVG icon for the robot marker (colored circle with direction arrow).
   * Leaflet 依存 (`L.divIcon`)。
   * @param {number} yaw - heading in radians
   * @param {string} color - circle fill color (default cyan); 内側の方向矢印は白固定
   * @param {number} sizePx - icon の outer width/height (px)。viewBox は固定 36 のままで
   *   内部 path / circle 座標は変えず、outer サイズだけリスケールする
   */
  function createRobotIcon(yaw, color = '#00bcd4', sizePx = 36) {
    const rotDeg = 90 - (yaw * 180 / Math.PI);
    const half = sizePx / 2;
    const svg = `<svg width="${sizePx}" height="${sizePx}" viewBox="0 0 36 36" style="transform: rotate(${rotDeg}deg);">` +
      `<circle cx="18" cy="18" r="12" fill="${color}" fill-opacity="0.7" stroke="#fff" stroke-width="2"/>` +
      `<path d="M18 8 L24 24 L18 19 L12 24 Z" fill="#fff" fill-opacity="0.9" stroke="none"/>` +
      `</svg>`;
    return L.divIcon({
      className: 'robot-icon',
      html: svg,
      iconSize: [sizePx, sizePx],
      iconAnchor: [half, half],
    });
  }

  /**
   * Route landmark バッジアイコン ("LM")。Leaflet 依存 (`L.divIcon`)。
   */
  function createRouteLandmarkIcon(color) {
    return L.divIcon({
      className: 'route-landmark-marker',
      html: `<span class="route-landmark-badge" style="background: ${color};">LM</span>`,
      iconSize: [28, 20],
      iconAnchor: [14, 24],
    });
  }

  const api = {
    getPoiColor,
    getRouteColor,
    routeDirectionDeg,
    createRouteDirectionSvg,
    createArrowIcon,
    createRobotIcon,
    createRouteLandmarkIcon,
  };

  if (typeof window !== 'undefined') {
    window.MapoiMapIcons = window.MapoiMapIcons || {};
    Object.assign(window.MapoiMapIcons, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
