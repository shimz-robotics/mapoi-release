/**
 * Leaflet map viewer for mapoi.
 * Displays the occupancy grid map and POI markers.
 */
class MapViewer {
  constructor(containerId) {
    this.map = L.map(containerId, {
      crs: L.CRS.Simple,
      minZoom: -3,
      maxZoom: 5,
      zoomSnap: 0.25,
      attributionControl: false,
      // +/- ズームボタン (Leaflet 既定) は不要。マウスホイール/ドラッグ・タッチのピンチで
      // 直感的にズームでき、地図上の余計なコントロールを減らす (#333)。
      zoomControl: false,
    });

    // ボタン削除後もホイール/ピンチでズームできることを e2e から検証できるよう、現在
    // zoom を DOM に反映する (#333 Codex review low 対応)。アプリの挙動には影響しない
    // test hook。
    this.map.on('zoom', () => {
      this.map.getContainer().dataset.zoom = String(this.map.getZoom());
    });

    this.imageOverlay = null;
    this.poiMarkers = [];  // { marker, poi, index }
    this.routeLayers = []; // Leaflet layers for route lines and labels
    this.metadata = null;
    this.tagColors = {};  // tag_name -> color, built from definitions
    this.robotMarker = null;     // Leaflet marker for robot position
    this.onMapClick = null;      // callback(worldX, worldY)
    this.onPoiClick = null;      // callback(index) — single click = select only
    this.onPoiDblClick = null;   // callback(index) — double click = open edit panel (#240)
    this.onPoiDragEnd = null;    // callback(index, worldX, worldY) — 選択中 POI をドラッグ移動 (#239)
    this.onPoiYawDragEnd = null; // callback(index, yaw) — 扇形ハンドルで yaw 回転 (#275)
    this.onRouteClick = null;    // callback(routeIndex)
    // POI marker のシングル/ダブルクリック判別状態 (#240)。native 'dblclick' は使えない:
    // highlightPoi が単発クリックで setIcon により marker の DOM 要素を差し替えるため、
    // ブラウザの「同一要素で 2 連クリック」条件が崩れて 'dblclick' が発火しない。連続 'click'
    // の時間差で判定する。dirty 時は選択ごとに showPois が marker を作り直すので、marker
    // closure ではなく MapViewer インスタンス側に状態を持って再生成をまたいで保持する。
    this._lastPoiClickIndex = -1;
    this._lastPoiClickTime = 0;
    this._poiDblClickMs = 300;
    // 選択中の POI marker だけドラッグで position 移動可 (#239)。route 編集中は
    // setPoiDraggingAllowed(false) で一時無効化し、waypoint 追加クリックと競合させない。
    // _highlightedPoiIndex は現在 highlight 中 (= 選択中) の POI index を保持し、
    // setPoiDraggingAllowed が再適用時に「どの marker を enable するか」判断するのに使う。
    this._poiDraggingAllowed = true;
    this._highlightedPoiIndex = -1;
    // POI tolerance の扇形描画 + yaw 回転ハンドル (#275) は MapViewerSector に切り出した
    // (#346)。sectorLayers / _poiWedgeByIndex / yawHandle marker はこのインスタンスが保持する。
    this._sector = new MapViewerSector(this);
    this._poseTool = null;       // pose tool state
    this._routePolylines = [];   // { line, hitLine, arrowMarkers, labelMarkers, routeIdx, color, latlngs } for click & highlight
    this._activeRouteIdx = -1;   // 現在 active な route index (highlightRoute で更新)
    this._lastRobotPose = null;  // 最新 pose (updateRobotMarker で更新)
    this._robotConnectorLayers = []; // 現在のロボット位置 → active route 先頭 POI への connector
    this._reachedRouteSignatures = new Set(); // 到達済み route の "name|firstWaypoint" signature 集合 (page 内 sticky)
    this._routeLandmarkLayers = []; // selected/editing route に紐づく landmark POI の badge/ring
    this._cachedPois = null; // showPois の POI array。landmark overlay の再描画に使う
    this._cachedPoiVisibility = null; // showPois の visibleSet snapshot。landmark overlay の可視性判定に使う
    this._layerPriorityMode = 'default'; // default | editing | navigation

    // ロボットの実寸 (m)。connector 到達閾値とロボットマーカーサイズの両方に
    // 使う (#116)。default は TurtleBot3 burger (~0.105m) に余裕を持たせた値。
    // backend (`/api/nav/status` payload の `robot_radius`) が起動時 launch
    // param 由来の値を返すので、app.js が `setRobotRadius` で上書きする (#117)。
    // backend が古い (key 不在) 場合や上書き前の初回描画はこの default を使う。
    this.robotRadiusM = 0.15;

    this.map.on('click', (e) => {
      // 地図の空白クリックは POI ダブルクリック判別シーケンスを打ち切る (#240 review)。
      // marker クリックは stopPropagation で map click を発火させないので、ここに来るのは
      // 純粋な背景クリックのみ。これがないと「POI クリック → 空白クリック → 同一 POI を
      // 300ms 内に再クリック」を誤ってダブルクリックと判定してしまう。
      this._resetPoiClickTracking();
      if (this._poseTool) {
        this._handlePoseToolClick(e.latlng);
        return;
      }
      if (this.onMapClick && this.metadata) {
        const world = this.latLngToWorld(e.latlng);
        this.onMapClick(world.x, world.y);
      }
    });

    this.map.on('mousemove', (e) => {
      if (this._poseTool && this._poseTool.phase === 'yaw') {
        this._updatePoseToolArrow(e.latlng);
      }
    });

    // Cursor 位置の world 座標 readout (#381)。mousemove で常時更新し、map 外へ出たら
    // (mouseout) 非表示に戻す。touch 端末は mousemove が来ないので出ないまま (タップ時
    // 表示は scope 外)。表示要素は index.html の #cursor-readout (右下固定 div —
    // #map-status は左下、#ui-controls は右上で重ならない)。metadata 未 load 中は
    // 座標変換できないので表示しない。桁は POI yaml の丸め (POSE_XY_DIGITS) に揃える。
    this._cursorReadoutEl = document.getElementById('cursor-readout');
    this.map.on('mousemove', (e) => {
      if (!this._cursorReadoutEl || !this.metadata) return;
      const world = this.latLngToWorld(e.latlng);
      const text = MapoiPoiFilter.formatCursorCoords(world.x, world.y);
      // 非有限座標 (metadata 異常等) では古い値を残さず隠す (Cursor review medium 対応)。
      if (!text) {
        this._cursorReadoutEl.classList.add('hidden');
        return;
      }
      this._cursorReadoutEl.textContent = text;
      this._cursorReadoutEl.classList.remove('hidden');
    });
    this.map.on('mouseout', () => {
      if (this._cursorReadoutEl) this._cursorReadoutEl.classList.add('hidden');
    });

    // zoom が変わると 1m あたりの pixel が変わるので、robot marker のピクセル
    // サイズを再計算するために再描画する (#116)。pose 未受信時は updateRobotMarker
    // が early return するので safe。
    this.map.on('zoomend', () => {
      if (this._lastRobotPose) this.updateRobotMarker(this._lastRobotPose);
    });

    // route polyline は parallel offset を pixel 単位で計算するため、zoom 変化で
    // 各 vertex の世界座標が同じでも画面表示用 displayLatlngs の位置が変わる
    // (#69 PR-2)。最後の showRoutes 引数を保持して再描画する。再描画後は
    // _activeRouteIdx に基づいて highlight も再適用する (clearRoutes は active
    // index を保持するので、新規 _routePolylines に対して再 paint すれば良い)。
    //
    // perf 最適化: 直近の showRoutes で実際に描画された route 数 (= drawable 数) が
    // 1 本以下なら offsetPx は必ず 0 で displayLatlngs === latlngs (世界座標)。
    // Leaflet の zoom 変換に任せれば足りるので redraw skip して DOM/SVG layer
    // の全削除→全作成コストを避ける (#69 round 2 low 対応)。
    // visibleNames で filter された結果でも、POI 未解決で drawable=0/1 になる
    // ケースに正しく対応する。
    this.map.on('zoomend', () => {
      if (!this._cachedRoutesArgs) return;
      if (this._routePolylines.length <= 1) return;
      const { routes, pois, visibleNames } = this._cachedRoutesArgs;
      const prevActive = this._activeRouteIdx;
      this.showRoutes(routes, pois, visibleNames);
      if (prevActive >= 0) {
        this.highlightRoute(prevActive);
      }
    });
  }

