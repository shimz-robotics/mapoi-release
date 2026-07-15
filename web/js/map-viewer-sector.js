/**
 * map-viewer.js から切り出した POI 扇形 (tolerance sector) 描画 + yaw 回転ハンドル (#346)。
 *
 * MapViewer の `_drawSectorForPoi` / `_wedgePoints` / `_circlePoints` / `_refreshYawHandle` /
 * `_removeYawHandle` / `_addYawHandle` と、それらが所有していた state (`sectorLayers` /
 * `_poiWedgeByIndex` / `_yawHandle`) をこのクラスに移した。MapViewer は自身のインスタンスを
 * `viewer` として渡し、必要な描画 API (`viewer.map` / `viewer.worldToLatLng` / `viewer._zIndex` /
 * `viewer.poiMarkers` / `viewer.updatePoiMarkerYaw` / `viewer.onPoiYawDragEnd` / `viewer.tagColors`)
 * を参照する。ロジック・呼び出し順序は元の実装のまま (挙動不変)。
 *
 * Browser からは top-level の `MapViewerSector` グローバルとして使い (map-viewer.js が
 * `new MapViewerSector(this)`)、Node (vitest) からは `module.exports` 経由で import する
 * (poi-editor.js / route-editor.js と同じクラス export 形式)。Leaflet / MapViewer 依存の
 * ため、既存の class 系モジュールと同様にこのクラス自体の新規 vitest は追加しない。
 */
class MapViewerSector {
  constructor(viewer) {
    this.viewer = viewer;
    this.sectorLayers = []; // POI tolerance を扇形描画した Leaflet layer (#136)
    this.wedgeByIndex = {}; // wedge POI の { layer, cx, cy, r, yawTol } を index 別に保持 (#275)
    this.yawHandle = null;  // 選択中 wedge POI の弧先端に出す回転ハンドル marker (1 個だけ)
  }

  /**
   * 扇形 (wedge) の polygon 頂点列 (latlng) を計算する (#275)。中心 → 弧 (yawCenter±yawTol) → 中心。
   * 幾何 (頂点数 / 角度範囲) は pure helper (poi-interactions.js) に委譲し単体テスト対象とし、
   * ここでは world 座標を worldToLatLng で latlng へ変換するだけ。初期描画 (drawForPoi) と
   * 回転ハンドルドラッグ中の setLatLngs 追従の両方で使う。
   */
  wedgePoints(cx, cy, r, yawCenter, yawTol) {
    return MapoiPoiInteractions.wedgePointsWorld(cx, cy, r, yawCenter, yawTol)
      .map((p) => this.viewer.worldToLatLng(p.x, p.y));
  }

  /**
   * xy tolerance の円 outline 用頂点列 (36 角形)。route landmark の半径リング描画
   * (map-viewer.js の showRouteLandmarkHighlights) からも共有で使われる。
   */
  circlePoints(cx, cy, r) {
    const N_CIRCLE = 36;
    const points = [];
    for (let i = 0; i <= N_CIRCLE; i += 1) {
      const a = (2 * Math.PI * i) / N_CIRCLE;
      points.push(this.viewer.worldToLatLng(cx + r * Math.cos(a), cy + r * Math.sin(a)));
    }
    return points;
  }

