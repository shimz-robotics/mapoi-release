/**
 * Pure helpers for map 上の POI 直接操作 (#240 クリック判別 / #239 ドラッグ / #275 yaw 回転)。
 *
 * map-viewer.js の Leaflet / DOM 配線から「純粋なロジック・幾何」だけを切り出し、
 * 単体テスト (mapoi_webui/tests/web/poi-interactions.test.js) で回帰検知できるようにする。
 * 各 PR で繰り返しテスト不足を指摘された判別コア・wedge 幾何・座標変換を対象とする (#273)。
 *
 * Browser からは `MapoiPoiInteractions.*` のグローバルとして使い、
 * Node (vitest) からは `module.exports` 経由で import する dual export 形式
 * (geometry.js / poi-filter.js と同じ #129 パターン)。Leaflet / DOM / window 非依存。
 */
(function () {
  'use strict';

  /**
   * POI marker クリックのシングル/ダブル判別コア (#240)。
   *
   * 同一 index への 2 連クリックが `thresholdMs` 未満ならダブル (= 編集パネル)、
   * それ以外はシングル (= 選択のみ)。判定は「前回クリックの index / 時刻」と「今回の
   * index / 時刻」だけで決まる純関数なので、map-viewer.js の `_handlePoiClick` が
   * 呼び出し前に prev を capture して渡す。これにより onPoiClick → highlightPoi 経由で
   * クリック追跡 state が reset されても、ここで掴んだ prev で判定が保たれる。
   *
   * 次に保持すべき追跡 state も返す:
   * - ダブル時は `(-1, 0)` にリセット (3 連クリックを 2 回目のダブルにしない)
   * - シングル時は今回の `(index, now)` を保持 (次クリックの判定基準にする)
   *
   * @param {number} prevIndex 前回クリックした POI index (未クリックは -1)
   * @param {number} prevTime 前回クリック時刻 (ms, Date.now() 値)
   * @param {number} index 今回クリックした POI index
   * @param {number} now 今回クリック時刻 (ms)
   * @param {number} thresholdMs ダブルクリック判定の上限間隔 (ms)
   * @returns {{ isDouble: boolean, nextIndex: number, nextTime: number }}
   */
  function classifyPoiClick(prevIndex, prevTime, index, now, thresholdMs) {
    const isDouble = prevIndex === index && (now - prevTime) < thresholdMs;
    if (isDouble) {
      return { isDouble: true, nextIndex: -1, nextTime: 0 };
    }
    return { isDouble: false, nextIndex: index, nextTime: now };
  }

  /**
   * 扇形 (wedge) polygon の頂点数 (#275)。中心角 `2*yawTol` を 0.1 rad 刻みで割り、
   * 最低 8 分割を保証する (狭い扇形でもカクつかない最低限の滑らかさ)。
   *
   * @param {number} yawTol 片側 tolerance (rad, > 0)
   * @returns {number} 弧の分割数 N (弧の頂点は N+1 個)
   */
  function wedgeVertexCount(yawTol) {
    return Math.max(8, Math.ceil((2 * yawTol) / 0.1));
  }

  /**
   * 扇形 (wedge) polygon の頂点列を world 座標で計算する (#275)。
   * 中心点 → 弧 (`yawCenter - yawTol` から `yawCenter + yawTol`) の順に並べ、polygon の
   * 自然な閉じ (最後の頂点 → 先頭 = 中心) で扇形になる。map-viewer.js の `_wedgePoints`
   * がこれを `worldToLatLng` で latlng に変換して描画 / ドラッグ追従に使う。
   *
   * 戻り値配列の構造: `[center, arc(i=0), arc(i=1), ..., arc(i=N)]` (長さ N+2)。
   * - `arc(i=0)` は始端角 `yawCenter - yawTol`
   * - `arc(i=N)` は終端角 `yawCenter + yawTol`
   *
   * @param {number} cx 中心 x (world)
   * @param {number} cy 中心 y (world)
   * @param {number} r 半径 (tolerance.xy)
   * @param {number} yawCenter 扇形中心角 (rad)
   * @param {number} yawTol 片側 tolerance (rad)
   * @returns {Array<{x: number, y: number}>}
   */
  function wedgePointsWorld(cx, cy, r, yawCenter, yawTol) {
    const totalAngle = 2 * yawTol;
    const N = wedgeVertexCount(yawTol);
    const startAngle = yawCenter - yawTol;
    const pts = [{ x: cx, y: cy }];
    for (let i = 0; i <= N; i += 1) {
      const a = startAngle + (totalAngle * i) / N;
      pts.push({ x: cx + r * Math.cos(a), y: cy + r * Math.sin(a) });
    }
    return pts;
  }

  /**
   * World 座標 → Leaflet CRS.Simple の (lat, lng) (pixel 空間) への変換 (純粋版)。
   * map-viewer.js の `worldToLatLng` がこれに委譲する。x,y を同一 `resolution` で
   * 割る等方スケールなので、ワールド空間の角度が latlng 空間でも保たれる
   * (yaw 回転ハンドルの不変条件、`yawFromDelta` 参照)。
   *
   * @param {number} x world x
   * @param {number} y world y
   * @param {Array<number>} origin map origin `[originX, originY]`
   * @param {number} resolution m/pixel
   * @returns {{ lat: number, lng: number }}
   */
  function worldToLatLng(x, y, origin, resolution) {
    return {
      lat: (y - origin[1]) / resolution,
      lng: (x - origin[0]) / resolution,
    };
  }

  /**
   * Leaflet CRS.Simple の (lat, lng) → world 座標への逆変換 (純粋版)。
   * map-viewer.js の `latLngToWorld` がこれに委譲する。
   *
   * @param {number} lat
   * @param {number} lng
   * @param {Array<number>} origin map origin `[originX, originY]`
   * @param {number} resolution m/pixel
   * @returns {{ x: number, y: number }}
   */
  function latLngToWorld(lat, lng, origin, resolution) {
    return {
      x: lng * resolution + origin[0],
      y: lat * resolution + origin[1],
    };
  }

  /**
   * yaw 回転ハンドルのドラッグ位置 (中心からの latlng 差分) から新 yaw を求める (#275)。
   *
   * `worldToLatLng` が x,y を同一 resolution で割る等方スケールのため、latlng 空間で
   * `atan2(latDelta, lngDelta)` を取った値はワールド yaw と厳密一致する (resolution が
   * 約分される)。これが PR #276 review で yaw 座標系 high と指摘された点の false-positive
   * 根拠 — テストで `wedgePointsWorld` → `worldToLatLng` → `yawFromDelta` の round-trip を
   * 任意の origin / resolution で確認することで不変条件を守る。
   *
   * @param {number} latDelta ハンドル lat - 中心 lat
   * @param {number} lngDelta ハンドル lng - 中心 lng
   * @returns {number} yaw (rad)
   */
  function yawFromDelta(latDelta, lngDelta) {
    return Math.atan2(latDelta, lngDelta);
  }

  /**
   * POI marker をドラッグ可能にするかの判定 (#239)。選択中 (highlighted) かつ全体の
   * ドラッグ許可 (route 編集中は false) の時だけ true。map-viewer.js の `_applyPoiDraggable`
   * がこれに従って `marker.dragging.enable()/disable()` を呼ぶ。route 編集中は選択中でも
   * 無効になり、waypoint 追加クリックと POI ドラッグの競合を防ぐ。
   *
   * @param {boolean} isHighlighted この marker が現在選択中か
   * @param {boolean} draggingAllowed POI ドラッグの全体許可 (route 編集中は false)
   * @returns {boolean}
   */
  function shouldEnablePoiDrag(isHighlighted, draggingAllowed) {
    return Boolean(isHighlighted) && Boolean(draggingAllowed);
  }

  /**
   * tolerance.yaw が表す到達角度制約の種別 (#267 / #275)。到達判定の角度差は [0, π] なので:
   * - `0 < yawTol < π` → 'wedge' (扇形を描き、yaw 回転ハンドルの対象になる)
   * - `yawTol >= π`    → 'disc'  (yaw 不問 = 全方位。塗りつぶし円で示し、ハンドルは出さない)
   * - それ以外 (`yawTol <= 0` / NaN) → 'none' (xy 円 outline のみ、扇形なし)
   *
   * map-viewer.js の `_drawSectorForPoi` がこの分類で扇形 / disc / なしを描き分け、wedge の時だけ
   * `_poiWedgeByIndex` に登録する (= `shouldShowYawHandle` の `hasWedge`)。
   *
   * @param {number} yawTol 片側 yaw tolerance (rad)
   * @returns {'wedge' | 'disc' | 'none'}
   */
  function classifyYawTolerance(yawTol) {
    if (yawTol > 0 && yawTol < Math.PI) return 'wedge';
    if (yawTol >= Math.PI) return 'disc';
    return 'none';
  }

  /**
   * 選択中 POI に yaw 回転ハンドルを出すかの判定 (#275)。選択中 (index >= 0) かつ
   * ドラッグ許可中かつ wedge POI (`classifyYawTolerance === 'wedge'` で `_poiWedgeByIndex` に
   * 登録済) の 3 条件を満たす時だけ true。disc (yaw 不問) / 扇形なし / 未選択 / route 編集中は
   * ハンドルを出さない。map-viewer.js の `_refreshYawHandle` がこれに従う。
   *
   * @param {number} highlightedIndex 選択中 POI の index (未選択は -1)
   * @param {boolean} draggingAllowed POI ドラッグの全体許可 (route 編集中は false)
   * @param {boolean} hasWedge 当該 POI に wedge (扇形) が登録されているか
   * @returns {boolean}
   */
  function shouldShowYawHandle(highlightedIndex, draggingAllowed, hasWedge) {
    return highlightedIndex >= 0 && Boolean(draggingAllowed) && Boolean(hasWedge);
  }

  const api = {
    classifyPoiClick,
    wedgeVertexCount,
    wedgePointsWorld,
    worldToLatLng,
    latLngToWorld,
    yawFromDelta,
    shouldEnablePoiDrag,
    classifyYawTolerance,
    shouldShowYawHandle,
  };

  if (typeof window !== 'undefined') {
    window.MapoiPoiInteractions = window.MapoiPoiInteractions || {};
    Object.assign(window.MapoiPoiInteractions, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