  /**
   * Convert world coordinates to Leaflet LatLng.
   * In CRS.Simple, lat=y, lng=x (in pixel space).
   * World coords: x, y  ->  pixel: (x - originX) / res, (y - originY) / res
   * But image y-axis is flipped: pixel_y measured from top, world y increases upward.
   * We use: lat = pixel_y_from_bottom = (y - originY) / res
   *         lng = (x - originX) / res
   */
  worldToLatLng(x, y) {
    const m = this.metadata;
    // 等方スケールの座標変換は pure helper に委譲し単体テスト対象にする (#273)。
    const ll = MapoiPoiInteractions.worldToLatLng(x, y, m.origin, m.resolution);
    return L.latLng(ll.lat, ll.lng);
  }

  /**
   * Convert Leaflet LatLng back to world coordinates.
   */
  latLngToWorld(latlng) {
    const m = this.metadata;
    return MapoiPoiInteractions.latLngToWorld(latlng.lat, latlng.lng, m.origin, m.resolution);
  }

  /**
   * Load and display a map image.
   */
  async loadMap(mapName) {
    // Fetch metadata
    this.metadata = await MapoiApi.getMapMetadata(mapName);
    if (this.metadata.error) {
      document.getElementById('map-status').textContent = 'Map not found';
      return;
    }

    const m = this.metadata;

    // Map 切替時は robot pose / connector / 到達履歴 / active 選択を全 reset。
    // 直後の fitBounds() で zoomend hook が走るため、旧 map の latlng 基準で
    // updateRobotMarker / _updateRobotConnector が実行されると、新 map 側で
    // 誤座標描画 + 旧 signature の sticky 引継ぎが起こる (Codex PR #118 round 1
    // medium)。同様に _cachedRoutesArgs に旧 map の pois が残っていると、
    // fitBounds → zoomend → showRoutes (旧 pois) で stale 描画する (#69 PR-2)。
    this._lastRobotPose = null;
    this._reachedRouteSignatures.clear();
    this._activeRouteIdx = -1;
    if (this.robotMarker) {
      this.map.removeLayer(this.robotMarker);
      this.robotMarker = null;
    }
    this._clearRobotConnector();
    // 旧 map の route layer (line / hitLine / arrow / label) も新 overlay に
    // 重なるため明示的に物理削除。app.js の loadRoutes が遅い・失敗する・
    // map 切替が連続するケースで stale polyline が見えないように同期 reset
    // (#69 PR-2 round 1 medium 対応)。clearRoutes 内で _cachedRoutesArgs = null
    // も実施されるため、再度の null 代入は冗長だが副作用は無い。
    this.clearRoutes();

    // Remove old overlay
    if (this.imageOverlay) {
      this.map.removeLayer(this.imageOverlay);
    }

    // Image bounds in CRS.Simple coordinates
    // Bottom-left: (0, 0), Top-right: (width, height) in pixel coords
    const bounds = [
      [0, 0],               // bottom-left (lat=0, lng=0)
      [m.height, m.width],  // top-right
    ];

    const imageUrl = MapoiApi.getMapImageUrl(mapName);
    this.imageOverlay = L.imageOverlay(imageUrl, bounds).addTo(this.map);
    this.map.fitBounds(bounds);

    document.getElementById('map-status').textContent =
      `${mapName} (${m.width}x${m.height}, res=${m.resolution})`;
  }

  /**
   * Set tag definitions and build color map.
   */
  setTagDefinitions(tags) {
    const palette = ['#e74c3c', '#3498db', '#27ae60', '#9b59b6', '#e67e22', '#1abc9c', '#f39c12', '#8e44ad'];
    this.tagColors = {};
    (tags || []).forEach((td, i) => {
      this.tagColors[td.name] = palette[i % palette.length];
    });
  }

  setLayerPriorityMode(mode) {
    const next = ['default', 'editing', 'navigation'].includes(mode) ? mode : 'default';
    if (this._layerPriorityMode === next) return;
    this._layerPriorityMode = next;
    this._applyLayerPriority();
    this._updateRobotConnector();
  }

  isPoseToolActive() {
    return !!this._poseTool;
  }

  cancelPoseToolPendingPosition() {
    const pt = this._poseTool;
    if (!pt || pt.phase !== 'yaw' || !pt.positionPicked) return false;
    this._cancelPoseToolYaw();
    return true;
  }

  _zIndex(kind) {
    const byMode = {
      default: {
        poi: 0,
        poiHighlight: 1000,
        routeMarker: 800,
        landmark: 1100,
        yawHandle: 1500,
        robotConnector: 1200,
        robot: 2000,
      },
      editing: {
        poi: 2400,
        poiHighlight: 2800,
        routeMarker: 2600,
        landmark: 2700,
        yawHandle: 3000,
        robotConnector: 250,
        robot: 300,
      },
      navigation: {
        poi: 0,
        poiHighlight: 1000,
        routeMarker: 1200,
        landmark: 1100,
        yawHandle: 1500,
        robotConnector: 3100,
        robot: 3200,
      },
    };
    return (byMode[this._layerPriorityMode] || byMode.default)[kind] || 0;
  }

  _poiZIndex(isHighlighted) {
    return this._zIndex(isHighlighted ? 'poiHighlight' : 'poi');
  }

  _applyLayerPriority() {
    this.poiMarkers.forEach((item) => {
      item.marker.setZIndexOffset(this._poiZIndex(item.index === this._highlightedPoiIndex));
    });
    if (this._sector.yawHandle) this._sector.yawHandle.setZIndexOffset(this._zIndex('yawHandle'));
    if (this.robotMarker) this.robotMarker.setZIndexOffset(this._zIndex('robot'));
    this._routePolylines.forEach((item) => {
      this._applyRouteLinePriority(item.line);
      this._applyRouteLinePriority(item.hitLine);
      [...(item.arrowMarkers || []), ...(item.labelMarkers || [])].forEach((marker) => {
        marker.setZIndexOffset(this._zIndex('routeMarker'));
      });
    });
    (this._editingPreviewLayers || []).forEach((layer) => {
      if (typeof layer.setZIndexOffset === 'function') {
        layer.setZIndexOffset(this._zIndex('routeMarker'));
      } else {
        this._applyRouteLinePriority(layer);
      }
    });
    this._routeLandmarkLayers.forEach((layer) => {
      if (typeof layer.setZIndexOffset === 'function') {
        layer.setZIndexOffset(this._zIndex('landmark'));
      }
      if (this._layerPriorityMode === 'navigation' && typeof layer.bringToBack === 'function') {
        layer.bringToBack();
      } else if (typeof layer.bringToFront === 'function') {
        layer.bringToFront();
      }
    });
  }