  /**
   * POI の tolerance を 円 (xy 判定領域) + 扇形 (yaw 制約) の重ね描きで描画する (#136 / #179)。
   *
   * - 円 outline: 半径 = `tolerance.xy` (xy 進入判定の境界、yaw 不問)、
   *   `L.polyline` で 36 角形を構成して常時描画 (細実線・薄め)
   * - 扇形: `tolerance.yaw` が `0 < yaw < π` の時のみ重ね描き (`L.polygon`)
   *   - waypoint: 塗り扇形 (`fillOpacity` 高)
   *   - landmark: 中抜き扇形 (`fillOpacity = 0`、stroke のみ)
   *   - その他 (旧 event 等): 描画しない (#70 で別途整理)
   * - pause tag があれば xy 円沿いに dot pattern overlay (#179: 旧 dash → dot 形式)
   *
   * `tolerance.xy <= 0` または対象 tag 無しなら no-op。
   *
   * `L.circle` ではなく polyline で実装するのは、`L.circle` の radius が pixel/meter 換算に
   * `crs` 依存の挙動を持つ一方、扇形は world 座標を `worldToLatLng` で変換して描画するため。
   * 同じ計算式で円と扇形の弧を生成すれば視覚的整合 (両者の弧が完全一致) が保証される。
   */
  drawForPoi(poi, index) {
    if (!poi.pose) return;
    if (!poi.tolerance || typeof poi.tolerance.xy !== 'number' || poi.tolerance.xy <= 0) return;

    const tags = poi.tags || [];
    const isWaypoint = tags.includes('waypoint');
    const isLandmark = tags.includes('landmark');
    const isPause = tags.includes('pause');
    if (!isWaypoint && !isLandmark) return;

    const r = poi.tolerance.xy;
    const yawCenter = poi.pose.yaw || 0;
    const yawTol = (typeof poi.tolerance.yaw === 'number') ? poi.tolerance.yaw : 0;
    // 到達判定の角度差は [0, π] なので yawTol >= π は「yaw 不問 (全方位)」。0 < yawTol < π は
    // 扇形 (wedge)、>= π は塗りつぶし円 (disc) で表す (#267)。分類は pure helper に委譲 (#273)。
    const yawKind = MapoiPoiInteractions.classifyYawTolerance(yawTol);
    const hasYawConstraint = yawKind === 'wedge';
    const isYawUnconstrained = yawKind === 'disc';

    const color = MapoiMapIcons.getPoiColor(this.viewer.tagColors, poi);

    // 円の頂点列 (36 角形 = 10 度刻み)。最後に起点へ戻り polyline が自然に閉じる。
    const circlePoints = this.circlePoints(poi.pose.x, poi.pose.y, r);

    // xy 判定領域 (円 outline): 控えめな細実線で常時表示 (#179)
    const circle = L.polyline(circlePoints, {
      color,
      weight: 1,
      opacity: 0.4,
      interactive: false,
    }).addTo(this.viewer.map);
    circle.bringToBack();
    this.sectorLayers.push(circle);

    // 扇形 (yaw 制約): 0 < yaw < π の時に重ね描き (#179)。
    const fillOpacity = isWaypoint ? 0.25 : 0.10;  // waypoint=塗り、landmark=薄塗り (#179 ユーザー確認 2)
    if (hasYawConstraint) {
      const sectorPoints = this.wedgePoints(poi.pose.x, poi.pose.y, r, yawCenter, yawTol);
      const sector = L.polygon(sectorPoints, {
        color,
        weight: 1,
        opacity: 0.7,
        fillColor: color,
        fillOpacity,
        interactive: false,
      }).addTo(this.viewer.map);
      sector.bringToBack();
      this.sectorLayers.push(sector);
      // wedge POI は回転ハンドル対象。layer + 幾何を index 別に保持し、ドラッグ中の
      // setLatLngs 追従と、refreshYawHandle でのハンドル位置算出に使う (#275)。
      if (typeof index === 'number' && index >= 0) {
        this.wedgeByIndex[index] = {
          layer: sector, cx: poi.pose.x, cy: poi.pose.y, r, yawTol,
        };
      }
    } else if (isYawUnconstrained) {
      // yaw 不問 (yawTol >= π = 全方位): 扇形を省略すると xy 円の outline だけになり「描画され
      // ていない?」と誤認されるため、xy 円を塗りつぶした disc で「全方位 OK」を明示する (#267)。
      // 扇形 (wedge) が広がりきって円になったもの、という連続的な見た目になる。
      const disc = L.polygon(circlePoints, {
        color,
        weight: 1,
        opacity: 0.7,
        fillColor: color,
        fillOpacity,
        interactive: false,
      }).addTo(this.viewer.map);
      disc.bringToBack();
      this.sectorLayers.push(disc);
    }

    // pause overlay: xy 円沿いに dot pattern を重ね描き (#179)。
    // 旧 dashArray '6, 4' (cycle 比 60% on, 1.5:1 dash) は dot として識別しづらく潰れて見える feedback
    // (#178 PR コメント) を受け、'2, 6' (cycle 比 25% on, 1:3 on:off) で短い dot + 大きい gap に変更。
    // RViz 側 (dot 0.02m / step 0.10m, 20% on / 1:4) と厳密比率は僅差だが共に sparse dot で整合。
    if (isPause) {
      const pauseOutline = L.polyline(circlePoints, {
        color,
        weight: 4,
        opacity: 0.9,
        dashArray: '2, 6',
        lineCap: 'round',  // dot を丸く描画して角ばった dash 感を消す
        interactive: false,
      }).addTo(this.viewer.map);
      pauseOutline.bringToBack();
      this.sectorLayers.push(pauseOutline);
    }
  }

