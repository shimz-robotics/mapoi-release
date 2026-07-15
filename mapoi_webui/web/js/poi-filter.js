/**
 * POI candidate filters for mapoi WebUI.
 *
 * landmark system tag (#85) は Nav2 navigation target にしない reference 専用。
 * waypoint candidate / route waypoint candidate / initial pose candidate の dropdown
 * から landmark POI を除外する純関数群を集約する。
 *
 * Browser からは `MapoiPoiFilter.*` のグローバルとして使い、
 * Node (vitest) からは `module.exports` 経由で import する dual export 形式。
 * 依存ゼロの pure 関数で DOM / window 等を参照しない。
 */
(function () {
  'use strict';

  const SYSTEM_TAGS = Object.freeze({
    WAYPOINT: 'waypoint',
    LANDMARK: 'landmark',
    PAUSE: 'pause',
  });
  const SYSTEM_TAG_NAMES = Object.freeze(Object.values(SYSTEM_TAGS));

  function normalizedTags(poi) {
    const tags = poi && Array.isArray(poi.tags) ? poi.tags : [];
    return tags.map((t) => String(t).toLowerCase());
  }

  function hasLandmarkTag(poi) {
    return normalizedTags(poi).includes(SYSTEM_TAGS.LANDMARK);
  }

  // Nav2 navigation 候補 (= waypoint tag 付き / landmark なし) を抽出する (#142)。
  // 旧 system tag は `goal` だったが #142 で `waypoint` にリネームし、
  // 単発 nav goal も route 中間点も同じ tag で表現する semantics に統一した。
  function filterWaypointCandidates(pois) {
    return (pois || []).filter((p) => {
      const tags = normalizedTags(p);
      return tags.includes(SYSTEM_TAGS.WAYPOINT) && !tags.includes(SYSTEM_TAGS.LANDMARK);
    });
  }

  function filterInitialPoseCandidates(pois) {
    return (pois || []).filter((p) => !hasLandmarkTag(p));
  }

  function filterRouteWaypointCandidates(pois) {
    return (pois || []).filter((p) => !hasLandmarkTag(p));
  }

  // tolerance 最小値 (msg spec): xy = 1 mm、yaw = 約 0.057° (#138)。
  // 0 / 負値 は「無反応 POI」を許してしまうため明示的に禁止。
  const TOLERANCE_MIN = 0.001;

  // yaml 書き出し時の有効桁数 (#242)。
  // xy は mm 精度 (3 桁)、yaw は rad で約 0.006° 精度 (4 桁) — 0.0001 rad ≒ 0.0057°。
  // frontend readForm と backend save_pois 両方で同じ値を使う defense-in-depth。
  const POSE_XY_DIGITS = 3;
  const POSE_YAW_DIGITS = 4;
  const TOLERANCE_XY_DIGITS = 3;
  const TOLERANCE_YAW_DIGITS = 4;

  /**
   * 数値を指定桁数で丸める。非数値 / NaN / Infinity は NaN を返す。
   * yaml の可読性と git diff の安定性のため、Save 経路で必ず通す (#242)。
   *
   * @param {string|number} value 数値 or parse 可能な文字列
   * @param {number} digits 小数点以下桁数 (>= 0)
   * @returns {number} 丸めた値、または NaN
   */
  function roundTo(value, digits) {
    const v = parseFloat(value);
    if (!Number.isFinite(v)) return NaN;
    const factor = Math.pow(10, digits);
    return Math.round(v * factor) / factor;
  }

  function degToRad(deg) {
    const v = parseFloat(deg);
    return Number.isFinite(v) ? v * Math.PI / 180.0 : NaN;
  }

  function radToDeg(rad) {
    const v = parseFloat(rad);
    return Number.isFinite(v) ? v * 180.0 / Math.PI : NaN;
  }

  // POI editor の tolerance.yaw 入力欄の度数表示桁数 (#267)。内部 / yaml は rad だが editor は
  // 度数で入出力する (#138)。2 桁丸め + 末尾ゼロ除去で 30/45/90/180 等の区切りの良い度数を
  // 綺麗に表示する (rad 4 桁保存 → 度数 0.01° 解像度で round-trip も安定)。
  const TOLERANCE_YAW_DISPLAY_DIGITS = 2;

  /**
   * tolerance.yaw (rad) を editor 表示用の度数値に変換する純関数 (#267)。
   * radToDeg 後に 2 桁で丸め、parseFloat で末尾ゼロを落とす (例: 45.00 → 45、180.00 → 180)。
   * 区切りの良い度数 (30/45/90/180 等) が綺麗に表示され、入力 → 保存 → 再表示の round-trip も
   * 安定する。旧実装は toFixed(4) で 3.14 rad → "179.9088"、180° 入力 → "180.0002" と汚かった。
   *
   * @param {string|number} rad
   * @returns {number} 度数 (非数値入力時は NaN)
   */
  function formatToleranceYawDeg(rad) {
    const deg = radToDeg(rad);
    if (!Number.isFinite(deg)) return NaN;
    return parseFloat(deg.toFixed(TOLERANCE_YAW_DISPLAY_DIGITS));
  }

  /**
   * POI list 検索ボックス (#383) の名前フィルタ判定。大文字小文字不区別の部分一致。
   * 空 / 空白のみの query は全件 match (= 絞り込みなし)。名前なし POI は query が
   * ある間は match しない。絞り込みは list 表示のみで、map marker の可視状態
   * (visiblePois) とは独立 (poi-editor.js renderList が card 生成を skip するだけ)。
   *
   * @param {object} poi POI object ({ name, ... })
   * @param {string} query 検索文字列 (未 trim で良い)
   * @returns {boolean} 表示すべきなら true
   */
  function matchesPoiName(poi, query) {
    const q = String(query == null ? '' : query).trim().toLowerCase();
    if (!q) return true;
    const name = poi && poi.name ? String(poi.name) : '';
    return name.toLowerCase().includes(q);
  }

  /**
   * Map cursor readout 用に world 座標を整形する純関数 (#381)。
   * POI yaml の座標桁数 (POSE_XY_DIGITS) に合わせて toFixed で桁を固定し、
   * mousemove 中に小数部の桁数が揺れて文字幅がちらつくのを防ぐ (roundTo だと
   * 末尾ゼロが落ちて "1.2" ⇄ "1.234" と幅が変わる)。非有限入力は空文字 =
   * 呼び出し側 (map-viewer.js) は表示を更新しない。
   *
   * @param {number} x world x (m)
   * @param {number} y world y (m)
   * @returns {string} 例 "x: -1.234, y: 5.678"、非有限入力は ''
   */
  function formatCursorCoords(x, y) {
    if (!Number.isFinite(x) || !Number.isFinite(y)) return '';
    return `x: ${x.toFixed(POSE_XY_DIGITS)}, y: ${y.toFixed(POSE_XY_DIGITS)}`;
  }

  /**
   * POI form の tolerance round-trip を保つ純関数 (#87 / #138)。
   * `||` だと意図的な小さい値も default で上書きされるので Number.isFinite で判定。
   * 最終 xy / yaw は msg spec に従い `>= TOLERANCE_MIN` を保証する (UI HTML min を
   * bypass する経路に対する防御)。
   *
   * @param {string|number} rawXyValueM xy input 値 (m)
   * @param {string|number} rawYawValueRad yaw input から deg→rad 変換済の値
   * @returns {{xy: number, yaw: number}} 両方とも >= TOLERANCE_MIN
   */
  function parseTolerance(rawXyValueM, rawYawValueRad) {
    const xyParsed = parseFloat(rawXyValueM);
    const yawParsed = parseFloat(rawYawValueRad);
    const xy = Number.isFinite(xyParsed) && xyParsed >= TOLERANCE_MIN ? xyParsed : TOLERANCE_MIN;
    const yaw = Number.isFinite(yawParsed) && yawParsed >= TOLERANCE_MIN ? yawParsed : TOLERANCE_MIN;
    return { xy, yaw };
  }

  /**
   * POI tag combination 排他 check (#85)。
   * `waypoint+landmark` / `pause+landmark` は仕様上排他。save 前に
   * 弾くことで mapoi_config.yaml に invalid な POI が永続化するのを防ぐ。
   *
   * @returns {{ ok: boolean, error?: string }}
   */
  function validatePoiTags(poi) {
    const tags = normalizedTags(poi);
    const has = (n) => tags.includes(n);
    if (has(SYSTEM_TAGS.WAYPOINT) && has(SYSTEM_TAGS.LANDMARK)) {
      return {
        ok: false,
        error: '"waypoint" and "landmark" cannot be used together. A landmark is only a reference POI and is not sent to Nav2.',
      };
    }
    // landmark × pause 排他 (#143): landmark は到達不可なので pause 動作が成立しない。
    if (has(SYSTEM_TAGS.PAUSE) && has(SYSTEM_TAGS.LANDMARK)) {
      return {
        ok: false,
        error: '"pause" and "landmark" cannot be used together. A landmark is not a navigation target, so the robot cannot pause there.',
      };
    }
    // (initial_pose × landmark 排他は #144 で initial_pose system tag を廃止したため不要に。)
    return { ok: true };
  }

  const api = {
    SYSTEM_TAGS,
    SYSTEM_TAG_NAMES,
    hasLandmarkTag,
    filterWaypointCandidates,
    filterInitialPoseCandidates,
    filterRouteWaypointCandidates,
    matchesPoiName,
    validatePoiTags,
    parseTolerance,
    degToRad,
    radToDeg,
    formatToleranceYawDeg,
    formatCursorCoords,
    roundTo,
    TOLERANCE_MIN,
    POSE_XY_DIGITS,
    POSE_YAW_DIGITS,
    TOLERANCE_XY_DIGITS,
    TOLERANCE_YAW_DIGITS,
    TOLERANCE_YAW_DISPLAY_DIGITS,
  };

  if (typeof window !== 'undefined') {
    window.MapoiPoiFilter = window.MapoiPoiFilter || {};
    Object.assign(window.MapoiPoiFilter, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