  _applyRouteLinePriority(line) {
    if (!line || typeof line.bringToBack !== 'function') return;
    if (this._layerPriorityMode === 'editing' && typeof line.bringToFront === 'function') {
      line.bringToFront();
    } else {
      line.bringToBack();
    }
  }

  /**
   * Display POI markers as directional arrows on the map.
   * @param {Array} pois - POI array
   * @param {Set|null} visibleSet - if provided, only show POIs whose index is in this set
   */
  showPois(pois, visibleSet) {
    this.clearPois();
    this._cachedPois = pois;
    this._cachedPoiVisibility = visibleSet ? new Set(visibleSet) : null;
    pois.forEach((poi, index) => {
      if (!poi.pose) return;
      if (visibleSet && !visibleSet.has(index)) return;

      // 扇形 (sector) を arrow icon の背景に描画 (#136)。index は wedge を回転ハンドルから
      // setLatLngs で更新するための追跡に使う (#275)。
      this._sector.drawForPoi(poi, index);

      const latlng = this.worldToLatLng(poi.pose.x, poi.pose.y);
      const color = MapoiMapIcons.getPoiColor(this.tagColors, poi);
      const yaw = poi.pose.yaw || 0;

      const icon = MapoiMapIcons.createArrowIcon(color, yaw, false);
      // draggable: true で生成して drag ハンドラを用意するが既定は disable。選択された
      // marker だけ highlightPoi (_applyPoiDraggable) が enable する (#239)。
      const marker = L.marker(latlng, {
        icon,
        draggable: true,
        zIndexOffset: this._poiZIndex(false),
      }).addTo(this.map);
      if (marker.dragging) marker.dragging.disable();

      marker.bindTooltip(poi.name || `POI ${index}`, {
        permanent: false,
        direction: 'top',
        offset: [0, -14],
      });

      // シングルクリック = 選択のみ、ダブルクリック = 編集パネル (#240)。判別は
      // _handlePoiClick で連続 click の時間差から行う (native 'dblclick' を使わない理由は
      // constructor の _lastPoiClickIndex 周りのコメント参照)。map の onMapClick /
      // doubleClickZoom に伝播しないよう stopPropagation する。
      marker.on('click', (e) => {
        L.DomEvent.stopPropagation(e);
        this._handlePoiClick(index);
      });

      // 選択中 POI のドラッグで position を移動 (#239)。ドラッグは click を発火させない
      // (Leaflet が抑止) ので dblclick 判定とは独立。dragstart で判別 state を切り、dragend で
      // 世界座標を通知する。確定は dirty + Save (auto-POST しない、pose tool / #241 と一貫)。
      marker.on('dragstart', () => {
        this._resetPoiClickTracking();
      });
      marker.on('dragend', () => {
        const world = this.latLngToWorld(marker.getLatLng());
        if (this.onPoiDragEnd) this.onPoiDragEnd(index, world.x, world.y);
      });

      this.poiMarkers.push({ marker, poi, index, color, yaw });
    });
    this._applyLayerPriority();
    this._refreshActiveRouteLandmarks();
  }

  /**
   * POI ダブルクリック判別 state をクリア (#240 review)。連続マーカークリック以外の操作
   * (地図空白クリック / POI 一覧選択 / route 選択 / 再描画) で呼び、間に別操作を挟んだ
   * 同一 POI 再クリックを誤ってダブルと判定しないようにする。_handlePoiClick は判定用の
   * prev 値を callback 実行前に capture するため、選択経由 (highlightPoi) でこれが呼ばれても
   * 本来のダブルクリック検出は壊れない。
   * @private
   */
  _resetPoiClickTracking() {
    this._lastPoiClickIndex = -1;
    this._lastPoiClickTime = 0;
  }

  /**
   * POI marker クリックのシングル/ダブル判別 (#240)。
   * 単発 = 選択のみ (onPoiClick)、同一 index への 2 連クリックが _poiDblClickMs 以内なら
   * ダブル = 編集パネル (onPoiDblClick)。onPoiClick は毎回即時に呼ぶので選択の即応性は保たれ、
   * ダブル時は「選択 → 編集」と自然に遷移する (selectPoi は idempotent)。
   * native 'dblclick' を使わない理由は constructor の _lastPoiClickIndex コメント参照。
   *
   * 判定用 prev 値はコールバック実行前に capture する。onPoiClick → selectPoi → highlightPoi
   * 経由で _resetPoiClickTracking が走っても、ここで掴んだ prev で判定するので検出は保たれる。
   * 新 state は onPoiClick の後に書き、その reset を上書きして次クリックの判定用に残す。
   * @private
   */
  _handlePoiClick(index) {
    // 判別コアは pure helper (#273)。prev はコールバック前に capture して渡すので、
    // onPoiClick → highlightPoi 経由で追跡 state が reset されても判定は保たれる。
    const r = MapoiPoiInteractions.classifyPoiClick(
      this._lastPoiClickIndex, this._lastPoiClickTime, index, Date.now(), this._poiDblClickMs,
    );
    if (this.onPoiClick) this.onPoiClick(index);
    // 追跡 state は onPoiClick の後・onPoiDblClick の前に書く。onPoiClick (→ highlightPoi)
    // が走らせる reset を上書きしつつ、旧実装と同じく onPoiDblClick より前に確定させる
    // (ダブル時は (-1,0) に reset 済みとなり、dblClick ハンドラが例外を投げても state は残らない)。
    this._lastPoiClickIndex = r.nextIndex;
    this._lastPoiClickTime = r.nextTime;
    if (r.isDouble && this.onPoiDblClick) this.onPoiDblClick(index);
  }

  /**
   * Highlight a specific POI marker.
   */
  highlightPoi(index) {
    // POI 選択 (一覧クリック / 選択再描画 / 可視性 toggle 等) はマーカー連続クリック以外の
    // 操作なので判別 state をクリアする (#240 review)。マーカー経由のダブルクリックは
    // _handlePoiClick が prev を capture 済みなので、ここで reset しても検出は保たれる。
    this._resetPoiClickTracking();
    this._highlightedPoiIndex = index;
    this.poiMarkers.forEach((item) => {
      const isHighlighted = item.index === index;
      const icon = MapoiMapIcons.createArrowIcon(item.color, item.yaw, isHighlighted);
      item.marker.setIcon(icon);
      if (isHighlighted) {
        item.marker.setZIndexOffset(this._poiZIndex(true));
      } else {
        item.marker.setZIndexOffset(this._poiZIndex(false));
      }
      // 選択中 marker だけドラッグ可 (#239)
      this._applyPoiDraggable(item, isHighlighted);
    });
    // 選択中 wedge POI に yaw 回転ハンドルを出す (#275)
    this._sector.refreshYawHandle();
  }

  /**
   * 選択 POI が viewport 外なら中心へパンする (#382)。ズームは変えない (panTo)。
   *
   * 「既に viewport 内なら動かさない」判定で選択経路を一本化する (issue #382 の
   * 実装時判断): map クリック由来の選択はクリックした POI が必ず見えているので
   * パンせず視点が飛ばない。list / nav dropdown (#262) 由来で画面外の POI を
   * 選んだ時だけ寄せる。可視性 checkbox が off の POI は marker が無い (= 地図に
   * 出ていない) のでパンしない。
   */
  panToPoiIfHidden(index) {
    if (!this.metadata) return;
    const item = this.poiMarkers.find((m) => m.index === index);
    if (!item) return;
    const latlng = item.marker.getLatLng();
    if (this.map.getBounds().contains(latlng)) return;
    this.map.panTo(latlng);
  }