  /**
   * 扇形 layer / wedge 追跡 / yaw 回転ハンドルを全クリアする (POI 全体の再描画前に呼ぶ)。
   * MapViewer#clearPois が担っていたリセット処理をそのまま移した。
   */
  clear() {
    this.wedgeByIndex = {};
    this.removeYawHandle();
    this.sectorLayers.forEach((l) => this.viewer.map.removeLayer(l));
    this.sectorLayers = [];
  }

  /**
   * 選択中 wedge POI の yaw 回転ハンドル (弧先端の marker) を出し直す (#275)。
   * 既存ハンドルを除去し、(選択中 && ドラッグ許可 && wedge POI) の時だけ再生成する。
   * disc (yaw 不問) や扇形のない POI は wedgeByIndex に無いのでハンドルなし。
   */
  refreshYawHandle() {
    this.removeYawHandle();
    const index = this.viewer._highlightedPoiIndex;
    const wedge = this.wedgeByIndex[index];
    if (!MapoiPoiInteractions.shouldShowYawHandle(index, this.viewer._poiDraggingAllowed, !!wedge)) return;
    this._addYawHandle(index, wedge);
  }

  removeYawHandle() {
    if (this.yawHandle) {
      this.viewer.map.removeLayer(this.yawHandle);
      this.yawHandle = null;
    }
  }

  /**
   * 弧先端 (中心から yaw 方向に r) にドラッグ可能なハンドル marker を置く (#275)。
   * drag 中: 中心 → ハンドル位置の角度を新 yaw とし、矢印 (updatePoiMarkerYaw) と
   *   wedge polygon (setLatLngs) を live 追従。ハンドル自体はカーソル追従 (円上に拘束しない)。
   * dragend: 確定 yaw を onPoiYawDragEnd で通知 (→ dirty + Save、再描画でハンドルは正位置へ)。
   * @private
   */
  _addYawHandle(index, wedge) {
    const item = this.viewer.poiMarkers.find((m) => m.index === index);
    if (!item) return;
    const yaw = item.yaw || 0;
    const centerLL = this.viewer.worldToLatLng(wedge.cx, wedge.cy);
    const tipLL = this.viewer.worldToLatLng(
      wedge.cx + wedge.r * Math.cos(yaw), wedge.cy + wedge.r * Math.sin(yaw),
    );
    const svg = '<svg width="18" height="18" viewBox="0 0 18 18">'
      + '<circle cx="9" cy="9" r="6" fill="#e67e22" fill-opacity="0.9" stroke="#fff" stroke-width="2"/>'
      + '</svg>';
    const icon = L.divIcon({
      className: 'poi-yaw-handle', html: svg, iconSize: [18, 18], iconAnchor: [9, 9],
    });
    const handle = L.marker(tipLL, {
      icon,
      draggable: true,
      zIndexOffset: this.viewer._zIndex('yawHandle'),
    }).addTo(this.viewer.map);
    const yawFrom = (ll) => MapoiPoiInteractions.yawFromDelta(
      ll.lat - centerLL.lat, ll.lng - centerLL.lng,
    );
    handle.on('drag', () => {
      const newYaw = yawFrom(handle.getLatLng());
      this.viewer.updatePoiMarkerYaw(index, newYaw);
      wedge.layer.setLatLngs(this.wedgePoints(wedge.cx, wedge.cy, wedge.r, newYaw, wedge.yawTol));
    });
    handle.on('dragend', () => {
      const newYaw = yawFrom(handle.getLatLng());
      if (this.viewer.onPoiYawDragEnd) this.viewer.onPoiYawDragEnd(index, newYaw);
    });
    this.yawHandle = handle;
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = MapViewerSector;
}