  /**
   * 選択中 (highlighted) の POI marker だけドラッグ可能にする (#239)。_poiDraggingAllowed が
   * false (route 編集中等) のときは選択中でも無効。setIcon で DOM 要素が差し替わっても
   * Leaflet の dragging handler は marker に紐づくので enable/disable はそのまま効く。
   * @private
   */
  _applyPoiDraggable(item, isHighlighted) {
    if (!item.marker.dragging) return;
    if (MapoiPoiInteractions.shouldEnablePoiDrag(isHighlighted, this._poiDraggingAllowed)) {
      item.marker.dragging.enable();
    } else {
      item.marker.dragging.disable();
    }
  }

  /**
   * POI ドラッグ移動の全体許可を切り替える (#239)。route 編集中は false にして、
   * waypoint 追加クリックと POI ドラッグの競合を防ぐ。現在の選択 marker に即反映する。
   */
  setPoiDraggingAllowed(allowed) {
    if (this._poiDraggingAllowed === allowed) return;
    this._poiDraggingAllowed = allowed;
    this.poiMarkers.forEach((item) => {
      this._applyPoiDraggable(item, item.index === this._highlightedPoiIndex);
    });
    // yaw 回転ハンドルも同じ許可に従う (route 編集中は出さない) (#275)
    this._sector.refreshYawHandle();
  }

  /**
   * Enable two-click pose tool (RViz-style).
   * Phase 'position': click previews position (orange circle) only — does NOT commit X/Y yet
   *   (#269: 誤クリックで POI が動くのを防ぐ)、then enters 'yaw' phase.
   * Phase 'yaw': mousemove shows arrow preview, click commits position + yaw together, then
   *   enters 'position' phase. Escape cancels yaw phase (discards pending position).
   * @param {object} callbacks - { onPositionSet(x,y), onYawSet(yaw) }
   * @param {L.LatLng|null} initialLatLng - if provided, start in 'yaw' phase with this position
   *   (新規 POI: 位置は placeNewPoi が確定済なので 2 クリック目は yaw のみ確定)
   */
  enablePoseTool(callbacks, initialLatLng) {
    this.disablePoseTool();
    this._poseTool = {
      phase: initialLatLng ? 'yaw' : 'position',
      startLatLng: initialLatLng || null,
      // この tool session 内で位置をクリック選択したか (#269)。initialLatLng 開始 (新規 POI の
      // yaw 選択) では位置は placeNewPoi が確定済なので false。position phase でのクリックで true。
      positionPicked: false,
      circle: null,
      line: null,
      arrowHead: null,
      callbacks,
    };
    // Escape で yaw 選択モードを解除する (#269)。map container はクリックしないとフォーカスを
    // 持たないので、document に listen する。tool が active かつ yaw phase の時だけ作用させ、
    // 他の Escape ハンドラ (modal 閉じる等) を妨げないよう preventDefault しない。
    // 入力欄 (name / x / y 等) で編集中の Escape は pose キャンセルではなく入力操作なので、
    // editable な target からの Escape は無視する (Cursor review #270 medium 対応)。
    this._poseTool.keydownHandler = (e) => {
      if (e.key !== 'Escape') return;
      const t = e.target;
      const tag = t && t.tagName ? t.tagName.toUpperCase() : '';
      const isEditable = tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT'
        || (t && t.isContentEditable);
      if (isEditable) return;
      this._cancelPoseToolYaw();
    };
    document.addEventListener('keydown', this._poseTool.keydownHandler);
    if (initialLatLng) {
      this._poseTool.circle = L.circleMarker(initialLatLng, {
        radius: 6, color: '#e67e22', fillColor: '#e67e22', fillOpacity: 0.8, weight: 2,
      }).addTo(this.map);
    }
    this.map.getContainer().style.cursor = 'crosshair';
  }

  /**
   * Disable the pose tool and clean up visuals.
   */
  disablePoseTool() {
    if (!this._poseTool) return;
    if (this._poseTool.keydownHandler) {
      document.removeEventListener('keydown', this._poseTool.keydownHandler);
    }
    this._clearPoseToolVisuals();
    this._poseTool = null;
    this.map.getContainer().style.cursor = '';
  }

  /** @private */
  _handlePoseToolClick(latlng) {
    const pt = this._poseTool;
    if (pt.phase === 'position') {
      // 1 クリック目: 位置はまだ確定しない (#269)。誤クリックで POI が動くのを防ぐため、
      // orange circle のプレビューを出すだけで form / marker は書き換えず yaw phase へ進む。
      // positionPicked = true で「この session でクリック選択した位置」を記録し、2 クリック目で
      // 位置 + 姿勢をまとめて確定する。新規 POI (initialLatLng で yaw phase 開始) では false の
      // ままで、位置は placeNewPoi が確定済なので 2 クリック目は yaw のみ確定する。
      this._clearPoseToolVisuals();
      pt.startLatLng = latlng;
      pt.positionPicked = true;

      pt.circle = L.circleMarker(latlng, {
        radius: 6, color: '#e67e22', fillColor: '#e67e22', fillOpacity: 0.8, weight: 2,
      }).addTo(this.map);

      pt.phase = 'yaw';
    } else if (pt.phase === 'yaw') {
      // 2 クリック目: 位置 (1 クリック目で選択していれば) + 姿勢を確定する (#269)。
      const start = pt.startLatLng;
      const yaw = Math.atan2(latlng.lat - start.lat, latlng.lng - start.lng);
      const startWorld = this.latLngToWorld(start);

      this._clearPoseToolVisuals();
      pt.phase = 'position';
      const committedPosition = pt.positionPicked;
      pt.positionPicked = false;

      if (committedPosition && pt.callbacks.onPositionSet) {
        pt.callbacks.onPositionSet(startWorld.x, startWorld.y);
      }
      if (pt.callbacks.onYawSet) {
        pt.callbacks.onYawSet(yaw);
      }
    }
  }

  /**
   * yaw phase を中断して position phase に戻す (#269)。保留中の位置プレビューを破棄し、
   * form / marker は一切変更しない (= 誤クリックの取り消し)。Escape から呼ばれる。
   * @private
   */
  _cancelPoseToolYaw() {
    const pt = this._poseTool;
    if (!pt || pt.phase !== 'yaw') return;
    this._clearPoseToolVisuals();
    pt.startLatLng = null;
    pt.positionPicked = false;
    pt.phase = 'position';
  }

  /** @private */
  _updatePoseToolArrow(latlng) {
    const pt = this._poseTool;
    if (!pt || !pt.startLatLng) return;
    const start = pt.startLatLng;

    // Dashed line from position to cursor
    if (pt.line) {
      pt.line.setLatLngs([start, latlng]);
    } else {
      pt.line = L.polyline([start, latlng], {
        color: '#e67e22', weight: 3, dashArray: '6, 4',
      }).addTo(this.map);
    }

    // Arrowhead at tip
    const yaw = Math.atan2(latlng.lat - start.lat, latlng.lng - start.lng);
    const rotDeg = 90 - (yaw * 180 / Math.PI);
    const svg = `<svg width="24" height="24" viewBox="0 0 24 24" style="transform: rotate(${rotDeg}deg);">` +
      `<path d="M12 2 L20 18 L12 14 L4 18 Z" fill="#e67e22" fill-opacity="0.9" stroke="#fff" stroke-width="1.5" stroke-linejoin="round"/>` +
      `</svg>`;
    const icon = L.divIcon({
      className: 'pose-tool-arrow',
      html: svg,
      iconSize: [24, 24],
      iconAnchor: [12, 14],
    });

    if (pt.arrowHead) {
      pt.arrowHead.setLatLng(latlng);
      pt.arrowHead.setIcon(icon);
    } else {
      pt.arrowHead = L.marker(latlng, { icon, interactive: false }).addTo(this.map);
    }
  }

  /** @private */
  _clearPoseToolVisuals() {
    const pt = this._poseTool;
    if (!pt) return;
    if (pt.circle) { this.map.removeLayer(pt.circle); pt.circle = null; }
    if (pt.line) { this.map.removeLayer(pt.line); pt.line = null; }
    if (pt.arrowHead) { this.map.removeLayer(pt.arrowHead); pt.arrowHead = null; }
  }

  /**
   * Update a POI marker's position on the map.
   */
  updatePoiMarkerPosition(index, x, y) {
    const item = this.poiMarkers.find((m) => m.index === index);
    if (!item) return;
    item.marker.setLatLng(this.worldToLatLng(x, y));
  }

  /**
   * Update a POI marker's arrow rotation to reflect a new yaw.
   */
  updatePoiMarkerYaw(index, yaw) {
    const item = this.poiMarkers.find((m) => m.index === index);
    if (!item) return;
    item.yaw = yaw;
    const icon = MapoiMapIcons.createArrowIcon(item.color, yaw, true);
    item.marker.setIcon(icon);
    item.marker.setZIndexOffset(this._poiZIndex(true));
  }

  /**
   * Remove all POI markers from the map.
   */
  clearPois() {
    this.poiMarkers.forEach((item) => {
      this.map.removeLayer(item.marker);
    });
    this.poiMarkers = [];
    // marker を全削除したので highlight 追跡もリセット (#239)。再描画後に highlightPoi が
    // 呼ばれれば再設定される。stale index で setPoiDraggingAllowed が誤判定するのを防ぐ。
    this._highlightedPoiIndex = -1;
    // 扇形 layer (#136) / wedge 追跡・回転ハンドル (#275) も作り直されるのでリセットする。
    // 再描画後の highlightPoi (refreshYawHandle) でハンドルは再生成される。
    this._sector.clear();
    this.clearRouteLandmarkHighlights();
  }

  clearRouteLandmarkHighlights() {
    if (!this._routeLandmarkLayers) return;
    this._routeLandmarkLayers.forEach((layer) => {
      this.map.removeLayer(layer);
    });
    this._routeLandmarkLayers = [];
  }

  showRouteLandmarkHighlights(landmarkNames, pois, color) {
    this.clearRouteLandmarkHighlights();
    if (!this.metadata || !landmarkNames || !pois) return;
    const poiByName = new Map();
    pois.forEach((poi, index) => {
      if (poi && poi.name) poiByName.set(poi.name, { poi, index });
    });
    [...new Set(landmarkNames)].forEach((name) => {
      const entry = poiByName.get(name);
      if (!entry || !entry.poi.pose) return;
      if (this._cachedPoiVisibility && !this._cachedPoiVisibility.has(entry.index)) return;
      const { pose } = entry.poi;
      const radius = entry.poi.tolerance && entry.poi.tolerance.xy;
      if (typeof radius === 'number' && Number.isFinite(radius) && radius > 0) {
        const ring = L.polyline(this._sector.circlePoints(pose.x, pose.y, radius), {
          color: color || '#8e44ad',
          weight: 3,
          opacity: 0.95,
          interactive: false,
          className: 'route-landmark-radius-ring',
        }).addTo(this.map);
        ring.bringToFront();
        this._routeLandmarkLayers.push(ring);
      }
      const marker = L.marker(this.worldToLatLng(pose.x, pose.y), {
        icon: MapoiMapIcons.createRouteLandmarkIcon(color || '#8e44ad'),
        interactive: false,
        zIndexOffset: this._zIndex('landmark'),
      }).addTo(this.map);
      marker.bindTooltip(`Landmark: ${name}`, {
        permanent: false,
        direction: 'top',
        offset: [0, -22],
      });
      this._routeLandmarkLayers.push(marker);
    });
    this._applyLayerPriority();
  }

  _refreshActiveRouteLandmarks() {
    const item = this._routePolylines.find((it) => it.routeIdx === this._activeRouteIdx);
    if (!item) {
      this.clearRouteLandmarkHighlights();
      return;
    }
    const pois = this._cachedPois || (this._cachedRoutesArgs && this._cachedRoutesArgs.pois);
    this.showRouteLandmarkHighlights(item.landmarks, pois, item.color);
  }

  /**
   * Offset a polyline by `offsetPx` pixels perpendicular to each segment.
   *
   * Leaflet `latLngToContainerPoint` で pixel 空間に変換し、純粋幾何関数
   * `MapoiGeometry.offsetPointsPx` (mapoi_webui/web/js/geometry.js) に委譲、
   * 結果を `containerPointToLatLng` で latlng に戻す。pure helper 側で
   * canonical normal / bisector / cap / 退化 fallback 等の幾何ロジックを担当
   * (#129 で単体テストの対象として切り出し)。
   *
   * pixel 計算なので zoom 変化で再計算が必要 → showRoutes は zoomend で cached
   * args を使って呼び直す (#69 PR-2)。
   *
   * @param {Array<L.LatLng>} latlngs - 入力経路 (raw)
   * @param {number} offsetPx - 正負で canonical 法線に対する offset 方向、0 なら no-op
   * @returns {Array<L.LatLng>}
   */
  _offsetLatLngs(latlngs, offsetPx) {
    if (!latlngs || latlngs.length < 2 || offsetPx === 0) return latlngs;
    const points = latlngs.map((ll) => this.map.latLngToContainerPoint(ll));
    const offsetPoints = MapoiGeometry.offsetPointsPx(points, offsetPx);
    return offsetPoints.map((pt) => this.map.containerPointToLatLng(L.point(pt.x, pt.y)));
  }

  /**
   * Display routes on the map as polylines with waypoint order labels.
   * @param {Array} routes - [{name, waypoints: [poiName, ...]}, ...]
   * @param {Array} pois - POI array to resolve waypoint names to coordinates
   * @param {Set|null} visibleNames - if provided, only show routes whose name is in this set
   */
  showRoutes(routes, pois, visibleNames) {
    this.clearRoutes();
    if (!this.metadata || !routes || !pois) return;

    // zoomend で再描画するため最後の引数を保持。
    // - visibleNames: 軽量なので `new Set` で snapshot し、呼び出し側の Set 変更が
    //   redraw に漏れない (#69 round 1 low 対応)。
    // - routes / pois: 配列が大きくなり得る (POI 多数) ため deep copy しない。代わりに
    //   呼び出し側 (app.js) は showRoutes 後に in-place mutation しない前提を取る。
    //   in-place 更新ではなく新配列で再 showRoutes する運用 (現状そう)。仕様変更で
    //   in-place mutation が出てきた場合は、ここを snapshot に切り替える。
    this._cachedRoutesArgs = {
      routes,
      pois,
      visibleNames: visibleNames ? new Set(visibleNames) : null,
    };

    // Build name -> poi lookup
    const poiByName = {};
    pois.forEach((poi) => {
      if (poi.name) poiByName[poi.name] = poi;
    });

    // 共通区間で polyline が完全重複する視認性問題への (b) parallel offset 対策 (#69 PR-2)。
    //
    // 1) drawable な route (rawLatlngs.length >= 2、つまり POI 2 点以上に解決でき
    //    実際に描画される route) を先に列挙する。
    //    visible だが waypoint 不足 / POI 未解決で描画されない route を offset lane
    //    数に含めると、画面上の route 数 1 でも offsetPx > 0 で route が中心から
    //    ずれる + zoomend skip 条件が崩れる (Codex round 2 low 対応)。
    // 2) drawable 数で中心揃え offset index を付与し、各 route に offsetPx を割り当てる。
    //    表示中 3 route → offset index は -1, 0, +1 → offsetPx は -STEP, 0, +STEP。
    //    route 数 1 のみだと offset 0 で _offsetLatLngs は no-op、原点描画と完全一致。
    const drawableEntries = [];
    routes.forEach((route, routeIdx) => {
      if (visibleNames && !visibleNames.has(route.name)) return;
      const rawLatlngs = [];
      // orders は rawLatlngs と並行配列。各 vertex に「元 waypoint 配列での
      // 1-based index」を保持する。missing POI が途中にある場合 (例: 2 番目が
      // 未解決で skip) でも、地図上のラベルが route editor 側の waypoint 順番
      // (1, 3, 4 等) を保つ (#69 round 4 low 対応、Codex review)。
      const orders = [];
      let firstWaypointName = '';
      (route.waypoints || []).forEach((wpName, i) => {
        const poi = poiByName[wpName];
        if (poi && poi.pose) {
          rawLatlngs.push(this.worldToLatLng(poi.pose.x, poi.pose.y));
          orders.push(i + 1);
          if (!firstWaypointName) firstWaypointName = wpName;
        }
      });
      if (rawLatlngs.length < 2) return;
      drawableEntries.push({ route, routeIdx, rawLatlngs, orders, firstWaypointName });
    });
    const totalDrawable = drawableEntries.length;
    const OFFSET_STEP_PX = 6;

    drawableEntries.forEach(({ route, routeIdx, rawLatlngs, orders, firstWaypointName }, visibleIdx) => {
      const offsetPx = (visibleIdx - (totalDrawable - 1) / 2) * OFFSET_STEP_PX;
      const color = MapoiMapIcons.getRouteColor(routeIdx);

      // 表示用に offset を適用 (offsetPx === 0 なら同じ配列参照が返る)
      const displayLatlngs = this._offsetLatLngs(rawLatlngs, offsetPx);

      // Draw polyline (dashed)
      const line = L.polyline(displayLatlngs, {
        color: color,
        weight: 3,
        opacity: 0.7,
        dashArray: '8, 6',
      }).addTo(this.map);

      line.bindTooltip(route.name || `Route ${routeIdx + 1}`, {
        sticky: true,
        direction: 'top',
        offset: [0, -8],
      });

      this._applyRouteLinePriority(line);
      this.routeLayers.push(line);

      // Invisible wider hit area for easier clicking
      const hitLine = L.polyline(displayLatlngs, {
        color: '#000',
        weight: 16,
        opacity: 0,
        interactive: true,
      }).addTo(this.map);

      hitLine.on('click', (e) => {
        L.DomEvent.stopPropagation(e);
        // route 選択もマーカー連続クリック以外の操作なので判別 state をクリア (#240 review)。
        this._resetPoiClickTracking();
        if (this.onRouteClick) this.onRouteClick(routeIdx);
      });

      this._applyRouteLinePriority(hitLine);
      this.routeLayers.push(hitLine);

      const arrowMarkers = [];
      const labelMarkers = [];

      // Draw teardrop direction markers along segments (offset 後の座標で配置)
      for (let i = 0; i < displayLatlngs.length - 1; i++) {
        const from = displayLatlngs[i];
        const to = displayLatlngs[i + 1];
        const midLat = (from.lat + to.lat) / 2;
        const midLng = (from.lng + to.lng) / 2;
        const rotDeg = MapoiMapIcons.routeDirectionDeg(from, to);

        const arrowIcon = L.divIcon({
          className: 'route-arrow',
          html: MapoiMapIcons.createRouteDirectionSvg(color, rotDeg, 16),
          iconSize: [16, 16],
          iconAnchor: [8, 8],
        });
        const arrowMarker = L.marker([midLat, midLng], {
          icon: arrowIcon,
          interactive: false,
          zIndexOffset: this._zIndex('routeMarker'),
        }).addTo(this.map);
        this.routeLayers.push(arrowMarker);
        arrowMarkers.push(arrowMarker);
      }

      // Draw order labels at each waypoint (offset 後位置でラベル重なりも軽減)。
      // 番号は元 waypoint 配列の 1-based index (orders[i]) を使い、missing POI
      // が skip されても route editor 側の順番と整合する (#69 round 4 low 対応)。
      displayLatlngs.forEach((latlng, i) => {
        const order = orders[i];
        const icon = L.divIcon({
          className: 'route-order-label',
          html: `<span style="background: ${color};">${order}</span>`,
          iconSize: [22, 22],
          iconAnchor: [-6, 28],
        });
        const labelMarker = L.marker(latlng, {
          icon: icon,
          interactive: false,
          zIndexOffset: this._zIndex('routeMarker'),
        }).addTo(this.map);
        this.routeLayers.push(labelMarker);
        labelMarkers.push(labelMarker);
      });

      this._routePolylines.push({
        line, hitLine, arrowMarkers, labelMarkers,
        routeIdx, routeName: route.name || '', firstWaypointName,
        landmarks: route.landmarks || [],
        color,
        // latlngs: raw (POI 物理座標)。robot connector の到達判定など物理距離を
        //   評価する用途で使う
        // displayLatlngs: offset 後 (画面表示用)。connector の視覚端点や zoom
        //   再描画時の再計算で使う
        latlngs: rawLatlngs, displayLatlngs,
      });
    });
    this._applyLayerPriority();
  }

  /**
   * Remove all route layers from the map.
   */
  clearRoutes() {
    this.routeLayers.forEach((layer) => {
      this.map.removeLayer(layer);
    });
    this.routeLayers = [];
    this._routePolylines = [];
    // cached args は次回 showRoutes で上書きされるので残しても害は無いが、
    // route データが本当に空 (cleared) のとき zoom した瞬間に古い args で
    // 再描画されないよう、clearRoutes が「外から明示的に空にされた」ケースで
    // 初期化することにした。showRoutes 内部から clearRoutes を呼ぶ場合は直後に
    // _cachedRoutesArgs が再設定されるので race にはならない。
    this._cachedRoutesArgs = null;
    this.clearEditingRoutePreview();
    this.clearRouteLandmarkHighlights();
    this._clearRobotConnector();
    // 到達履歴 (_reachedRouteSignatures) は clearRoutes では reset しない。
    // clearRoutes は visibility toggle / 編集 preview 出入り等の表示再描画でも
    // 呼ばれるため、ここで reset すると sticky 性が壊れる (Codex round 2 medium)。
    // signature は "routeName|firstWaypoint" 複合 ID で、先頭 POI 変更や
    // map 切替で別 route が同名で再生成された場合は signature が変わる
    // ため、自動的に fresh 評価される (Codex round 3 medium 対応)。
  }

  /**
   * Highlight a specific route on the map.
   *
   * - routeIdx >= 0: active route を太線・実線・不透明にし、他 route は dim (薄い + 細線 + dashed)
   *   にして焦点を作る。矢印・順序ラベルにも opacity を適用。
   * - routeIdx === -1: 全 route を default 表示 (中程度の太さ + dashed + 通常 opacity) に戻す。
   *
   * 複数 route が同じ POI を経由して polyline が重なる視認性問題への (d) active highlight 対策。
   */
  highlightRoute(routeIdx) {
    // 描画済みの _routePolylines に対象 routeIdx が実在する時のみ active state に入る。
    // 非表示 / 未描画 / waypoint 不足等で対象 entry がない場合に全 route を dim にしない (Codex review medium #105)。
    const activeExists = routeIdx >= 0
      && this._routePolylines.some((item) => item.routeIdx === routeIdx);
    this._routePolylines.forEach((item) => {
      const isActive = item.routeIdx === routeIdx;
      if (activeExists && isActive) {
        item.line.setStyle({ weight: 5, opacity: 1.0, dashArray: null });
        this._setMarkersOpacity(item.arrowMarkers, 1.0);
        this._setMarkersOpacity(item.labelMarkers, 1.0);
      } else if (activeExists && !isActive) {
        item.line.setStyle({ weight: 2, opacity: 0.25, dashArray: '8, 6' });
        this._setMarkersOpacity(item.arrowMarkers, 0.25);
        this._setMarkersOpacity(item.labelMarkers, 0.25);
      } else {
        item.line.setStyle({ weight: 3, opacity: 0.7, dashArray: '8, 6' });
        this._setMarkersOpacity(item.arrowMarkers, 1.0);
        this._setMarkersOpacity(item.labelMarkers, 1.0);
      }
    });
    this._activeRouteIdx = routeIdx;
    this._refreshActiveRouteLandmarks();
    this._updateRobotConnector();
    // route 切替で marker 色も追従させる (pose 未受信時は updateRobotMarker が
    // early return するので safe)。
    if (this._lastRobotPose) {
      this.updateRobotMarker(this._lastRobotPose);
    }
  }

  _setMarkersOpacity(markers, opacity) {
    if (!markers) return;
    markers.forEach((m) => {
      if (m && typeof m.setOpacity === 'function') {
        m.setOpacity(opacity);
      }
    });
  }

  /**
   * Show a live preview of the route being edited.
   * @param {Array} waypoints - waypoint name array (editingWaypoints)
   * @param {Array} pois - POI array to resolve names
   * @param {string} color - route color
   * @param {Array} landmarks - landmark POI names linked to this route
   */
  showEditingRoutePreview(waypoints, pois, color, landmarks) {
    this.clearEditingRoutePreview();
    this.clearRouteLandmarkHighlights();
    if (!this.metadata || !waypoints || !pois) return;

    const poiByName = {};
    pois.forEach((poi) => { if (poi.name) poiByName[poi.name] = poi; });

    const latlngs = [];
    const resolved = [];
    waypoints.forEach((wpName, i) => {
      const poi = poiByName[wpName];
      if (poi && poi.pose) {
        const ll = this.worldToLatLng(poi.pose.x, poi.pose.y);
        latlngs.push(ll);
        resolved.push({ latlng: ll, order: i + 1 });
      }
    });

    this._editingPreviewLayers = [];

    // Draw solid polyline (editing preview is solid, not dashed)
    if (latlngs.length >= 2) {
      const line = L.polyline(latlngs, {
        color: color,
        weight: 5,
        opacity: 0.9,
      }).addTo(this.map);
      this._applyRouteLinePriority(line);
      this._editingPreviewLayers.push(line);

      // Directional teardrop markers
      for (let i = 0; i < latlngs.length - 1; i++) {
        const from = latlngs[i];
        const to = latlngs[i + 1];
        const midLat = (from.lat + to.lat) / 2;
        const midLng = (from.lng + to.lng) / 2;
        const rotDeg = MapoiMapIcons.routeDirectionDeg(from, to);
        const arrowIcon = L.divIcon({
          className: 'route-arrow',
          html: MapoiMapIcons.createRouteDirectionSvg(color, rotDeg, 20),
          iconSize: [20, 20],
          iconAnchor: [10, 10],
        });
        const arrowMarker = L.marker([midLat, midLng], {
          icon: arrowIcon,
          interactive: false,
          zIndexOffset: this._zIndex('routeMarker'),
        }).addTo(this.map);
        this._editingPreviewLayers.push(arrowMarker);
      }
    }

    // Order labels
    resolved.forEach(({ latlng, order }) => {
      const icon = L.divIcon({
        className: 'route-order-label',
        html: `<span style="background: ${color}; box-shadow: 0 0 6px ${color};">${order}</span>`,
        iconSize: [22, 22],
        iconAnchor: [-6, 28],
      });
      const labelMarker = L.marker(latlng, {
        icon: icon,
        interactive: false,
        zIndexOffset: this._zIndex('routeMarker'),
      }).addTo(this.map);
      this._editingPreviewLayers.push(labelMarker);
    });
    this.showRouteLandmarkHighlights(landmarks || [], pois, color);
    this._applyLayerPriority();
  }

  /**
   * Remove editing route preview layers.
   */
  clearEditingRoutePreview() {
    if (this._editingPreviewLayers) {
      this._editingPreviewLayers.forEach((layer) => {
        this.map.removeLayer(layer);
      });
    }
    this._editingPreviewLayers = [];
  }

  /**
   * Update `robotRadiusM` from backend (`/api/nav/status` の `robot_radius`)。
   *
   * 値は launch param `robot_radius` 由来で起動後は変わらない想定だが、
   * polling 経路で来るので idempotent に書く。次の `updateRobotMarker` で
   * marker サイズと connector 閾値が新値で再計算される。
   *
   * 不正値 (NaN / 非有限 / 0以下) は ignore して default / 前回値を維持する。
   * connector 到達判定で `< robotRadiusM` を使うため 0 以下を許すと到達認識が
   * 永久に立たない。
   */
  setRobotRadius(meters) {
    if (typeof meters !== 'number' || !Number.isFinite(meters) || meters <= 0) {
      return;
    }
    if (this.robotRadiusM === meters) return;
    this.robotRadiusM = meters;
  }

  /**
   * Compute the marker icon size (pixels) so it reflects the robot's real
   * world radius at the current map zoom level (#116).
   *
   * `worldToLatLng` + `latLngToContainerPoint` で 1m が今の zoom で何 pixel
   * になるかを実測する。CRS / resolution に依存せず robust。低 zoom で
   * marker が点になるのを防ぐため下限 16px を入れる。
   *
   * Note: 戻り値は icon の **outer width/height (= bounding box)** で、
   * 内部の見える circle は viewBox の `r=12 / 36 ≒ 2/3` の比率になる
   * (矢印 path 含む余白のため)。outer = robot diameter として扱うので
   * 見える円自体は実 robot より少し小さく描かれるが、heading 矢印を含めた
   * 総占有面積として robot 実寸に揃う設計 (Codex PR #118 round 1 low の
   * 意図明示)。見える円を robot 実寸に厳密に合わせる場合は別 PR で sizePx
   * を 1.5 倍するか、SVG 内部を r=18 に書き換える。
   *
   * pose / metadata 未受信時は default 36px (constructor 時 marker のため)。
   */
  _resolveRobotMarkerSizePx() {
    const MIN_PX = 16;
    const DEFAULT_PX = 36;
    if (!this.metadata || !this._lastRobotPose) return DEFAULT_PX;
    const center = this.worldToLatLng(this._lastRobotPose.x, this._lastRobotPose.y);
    const edge = this.worldToLatLng(
      this._lastRobotPose.x + this.robotRadiusM, this._lastRobotPose.y,
    );
    const pxRadius = this.map.latLngToContainerPoint(center)
      .distanceTo(this.map.latLngToContainerPoint(edge));
    const diameterPx = pxRadius * 2;
    return Math.max(MIN_PX, diameterPx);
  }

  /**
   * Resolve the marker color to use based on the current active route selection.
   * - active route が選択されていてかつ entry が `_routePolylines` に実在する場合は
   *   その色 (route 色と marker 色を揃える)
   * - 未選択 / entry 不在 (waypoint <2 / 非表示等) は default cyan
   *   PR #105 の `activeExists` ガードと同じ趣旨
   */
  _resolveRobotMarkerColor() {
    if (this._activeRouteIdx < 0) return '#00bcd4';
    const item = this._routePolylines.find(
      (it) => it.routeIdx === this._activeRouteIdx,
    );
    return item ? item.color : '#00bcd4';
  }

  /**
   * Update or hide the robot marker on the map.
   * @param {Object|null} pose - {x, y, yaw} or null to hide
   */
  updateRobotMarker(pose) {
    if (!pose || !this.metadata) {
      if (this.robotMarker) {
        this.map.removeLayer(this.robotMarker);
        this.robotMarker = null;
      }
      this._lastRobotPose = null;
      this._updateRobotConnector();
      return;
    }
    // _resolveRobotMarkerSizePx は this._lastRobotPose を読むため、
    // metadata / pose を使う前に最新値を保存しておく。
    this._lastRobotPose = pose;
    const latlng = this.worldToLatLng(pose.x, pose.y);
    const icon = MapoiMapIcons.createRobotIcon(
      pose.yaw || 0,
      this._resolveRobotMarkerColor(),
      this._resolveRobotMarkerSizePx(),
    );
    if (this.robotMarker) {
      this.robotMarker.setLatLng(latlng);
      this.robotMarker.setIcon(icon);
      this.robotMarker.setZIndexOffset(this._zIndex('robot'));
    } else {
      this.robotMarker = L.marker(latlng, {
        icon,
        interactive: false,
        zIndexOffset: this._zIndex('robot'),
      }).addTo(this.map);
    }
    this._updateRobotConnector();
  }

  /**
   * Draw or refresh a directional connector from the current robot position
   * to the first waypoint of the active route.
   *
   * Guards (no-op の条件):
   * - active route が選択されていない (`_activeRouteIdx === -1`)
   * - robot pose 未取得 (`_lastRobotPose === null`)
   * - active route の polyline entry が `_routePolylines` に無い
   *   (waypoint 不足 / 非表示等。PR #105 の activeExists と同じガード)
   * - 当 route の signature (`routeName|firstWaypointName`) が
   *   `_reachedRouteSignatures` に登録済み。複合 ID にすることで
   *   - 表示再描画 (clearRoutes / visibility toggle / 編集 cancel 等) では
   *     sticky 性を維持
   *   - 同名のまま先頭 waypoint を変更した、map 切替で別 route が同名で
   *     再生成された等のケースは signature が変わるため fresh 評価される
   *     (Codex round 3 medium 対応)
   *
   * 距離が `this.robotRadiusM` (#116) 未満になった瞬間に signature を Set に
   * 登録し、以降同 signature の間は描画しない (page 内 sticky)。
   *
   * polyline は active route と同色 + dashed (本線 polyline と差別化) で、
   * 中点に既存の route 矢印 SVG を再利用して方向を示す。
   */
  _updateRobotConnector() {
    this._clearRobotConnector();
    if (this._activeRouteIdx < 0) return;
    if (!this._lastRobotPose || !this.metadata) return;
    const item = this._routePolylines.find(
      (it) => it.routeIdx === this._activeRouteIdx,
    );
    if (!item || !item.latlngs || item.latlngs.length === 0) return;
    const signature = this._routeSignature(item);
    if (signature && this._reachedRouteSignatures.has(signature)) return;

    const robotLatLng = this.worldToLatLng(
      this._lastRobotPose.x, this._lastRobotPose.y,
    );
    // 視覚端点は offset 後 (displayLatlngs) を使い、parallel offset で動いた
    // polyline 始点に矢印が刺さるようにする (#69 PR-2)。displayLatlngs が無い
    // 古い entry / no-offset 時は latlngs にフォールバック。
    const firstLatLngVisual = (item.displayLatlngs && item.displayLatlngs[0])
      || item.latlngs[0];

    // 先頭 POI に到達済みなら Set に登録して以降描かない。
    // 到達判定は offset で動かない物理座標 (latlngs[0] = POI 実位置) で評価する
    // (offset 後位置で判定すると同じ POI に到達しているのに route ごとに到達タイ
    //  ミングがずれる)。
    const firstWorld = this.latLngToWorld(item.latlngs[0]);
    const dx = this._lastRobotPose.x - firstWorld.x;
    const dy = this._lastRobotPose.y - firstWorld.y;
    if (Math.hypot(dx, dy) < this.robotRadiusM) {
      if (signature) {
        this._reachedRouteSignatures.add(signature);
      }
      return;
    }

    const line = L.polyline([robotLatLng, firstLatLngVisual], {
      color: item.color,
      weight: 3,
      opacity: 0.8,
      dashArray: '4, 6',
      interactive: false,
    }).addTo(this.map);
    if (this._layerPriorityMode === 'navigation') line.bringToFront();
    else line.bringToBack();
    this._robotConnectorLayers.push(line);

    const midLat = (robotLatLng.lat + firstLatLngVisual.lat) / 2;
    const midLng = (robotLatLng.lng + firstLatLngVisual.lng) / 2;
    const rotDeg = MapoiMapIcons.routeDirectionDeg(robotLatLng, firstLatLngVisual);
    const arrowIcon = L.divIcon({
      className: 'route-arrow',
      html: MapoiMapIcons.createRouteDirectionSvg(item.color, rotDeg, 18),
      iconSize: [18, 18],
      iconAnchor: [9, 9],
    });
    const arrowMarker = L.marker([midLat, midLng], {
      icon: arrowIcon,
      interactive: false,
      zIndexOffset: this._zIndex('robotConnector'),
    }).addTo(this.map);
    this._robotConnectorLayers.push(arrowMarker);
  }

  _clearRobotConnector() {
    if (!this._robotConnectorLayers) return;
    this._robotConnectorLayers.forEach((layer) => {
      this.map.removeLayer(layer);
    });
    this._robotConnectorLayers = [];
  }

  /**
   * route の到達履歴 sticky 用の identity signature。
   * routeName 単独だと「同名のまま先頭 waypoint 差替え / map 切替で別 route 再生成」
   * で stale entry が残る (Codex round 3 medium)。先頭 waypoint 名を加えた複合 ID
   * にすることで、これらの identity 変化で signature が変わり fresh 評価される。
   * routeName / firstWaypointName のいずれかが空なら identity 確定不可で空返し。
   */
  _routeSignature(item) {
    if (!item || !item.routeName || !item.firstWaypointName) return '';
    return `${item.routeName}|${item.firstWaypointName}`;
  }
}
