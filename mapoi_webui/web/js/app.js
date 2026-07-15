/**
 * Main application controller.
 * Wires together MapViewer, PoiEditor, RouteEditor, and API calls.
 */
(async function() {
  const mapViewer = new MapViewer('map');
  const poiEditor = new PoiEditor();
  const routeEditor = new RouteEditor();
  const tagEditor = new TagEditor();

  const mapSelect = document.getElementById('map-select');
  const navigationMapSelect = document.getElementById('navigation-map-select');
  const navGoalSelect = document.getElementById('nav-goal-select');
  const navRouteSelect = document.getElementById('nav-route-select');
  const navInitialPoseSelect = document.getElementById('nav-initialpose-select');
  const navStatusDot = document.getElementById('nav-status-dot');
  const navStatusText = document.getElementById('nav-status-text');
  // navigation/localization availability 表示は nav-controls.js が id 解決して適用する (#199)。
  const navigationWarning = document.getElementById('navigation-warning');
  const compactMediaQuery = window.matchMedia('(max-width: 768px)');
  let currentMap = '';
  let currentNavigationCapabilities = null;
  let currentNavStatus = 'idle';
  // 選択中 POI の位置編集 (drag + yaw ハンドル) のロック状態 (#333)。位置編集は
  // ナビゲーション/監視に対して「たまに行う設定変更」なので既定はロック (安全側)。
  // リロードで再度ロックへ戻り、解除状態は永続化しない。
  let poiPositionLocked = true;
  // SSE config_changed reload の debounce timer と dirty guard の保留 state (#373)。
  // 保留 (deferred) = 「blocker (未保存編集 / 開いている edit form) があり、ユーザーが
  // 編集続行 (Cancel) を選んだ」状態。blocker 解消時に保留分の reload を再開する。
  // in-flight/queued は reload 実行中に届いた次の config_changed の扱い: dirty クリアが
  // fetch 完了後になるため、実行中に再判定すると直後に同じ prompt を重ねて出してしまう。
  // 実行中は queue に積み、完了後に 1 回だけ再判定する。
  // 初期 load 中の onDirtyChange からも参照されるため IIFE 冒頭で宣言する。
  let configChangedTimer = null;
  let configReloadDeferred = false;
  let configReloadInFlight = false;
  let configReloadQueued = false;
  // 直近の config_changed event が運んできた config_version (#384)。debounce や in-flight
  // queue を跨いで保持し、自タブ発の変更 (= 既知 version) なら reload を skip する判定に使う。
  let configChangedVersion = null;

  // --- Tag editor (tag-editor.js に切り出し、#346) ---
  // save 成功 / discard 後は tag 定義の再取得のみ、409 確認後は全体 reload
  // (loadTagDefinitions → loadPois → loadRoutes) — poi-editor.js / route-editor.js の
  // onConflictReload と同じ順序。
  tagEditor.onReload = () => loadTagDefinitions();
  tagEditor.onConflictReload = async () => {
    await loadTagDefinitions();
    await loadPois();
    await loadRoutes();
  };
  tagEditor.onDirtyChange = (isDirty) => {
    if (!isDirty) maybeResumeDeferredConfigReload();
  };

  // --- Load maps list ---
  function populateMapSelect(selectEl, maps, selectedMap) {
    if (!selectEl) return;
    selectEl.innerHTML = '';
    maps.forEach((name) => {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      if (name === selectedMap) opt.selected = true;
      selectEl.appendChild(opt);
    });
  }

  async function loadMaps() {
    const data = await MapoiApi.getMaps();
    populateMapSelect(mapSelect, data.maps, data.current_map);
    populateMapSelect(navigationMapSelect, data.maps, data.current_map);
    currentMap = data.current_map;
    await switchMap(currentMap, { selectBackend: false });
    if (currentNavigationCapabilities) {
      updateNavControlsAvailability(currentNavigationCapabilities);
    }
  }

  // --- Switch map ---
  async function switchMap(mapName, options = {}) {
    const { selectBackend = true } = options;
    if (selectBackend) {
      const result = await MapoiApi.selectMap(mapName);
      if (result.error) {
        throw new Error(result.error);
      }
    }
    currentMap = mapName;
    if (mapSelect) mapSelect.value = mapName;
    if (navigationMapSelect) navigationMapSelect.value = mapName;
    await mapViewer.loadMap(mapName);
    await loadTagDefinitions();
    await loadPois();
    await loadRoutes();
    refreshResponsiveLayout();
  }

  // --- Load tag definitions ---
  async function loadTagDefinitions() {
    const data = await MapoiApi.getTagDefinitions();
    const tags = data.tags || [];
    poiEditor.setTagDefinitions(tags);
    mapViewer.setTagDefinitions(tags);
    // config_version は楽観的競合検出 token (#241 の展開, #343): backend から受け取り、
    // tagEditor の save() で送り返す。
    tagEditor.setTagDefinitions(tags, data.config_version);
  }

  // --- Load POIs ---
  async function loadPois() {
    const data = await MapoiApi.getPois();
    // config_version は楽観的競合検出 token (#241): backend から受け取り、save() で送り返す。
    poiEditor.loadPois(data.pois || [], data.config_version);
    mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
    populateNavGoalSelect(data.pois || []);
    populateInitialPoseSelect(data.pois || []);
    updateRouteEditorPoiNames();
  }

  function updateRouteEditorPoiNames() {
    // landmark POI は Nav2 navigation goal にできない reference 専用 (#85)。
    // route waypoint candidate からは除外、route.landmarks candidate に振り分ける (#143)。
    const navigablePois = MapoiPoiFilter.filterRouteWaypointCandidates(poiEditor.pois);
    routeEditor.setPoiNames(navigablePois.map((p) => p.name));
    const landmarkPois = (poiEditor.pois || []).filter((p) => MapoiPoiFilter.hasLandmarkTag(p));
    routeEditor.setLandmarkNames(landmarkPois.map((p) => p.name));
  }

  // --- Load routes ---
  async function loadRoutes() {
    const data = await MapoiApi.getRoutes();
    // config_version は楽観的競合検出 token (#241 の展開, #343): backend から受け取り、save() で送り返す。
    routeEditor.loadRoutes(data.routes || [], data.config_version);
    redrawRoutes();
    populateNavRouteSelect(routeEditor.routes);
  }

  function redrawRoutes() {
    if (routeEditor.editingIndex !== -1) {
      // Hide all other routes while editing; show only the editing preview
      mapViewer.clearRoutes();
      const color = routeEditor.getEditingRouteColor() || '#2980b9';
      mapViewer.showEditingRoutePreview(
        routeEditor.editingWaypoints, poiEditor.pois, color, routeEditor.editingLandmarks
      );
    } else {
      mapViewer.showRoutes(routeEditor.routes, poiEditor.pois, routeEditor.visibleRoutes);
    }
  }

  // --- Map selector change ---
  mapSelect.addEventListener('change', () => {
    if (poiEditor.dirty || routeEditor.dirty || tagEditor.dirty) {
      if (!confirm('Unsaved changes will be lost. Switch map?')) {
        mapSelect.value = currentMap;
        return;
      }
    }
    switchMap(mapSelect.value, { selectBackend: true }).catch((e) => {
      alert('Failed to switch map: ' + e.message);
      mapSelect.value = currentMap;
    });
  });

  if (navigationMapSelect) {
    navigationMapSelect.addEventListener('change', () => {
      requestNavigationMapSwitch(navigationMapSelect.value);
    });
  }

  // --- Wire callbacks ---

  function isRouteEditingActive() {
    return routeEditor.editingIndex !== -1;
  }

  function isPoiEditingActive() {
    return poiEditor.editingIndex !== -1 || poiEditor.placingMode;
  }

  function isNavigationActive() {
    return ['navigating', 'paused', 'map_switching'].includes(currentNavStatus);
  }

  function updateMapLayerPriority() {
    if (isNavigationActive()) {
      mapViewer.setLayerPriorityMode('navigation');
    } else if (isPoiEditingActive() || isRouteEditingActive()) {
      mapViewer.setLayerPriorityMode('editing');
    } else {
      mapViewer.setLayerPriorityMode('default');
    }
  }

  function syncRouteEditPoiCandidate(index) {
    if (routeEditor.editingIndex === -1 || index < 0) return '';
    const poi = poiEditor.pois[index];
    if (!poi || !poi.name) return '';
    return routeEditor.selectPoiCandidate(poi.name);
  }

  function restorePoiEditPreview(index) {
    if (index < 0) return;
    mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
    if (poiEditor.selectedIndex >= 0) {
      mapViewer.highlightPoi(poiEditor.selectedIndex);
    }
  }

  poiEditor.onBeforeEditStart = () => {
    if (!isRouteEditingActive()) return true;
    alert('Finish or cancel route editing before editing POIs.');
    return false;
  };

  routeEditor.onBeforeEditStart = () => {
    if (!isPoiEditingActive()) return true;
    alert('Finish or cancel POI editing before editing routes.');
    return false;
  };

  poiEditor.onEditCancel = (previousEditingIndex) => {
    restorePoiEditPreview(previousEditingIndex);
  };

  // Helper: enable pose tool for the currently editing POI
  function enablePoseToolForEditing() {
    const callbacks = {
      onPositionSet: (x, y) => {
        poiEditor.updateFormPosition(x, y);
        if (poiEditor.editingIndex >= 0) {
          mapViewer.updatePoiMarkerPosition(poiEditor.editingIndex, x, y);
        }
      },
      onYawSet: (yaw) => {
        poiEditor.updateFormYaw(yaw);
        if (poiEditor.editingIndex >= 0) {
          mapViewer.updatePoiMarkerYaw(poiEditor.editingIndex, yaw);
        }
      },
    };

    if (poiEditor.editingIndex === -2) {
      // New POI: placing click already set position → start in yaw phase
      const formX = parseFloat(poiEditor.inputX.value) || 0;
      const formY = parseFloat(poiEditor.inputY.value) || 0;
      mapViewer.enablePoseTool(callbacks, mapViewer.worldToLatLng(formX, formY));
    } else {
      // Existing POI: start in position phase (click 1 = position, click 2 = yaw)
      mapViewer.enablePoseTool(callbacks);
    }
  }

  // Map click — only used for placing new POI (pose tool handles editing clicks)
  mapViewer.onMapClick = (x, y) => {
    if (poiEditor.placingMode) {
      poiEditor.placeNewPoi(x, y);
      enablePoseToolForEditing();
    }
  };

  // POI marker click on map
  mapViewer.onPoiClick = (index) => {
    // Route editing uses map clicks for candidate selection by default.
    // Optional click-to-add keeps fast waypoint entry available without making
    // accidental route mutations the default interaction.
    if (routeEditor.editingIndex !== -1) {
      const poi = poiEditor.pois[index];
      if (poi && poi.name) {
        const candidateKind = syncRouteEditPoiCandidate(index);
        if (candidateKind !== 'waypoint') return;
        if (routeEditor.isMapInsertModeEnabled()) {
          routeEditor.insertWaypointAfterSelected(poi.name);
        }
      }
      return;
    }
    // 既存 POI の編集中に別 POI をシングルクリックしたら、編集モードを終了して
    // その POI を選択するだけに切り替える (#240)。フォームの未確定入力は破棄される。
    // 同じ POI を再クリックした時や新規 POI 配置中 (editingIndex === -2) は維持する。
    if (poiEditor.editingIndex >= 0 && poiEditor.editingIndex !== index) {
      poiEditor.exitEditMode();
    }
    poiEditor.selectPoi(index);
    mapViewer.highlightPoi(index);
  };

  // POI marker double-click on map — open edit panel (#240)。
  // シングルクリック = 選択のみ、ダブルクリック = 編集モード、と操作意図を分離して
  // 意図しない編集突入を防ぐ。route 編集中は POI クリックが waypoint 追加なので、
  // 編集パネルは開かず無視する (route 編集の流れを邪魔しない)。
  mapViewer.onPoiDblClick = (index) => {
    if (routeEditor.editingIndex !== -1) return;
    poiEditor.editPoi(index);
  };

  // 選択中 POI を地図上でドラッグして position 移動 (#239)。dragend で working copy の
  // 座標を更新 (dirty) し、marker + tolerance 扇形を新位置で再描画。確定はユーザーの
  // Save クリック (auto-POST しない)。drag した POI = 選択中 = 再 highlight 対象なので、
  // showPois 後に highlightPoi で再強調 + ドラッグ可を復元する。
  mapViewer.onPoiDragEnd = (index, x, y) => {
    poiEditor.updateDraggedPosition(index, x, y);
    mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
    mapViewer.highlightPoi(index);
  };

  // 選択中 wedge POI の扇形ハンドルで yaw を回転 (#275)。dragend で working copy の yaw を
  // 更新 (dirty) し、扇形・矢印を新 yaw で再描画 + ハンドルを正位置へ。確定は Save クリック。
  mapViewer.onPoiYawDragEnd = (index, yaw) => {
    poiEditor.updateDraggedYaw(index, yaw);
    mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
    mapViewer.highlightPoi(index);
  };

  // POI selection in list
  poiEditor.onSelectionChange = (index) => {
    mapViewer.highlightPoi(index);
    syncRouteEditPoiCandidate(index);
    // Refresh markers when dirty (POIs changed)
    if (poiEditor.dirty) {
      mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
      if (index >= 0) mapViewer.highlightPoi(index);
    }
    // 画面外の POI を選んだら map をパンして見せる (#382)。map クリック由来の選択は
    // クリックした POI が必ず viewport 内なのでパンしない (視点が飛ばない)。dirty 時の
    // showPois 再描画より後に呼び、再生成後の実 marker 位置で判定する。
    if (index >= 0) {
      mapViewer.panToPoiIfHidden(index);
    }
    // Enable pose tool when editing, disable when form closes
    if (poiEditor.editingIndex >= 0) {
      enablePoseToolForEditing();
    } else {
      mapViewer.disablePoseTool();
    }
    updateMapLayerPriority();
    syncNavGoalSelect();
  };

  poiEditor.onPlacingModeChange = () => {
    updateMapLayerPriority();
  };

  // POI visibility toggle
  poiEditor.onVisibilityChange = () => {
    mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
    if (poiEditor.selectedIndex >= 0) {
      mapViewer.highlightPoi(poiEditor.selectedIndex);
    }
  };

  // Dirty state change — refresh map markers and routes after save/discard/undo/redo。
  // isDirty=false になる経路は save 成功・discard・undo・redo と複数あり (Codex review
  // #334 round 1-4 で全経路が同じ問題を踏むと判明)、loadRoutes() はサーバ最新状態で
  // route working copy を丸ごと上書きするため、route 編集フォームが開いている間、または
  // OK 後 Save 前の未保存 route 変更が残っている間に走ると route 側の作業を黙って失う。
  // POI 側の操作自体 (undo/redo/discard/save) は route 編集と無関係にいつでも行えて
  // よく、危険なのは loadRoutes() という副作用の方なので、個々の POI 操作ではなく
  // ここ (route working copy を触る唯一の呼び出し元) で一元的にガードする。
  poiEditor.onDirtyChange = (isDirty) => {
    updateRouteEditorPoiNames();
    if (!isDirty) {
      mapViewer.showPois(poiEditor.pois, poiEditor.visiblePois);
      if (routeEditor.editingIndex === -1 && !routeEditor.dirty) {
        loadRoutes();
      } else {
        // loadRoutes() (サーバ最新化 + フォーム閉じ) は route working copy 保護のため
        // skip したが、クライアント側の表示は最新の poiEditor.pois に合わせて更新して
        // おかないと、開いている route フォームの waypoint/landmark select が古い POI
        // 名を出し続けたり、route polyline が undo 前の POI 座標のまま残ったりする
        // (Codex review #334 round 5)。どちらも poiEditor.pois の再取得済みデータだけで
        // 完結する再描画で、サーバ最新化 (route working copy 上書き) は伴わない。
        redrawRoutes();
        if (routeEditor.editingIndex !== -1) {
          // updateRouteEditorPoiNames() は関数先頭で呼び済み (routeEditor.poiNames は
          // 最新)。select の DOM 再構築だけここで行う。
          routeEditor.populateWaypointSelect();
          routeEditor.populateLandmarkSelect();
        }
      }
      maybeResumeDeferredConfigReload();
    }
  };

  // モバイルで #poi-body が畳まれていても POI のドラッグ誤操作をすぐ取り消せるよう、
  // undo 履歴がある間だけ地図上のフローティング Undo を出す (#332)。
  const btnPoiUndoFloat = document.getElementById('btn-poi-undo-float');
  if (btnPoiUndoFloat) {
    btnPoiUndoFloat.addEventListener('click', () => poiEditor.undo());
  }
  poiEditor.onHistoryChange = (canUndo) => {
    if (btnPoiUndoFloat) btnPoiUndoFloat.classList.toggle('hidden', !canUndo);
  };

  // 409 version_mismatch 受信時の全体 reload (#241)。POI だけ再 load しても route 側
  // の参照が壊れる可能性があるため、loadTagDefinitions → loadPois → loadRoutes を一括で。
  poiEditor.onConflictReload = async () => {
    await loadTagDefinitions();
    await loadPois();
    await loadRoutes();
  };

  // 409 version_mismatch 受信時の全体 reload (#241 の展開, #343)。route save 経路でも
  // poiEditor.onConflictReload と同じ理由 (loadTagDefinitions → loadPois → loadRoutes を
  // 一括で行い、全 working copy の参照を揃える) で同じ順序にする。
  routeEditor.onConflictReload = async () => {
    await loadTagDefinitions();
    await loadPois();
    await loadRoutes();
  };

  // 選択中 POI の位置編集ロック (#333)。route 編集中の抑止 (#239) と AND して
  // MapViewer.setPoiDraggingAllowed に渡す単一の同期点。
  function refreshPoiDraggingAllowed() {
    mapViewer.setPoiDraggingAllowed(!poiPositionLocked && routeEditor.editingIndex === -1);
  }

  const btnPoiLockToggle = document.getElementById('btn-poi-lock-toggle');
  const poiLockIconLocked = document.getElementById('poi-lock-icon-locked');
  const poiLockIconUnlocked = document.getElementById('poi-lock-icon-unlocked');
  function applyPoiLockUi() {
    if (btnPoiLockToggle) {
      btnPoiLockToggle.setAttribute('aria-pressed', String(poiPositionLocked));
      btnPoiLockToggle.title = poiPositionLocked
        ? 'Unlock POI position editing (L)' : 'Lock POI position editing (L)';
    }
    if (poiLockIconLocked) poiLockIconLocked.classList.toggle('hidden', !poiPositionLocked);
    if (poiLockIconUnlocked) poiLockIconUnlocked.classList.toggle('hidden', poiPositionLocked);
  }
  if (btnPoiLockToggle) {
    btnPoiLockToggle.addEventListener('click', () => {
      poiPositionLocked = !poiPositionLocked;
      applyPoiLockUi();
      refreshPoiDraggingAllowed();
    });
  }
  applyPoiLockUi();
  refreshPoiDraggingAllowed();

  // Route click on map
  mapViewer.onRouteClick = (routeIdx) => {
    // Ignore during editing
    if (routeEditor.editingIndex !== -1) return;
    routeEditor.selectRoute(routeIdx);
  };

  // Route selection change — highlight on map
  routeEditor.onSelectionChange = (index) => {
    mapViewer.highlightRoute(index);
    syncNavRouteSelect();
  };

  // Route editing state change — redraw routes with editing preview
  routeEditor.onEditingChange = () => {
    refreshPoiDraggingAllowed();
    redrawRoutes();
    // editRoute / copyRoute は selectedIndex を直接変えるが onSelectionChange を
    // 発火しないため、ここで highlightRoute を再適用して MapViewer 側の
    // _activeRouteIdx と同期する。これがないと「route A 選択 → route B Edit →
    // Cancel」で sidebar は B selected でも _activeRouteIdx は A のままになり、
    // robot connector が古い route の先頭 POI を指し続ける (PR #107 Codex round
    // 2/3 medium 残課題)。
    if (routeEditor.editingIndex === -1 && routeEditor.selectedIndex >= 0) {
      mapViewer.highlightRoute(routeEditor.selectedIndex);
    }
    updateMapLayerPriority();
    syncNavRouteSelect();
  };

  // Waypoint candidate/row focus during route editing highlights the matching POI marker.
  // This is preview-only UI feedback: it must not mutate route / POI dirty state (#305).
  routeEditor.onWaypointFocus = (name) => {
    if (!name) return;
    const index = poiEditor.pois.findIndex((p) => p.name === name);
    if (index < 0) return;
    mapViewer.highlightPoi(index);
  };

  routeEditor.onLandmarkFocus = (name) => {
    if (!name) return;
    const index = poiEditor.pois.findIndex((p) => p.name === name);
    if (index < 0) return;
    mapViewer.highlightPoi(index);
  };

  // Route visibility toggle
  routeEditor.onVisibilityChange = () => {
    redrawRoutes();
    if (routeEditor.editingIndex === -1 && routeEditor.selectedIndex >= 0) {
      mapViewer.highlightRoute(routeEditor.selectedIndex);
    }
  };

  // Route dirty state change — refresh routes on map and nav select after save/discard
  routeEditor.onDirtyChange = (isDirty) => {
    redrawRoutes();
    if (routeEditor.editingIndex === -1 && routeEditor.selectedIndex >= 0) {
      mapViewer.highlightRoute(routeEditor.selectedIndex);
    }
    if (!isDirty) {
      populateNavRouteSelect(routeEditor.routes);
      // populate で options が rebuild されるが、選択中なら value を再同期して
      // ユーザーの選択 state を保持する。
      syncNavRouteSelect();
      maybeResumeDeferredConfigReload();
    }
  };

  // --- Section toggles ---
  function refreshResponsiveLayout() {
    const header = document.querySelector('header');
    if (header) {
      document.documentElement.style.setProperty(
        '--header-height',
        `${Math.ceil(header.getBoundingClientRect().height)}px`
      );
    }
    window.requestAnimationFrame(() => {
      if (mapViewer.map && typeof mapViewer.map.invalidateSize === 'function') {
        mapViewer.map.invalidateSize();
      }
    });
  }

  // setupSectionToggle は { isOpen, setOpen } を返し、編集モード時のプログラム制御
  // (collapseSectionsForEditing) からも開閉できるようにする (#240)。
  function setupSectionToggle(toggleBtnId, bodyEl, initialOpen = true) {
    const btn = document.getElementById(toggleBtnId);
    let open = initialOpen;
    const apply = () => {
      bodyEl.style.display = open ? '' : 'none';
      btn.innerHTML = open ? '&#9660;' : '&#9654;';
    };
    apply();
    btn.addEventListener('click', () => {
      open = !open;
      apply();
      refreshResponsiveLayout();
    });
    return {
      isOpen: () => open,
      setOpen: (next) => {
        if (open === next) return;
        open = next;
        apply();
        refreshResponsiveLayout();
      },
    };
  }

  const compactInitial = compactMediaQuery.matches;
  // compact viewport 初期表示で開く section は nav-controls.js に集約 (#199)。
  const initialSections = MapoiNavControls.initialSectionOpenStates(compactInitial);
  const routeToggle = setupSectionToggle('btn-route-toggle', document.getElementById('route-body'), initialSections.route);
  const poiToggle = setupSectionToggle('btn-poi-toggle', document.getElementById('poi-body'), initialSections.poi);
  const tagToggle = setupSectionToggle('btn-tag-toggle', document.getElementById('tag-body'), initialSections.tag);
  if (typeof compactMediaQuery.addEventListener === 'function') {
    compactMediaQuery.addEventListener('change', refreshResponsiveLayout);
  }
  window.addEventListener('resize', refreshResponsiveLayout);
  refreshResponsiveLayout();

  // --- All / None buttons ---
  // Routes
  document.getElementById('btn-route-all').addEventListener('click', () => {
    routeEditor.setAllVisible();
  });
  document.getElementById('btn-route-none').addEventListener('click', () => {
    routeEditor.setAllHidden();
  });
  // POIs
  document.getElementById('btn-poi-all').addEventListener('click', () => {
    poiEditor.setAllVisible();
  });
  document.getElementById('btn-poi-none').addEventListener('click', () => {
    poiEditor.setAllHidden();
  });

  // --- Navigation controls ---
  function populateNavGoalSelect(pois) {
    navGoalSelect.innerHTML = '<option value="">-- Select POI --</option>';
    // landmark POI は reference 専用、Nav2 navigation candidate から除外 (#85)。
    MapoiPoiFilter.filterWaypointCandidates(pois).forEach((p) => {
      const opt = document.createElement('option');
      opt.value = p.name;
      opt.textContent = p.name;
      navGoalSelect.appendChild(opt);
    });
  }

  function populateNavRouteSelect(routes) {
    navRouteSelect.innerHTML = '<option value="">-- Select Route --</option>';
    routes.forEach((r) => {
      const opt = document.createElement('option');
      opt.value = r.name;
      opt.textContent = r.name;
      navRouteSelect.appendChild(opt);
    });
  }

  // Nav dropdowns auto-fill from POI/route selection (#111)。
  // 選択中の route 名 / POI 名を Navigation グループの dropdown に反映し、
  // 「選択 → 再選択 → Run/Go」の重複操作を 1 ステップ減らす。
  // 選択解除 (-1) では dropdown を維持する (再 nav 時の再利用性のため)。
  //
  // 重要: 該当 name が dropdown options に無い場合 (例: goal タグ無し POI、
  // 未保存の Copy 直後の route 等)、`select.value = missingName` は silent
  // no-op ではなく value を空 ("") に落とし前値を消してしまう。前値維持を
  // 守るため options 存在チェックしてから代入する (PR #114 Codex round 1
  // medium 対応)。
  function _hasOption(selectEl, name) {
    return Array.from(selectEl.options).some((o) => o.value === name);
  }

  function syncNavRouteSelect() {
    const idx = routeEditor.selectedIndex;
    if (idx < 0) return;
    const name = routeEditor.routes[idx]?.name;
    if (!name) return;
    if (_hasOption(navRouteSelect, name)) navRouteSelect.value = name;
  }

  function syncNavGoalSelect() {
    const idx = poiEditor.selectedIndex;
    if (idx < 0) return;
    const name = poiEditor.pois[idx]?.name;
    if (!name) return;
    if (_hasOption(navGoalSelect, name)) navGoalSelect.value = name;
  }

  function populateInitialPoseSelect(pois) {
    navInitialPoseSelect.innerHTML = '<option value="">-- Select POI --</option>';
    // landmark + initial_pose は排他、initial pose candidate から除外 (#85)。
    MapoiPoiFilter.filterInitialPoseCandidates(pois).forEach((p) => {
      const opt = document.createElement('option');
      opt.value = p.name;
      opt.textContent = p.name;
      navInitialPoseSelect.appendChild(opt);
    });
  }

  function setNavigationWarning(message) {
    if (!navigationWarning) return;
    navigationWarning.textContent = message || '';
    navigationWarning.classList.toggle('hidden', !message);
    refreshResponsiveLayout();
  }

  function updateNavControlsAvailability(navigation) {
    // 状態派生・DOM 適用は nav-controls.js に切り出し済 (#199)。ここは closure state
    // (currentNavigationCapabilities) の維持と responsive 再計算だけを担う。
    currentNavigationCapabilities = MapoiNavControls.resolveCapabilities(
      navigation,
      currentNavigationCapabilities,
    );
    const state = MapoiNavControls.deriveNavControlsState(currentNavigationCapabilities);
    MapoiNavControls.applyNavControlsState(state, document);
    refreshResponsiveLayout();
  }

  function ensureNoUnsavedChangesBeforeNavigationCommand() {
    if (!(poiEditor.dirty || routeEditor.dirty || tagEditor.dirty)) return true;
    return confirm('Unsaved changes may be replaced by the robot map switch. Continue?');
  }

  async function requestNavigationMapSwitch(mapName) {
    if (!mapName) return;
    if (!ensureNoUnsavedChangesBeforeNavigationCommand()) {
      if (navigationMapSelect) navigationMapSelect.value = currentMap;
      if (mapSelect) mapSelect.value = currentMap;
      return;
    }
    try {
      setNavigationWarning('');
      const result = await MapoiApi.navSwitchMap(mapName);
      // error / warning / switching の分岐は nav-controls.js の純関数に集約 (#199)。
      const outcome = MapoiNavControls.decideMapSwitchOutcome(result);
      if (outcome.kind !== 'switching') {
        setNavigationWarning(outcome.message);
        if (navigationMapSelect) navigationMapSelect.value = currentMap;
        // error は header の Map: も巻き戻す (両 select)。warning は navigation 側のみ。
        if (outcome.revert === 'both' && mapSelect) mapSelect.value = currentMap;
        return;
      }
      updateNavStatus('map_switching', mapName);
    } catch (e) {
      // fetch 自体の失敗 (network 等)。result.error 由来のメッセージ整形は
      // decideMapSwitchOutcome 側と揃えてある。
      setNavigationWarning('Map switch failed: ' + e.message);
      if (navigationMapSelect) navigationMapSelect.value = currentMap;
      if (mapSelect) mapSelect.value = currentMap;
    }
  }

  const statusLabels = {
    idle: 'Idle',
    navigating: 'Navigating',
    succeeded: 'Succeeded',
    aborted: 'Aborted',
    canceled: 'Canceled',
    paused: 'Paused',
    map_switching: 'Switching map',
    map_switch_succeeded: 'Map switched',
    map_switch_failed: 'Map switch failed',
    backend_unavailable: 'Navigation backend unavailable',
    rejected: 'Rejected',
  };

  function updateNavStatus(status, target) {
    const s = status || 'idle';
    currentNavStatus = s;
    navStatusDot.className = 'nav-status-dot nav-status-' + s;
    let label = statusLabels[s] || s;
    if (target && s !== 'idle') {
      label += ': ' + target;
    }
    navStatusText.textContent = label;
    updateMapLayerPriority();
  }

  let navPollingTimer = null;
  async function pollNavStatus() {
    try {
      const data = await MapoiApi.navStatus();
      updateNavStatus(data.status, data.target);
      updateNavControlsAvailability(data.navigation);
      // robot_radius は launch param 由来 (#117)。古い backend では key
      // 不在になるが setRobotRadius が型 check で no-op にする。値が来る
      // 前に updateRobotMarker が走っても map-viewer の default 0.15 で
      // 描画されるので safe。
      mapViewer.setRobotRadius(data.robot_radius);
      mapViewer.updateRobotMarker(data.robot_pose);
    } catch (e) {
      // ignore fetch errors
    }
  }

  function startNavStatusPolling() {
    if (navPollingTimer) return;
    pollNavStatus();
    navPollingTimer = setInterval(pollNavStatus, 1000);
  }

  // Navigation プルダウン選択 → map 上で選択状態にする (#262)。
  // プルダウンで POI / route を選んだら editor 選択経路 (selectPoi / selectRoute) に
  // 流し込み、既存の onSelectionChange (→ highlightPoi/highlightRoute + syncNav*) に
  // map ハイライトと dropdown 逆同期を委ねる。これで list / map / dropdown の選択 state を
  // 単一ソース (poiEditor.selectedIndex / routeEditor.selectedIndex) で揃える。
  // 空 ("-- Select --") は選択解除ではなく no-op とし現在の選択を維持する
  // (Run/Go ボタンの `if (!name) return;` と同じく「未選択 = 操作しない」)。
  function highlightPoiFromNav(name) {
    if (!name) return;
    const index = poiEditor.pois.findIndex((p) => p.name === name);
    if (index < 0) return;
    // 既に選択中なら再 render を避け、map ハイライトだけ確実に反映する。
    if (index === poiEditor.selectedIndex) {
      mapViewer.highlightPoi(index);
      return;
    }
    poiEditor.selectPoi(index);
  }

  // goal POI / initial pose POI どちらの dropdown も同じ POI 選択経路を共有する
  // (#111 の goal 欄 auto-fill もこの選択経由でそのまま効く)。
  navGoalSelect.addEventListener('change', () => {
    highlightPoiFromNav(navGoalSelect.value);
  });
  navInitialPoseSelect.addEventListener('change', () => {
    highlightPoiFromNav(navInitialPoseSelect.value);
  });

  navRouteSelect.addEventListener('change', () => {
    const name = navRouteSelect.value;
    if (!name) return;
    const index = routeEditor.routes.findIndex((r) => r.name === name);
    if (index < 0) return;
    // routeEditor.selectRoute は toggle (同 index 再選択で -1 解除)。dropdown 経由で
    // 既選択 route を選び直した時に誤って解除しないよう、選択中なら highlight のみ。
    if (index === routeEditor.selectedIndex) {
      mapViewer.highlightRoute(index);
      return;
    }
    routeEditor.selectRoute(index);
  });

  document.getElementById('btn-nav-go').addEventListener('click', async () => {
    const name = navGoalSelect.value;
    if (!name) return;
    await MapoiApi.navGoal(name);
    updateNavStatus('navigating', name);
  });

  document.getElementById('btn-nav-run').addEventListener('click', async () => {
    const name = navRouteSelect.value;
    if (!name) return;
    await MapoiApi.navRoute(name);
    updateNavStatus('navigating', name);
  });

  document.getElementById('btn-nav-setpose').addEventListener('click', async () => {
    const name = navInitialPoseSelect.value;
    if (!name) return;
    await MapoiApi.navInitialPose(name);
  });

  document.getElementById('btn-nav-pause').addEventListener('click', async () => {
    await MapoiApi.navPause();
    updateNavStatus('paused', '');
  });

  document.getElementById('btn-nav-resume').addEventListener('click', async () => {
    await MapoiApi.navResume();
    updateNavStatus('navigating', '');
  });

  document.getElementById('btn-nav-stop').addEventListener('click', async () => {
    await MapoiApi.navCancel();
    updateNavStatus('canceled', '');
  });

  const navToggle = setupSectionToggle('btn-nav-toggle', document.getElementById('nav-body'), initialSections.nav);
  startNavStatusPolling();

  // --- 編集モード時の sidebar focus (#240 / #302) ---
  // POI / Route の edit form を開いたら他セクションを畳み、対象 list も隠して form +
  // compact toolbar に集中させる。閉じたら元の開閉状態と list 表示へ戻す。
  const poiListEl = document.getElementById('poi-list');
  const routeListEl = document.getElementById('route-list');
  let savedSidebarFocus = null;
  let activeSidebarFocus = null;
  function activateSidebarFocus(editor) {
    if (activeSidebarFocus === editor) return;
    if (activeSidebarFocus) restoreSidebarFocus(activeSidebarFocus);
    savedSidebarFocus = {
      nav: navToggle.isOpen(),
      poi: poiToggle.isOpen(),
      tag: tagToggle.isOpen(),
      route: routeToggle.isOpen(),
      poiListDisplay: poiListEl ? poiListEl.style.display : '',
      routeListDisplay: routeListEl ? routeListEl.style.display : '',
    };
    activeSidebarFocus = editor;
    navToggle.setOpen(false);
    tagToggle.setOpen(false);
    if (editor === 'poi') {
      poiToggle.setOpen(true);
      routeToggle.setOpen(false);
      if (poiListEl) poiListEl.style.display = 'none';
    } else if (editor === 'route') {
      poiToggle.setOpen(false);
      routeToggle.setOpen(true);
      if (routeListEl) routeListEl.style.display = 'none';
    }
  }
  function restoreSidebarFocus(editor) {
    if (activeSidebarFocus !== editor || !savedSidebarFocus) return;
    if (poiListEl) poiListEl.style.display = savedSidebarFocus.poiListDisplay;
    if (routeListEl) routeListEl.style.display = savedSidebarFocus.routeListDisplay;
    navToggle.setOpen(savedSidebarFocus.nav);
    poiToggle.setOpen(savedSidebarFocus.poi);
    tagToggle.setOpen(savedSidebarFocus.tag);
    routeToggle.setOpen(savedSidebarFocus.route);
    savedSidebarFocus = null;
    activeSidebarFocus = null;
  }
  poiEditor.onEditFormVisibilityChange = (isOpen) => {
    if (isOpen) activateSidebarFocus('poi');
    else restoreSidebarFocus('poi');
    updateMapLayerPriority();
    // edit form open は dirty と独立した reload blocker (#373)。close 時に保留分を再開判定。
    if (!isOpen) maybeResumeDeferredConfigReload();
  };
  routeEditor.onEditFormVisibilityChange = (isOpen) => {
    if (isOpen) activateSidebarFocus('route');
    else restoreSidebarFocus('route');
    updateMapLayerPriority();
    if (!isOpen) maybeResumeDeferredConfigReload();
  };

  // --- Initialize ---
  await loadMaps();

  // --- SSE: rviz / 外部 save 由来の config 変更で再 fetch (#135 (B)) ---
  // 初期 load (loadMaps → switchMap → loadPois/Routes/TagDefs) 完了後に setup する
  // (#173 Round 2 high: 初期化前の SSE 受信で reload が前提データ未 load 状態に
  // なるのを回避)。mapoi_webui_node が mapoi/config_path topic 受信時に push する。
  // EventSource は browser が自動 reconnect するので最初の setup のみ。
  // 短時間のバースト時は 200ms にまとめて 1 回だけ fetch する debounce (#173 Round 1 medium)。
  // 未保存編集や開いている edit form があるうちは黙って loadMaps() せず、conflict
  // ダイアログ相当の選択 (最新読込 or 編集続行) を挟む (#373)。判定は reload-guard.js。
  // 自タブ発の変更 (save 応答で受領済みの config_version と一致) は reload 自体を skip
  // する (#384) — reload は undo 履歴 (loadPois が破棄) と map 視点 (loadMap の fitBounds)
  // を失わせるだけで、working copy は既に最新のため。
  // version 引数は event 受信時のみ渡す (null = version 不明)。maybeResume からの再呼び出し
  // は引数なしで、保留中 event の version をそのまま使う。

  // collectReloadBlockers に渡す現在の editor state (#373 / #380 共通)。SSE reload guard と
  // beforeunload guard は「失われるもの」の定義を共有するので、state の収集も一本化して
  // 片方だけの更新漏れ (定義乖離) を防ぐ。tag editor は inline の add form のみで開閉式
  // edit form を持たないため *FormOpen は POI / route の 2 つ (tag は dirty のみ blocker)。
  function currentEditorBlockerState() {
    return {
      poiDirty: poiEditor.dirty,
      routeDirty: routeEditor.dirty,
      tagDirty: tagEditor.dirty,
      poiFormOpen: poiEditor.editingIndex !== -1,
      routeFormOpen: routeEditor.editingIndex !== -1,
    };
  }

  function scheduleConfigChangedReload(version) {
    if (version !== undefined) configChangedVersion = version;
    if (configChangedTimer) clearTimeout(configChangedTimer);
    configChangedTimer = setTimeout(() => {
      configChangedTimer = null;
      if (configReloadInFlight) {
        configReloadQueued = true;
        return;
      }
      if (MapoiReloadGuard.isSelfConfigChange(configChangedVersion, [
        poiEditor.configVersion, routeEditor.configVersion, tagEditor.configVersion,
      ])) {
        // 従来この同期は reload の GET 群が担っていた: version は yaml 全体の hash なので
        // 一 section の save で他 section の保持 version も古くなる。skip する場合もここで
        // 配っておかないと、直後の別 section save が誤って 409 conflict に落ちる。
        poiEditor.configVersion = configChangedVersion;
        routeEditor.configVersion = configChangedVersion;
        tagEditor.configVersion = configChangedVersion;
        return;
      }
      const blockers = MapoiReloadGuard.collectReloadBlockers(currentEditorBlockerState());
      const action = MapoiReloadGuard.decideReloadAction(blockers, configReloadDeferred);
      if (action === 'hold') return;
      if (action === 'prompt'
          && !window.confirm(MapoiReloadGuard.buildReloadConfirmMessage(blockers))) {
        // 編集続行: reload を保留。configVersion が古いまま残るので、後続の Save は
        // #343 の 409 安全網 (version_mismatch conflict ダイアログ) に落ちる。
        configReloadDeferred = true;
        return;
      }
      configReloadDeferred = false;
      configReloadInFlight = true;
      loadMaps()
        .catch((err) => console.error('SSE reload failed:', err))
        .finally(() => {
          configReloadInFlight = false;
          if (configReloadQueued) {
            configReloadQueued = false;
            scheduleConfigChangedReload();
          }
        });
    }, 200);
  }

  // 保留中 reload の再開点 (#373)。blocker になり得る state の解消時 (dirty クリア /
  // edit form close) に呼ぶ。判定は scheduleConfigChangedReload の decide に集約して
  // いるので、blocker が残っていれば hold のまま、全て解消済みなら reload が走る。
  function maybeResumeDeferredConfigReload() {
    if (!configReloadDeferred) return;
    scheduleConfigChangedReload();
  }

  const evtSrc = new EventSource('/api/events');
  evtSrc.onmessage = (e) => {
    try {
      const data = JSON.parse(e.data);
      if (data.type === 'config_changed') {
        // config_version 無し (旧 backend / config 不在) は null で上書きし、前 event の
        // version で誤って self 判定しないようにする (#384)。
        scheduleConfigChangedReload((data.payload && data.payload.config_version) || null);
      } else if (data.type === 'command_rejected') {
        // #354: mapoi/nav/command_rejected 経由の非破壊イベント通知。走行中の typo goal 等
        // でも mapoi/nav/status (latched snapshot) を汚さず、一時的な toast だけで知らせる。
        const target = data.payload && data.payload.target;
        // target が取れない場合は末尾空白の "Command rejected: " を出さず一般文言にする。
        MapoiToast.showToast(
          document, target ? `Command rejected: ${target}` : 'Command rejected');
      }
    } catch (err) {
      console.error('Invalid SSE event:', err);
    }
  };
  evtSrc.onerror = (err) => {
    // EventSource は readyState=CONNECTING に戻って自動 reconnect する
    console.warn('SSE connection error, browser will auto-reconnect:', err);
  };

  // --- Keyboard shortcuts (#300 / #303 / #321 / #374 / #375) ---
  // active editor の Escape / Undo/Redo / Delete / Ctrl+S を document レベルで拾う。
  // Escape はフォームの Cancel 相当として編集モードを抜ける。Undo/Redo は入力欄
  // (name / x / y / tags 等) の編集中はブラウザ標準の text undo を優先するため無視する。
  // owned key のみ preventDefault し、他ハンドラを妨げない。

  // Ctrl+S の保存対象決定 (#375)。編集フォームが開いていればまずフォーム内容を working
  // copy に確定 (OK ボタン相当の formOk) してから save する — フォーム内の入力は OK まで
  // working copy に載らないため、確定せずに save すると「見えている編集が保存されない」
  // 齟齬が起きる。formOk は validation 失敗時に alert を出してフォームを開いたまま返る
  // ので、editingIndex が -1 に戻らなかったら保存へ進まない。フォームが無ければ dirty な
  // エディタを全て保存する (POI / route / tag は別 endpoint で独立、各 save() は 409
  // conflict ダイアログ (#241/#343) 含め既存経路をそのまま通す)。
  // 保存対象が無い時は toast で応答を返す: ショートカットが「効かない」ように見える
  // 無反応を避ける (ブラウザ拡張のキー横取り等との切り分けにもなる)。
  let saveShortcutInFlight = false;
  async function saveViaShortcut() {
    if (routeEditor.editingIndex !== -1) {
      routeEditor.formOk();
      if (routeEditor.editingIndex !== -1) return;
    } else if (poiEditor.editingIndex !== -1) {
      poiEditor.formOk();
      if (poiEditor.editingIndex !== -1) return;
    }
    if (!(poiEditor.dirty || routeEditor.dirty || tagEditor.dirty)) {
      MapoiToast.showToast(document, 'No unsaved changes');
      return;
    }
    if (poiEditor.dirty) await poiEditor.save();
    if (routeEditor.dirty) await routeEditor.save();
    if (tagEditor.dirty) await tagEditor.save();
    // 保存完了の合図 (#384)。従来は自タブ save 起因の SSE reload のちらつきが事実上の
    // 合図になっていたが、その reload を skip したので明示のフィードバックを出す。
    // 409 conflict 等で dirty が残った場合は出さない (各 save() のダイアログが出ている)。
    if (!(poiEditor.dirty || routeEditor.dirty || tagEditor.dirty)) {
      MapoiToast.showToast(document, 'Saved');
    }
  }

  document.addEventListener('keydown', (e) => {
    const t = e.target;
    const isEditableTarget = t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA'
        || t.tagName === 'SELECT' || t.isContentEditable);
    // Help overlay (#391) 表示中は modal として振る舞う: Escape は「閉じる」を最優先
    // (フォーム Cancel 等の段階挙動より先、stopImmediatePropagation で pose tool の
    // 独立 handler にも波及させない)。Ctrl+S はブラウザの保存ダイアログ抑止だけ行い
    // 保存はしない。他の owned key (L/U/Delete/undo 系/?) は閲覧中の誤操作 (見えない
    // POI 削除等) を防ぐため不発にする。所有しないキーには干渉しない。
    if (MapoiHelp.isOpen()) {
      if (e.key === 'Escape' && !e.isComposing) {
        e.preventDefault();
        e.stopImmediatePropagation();
        MapoiHelp.close();
      } else if ((e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey
          && e.key.toLowerCase() === 's') {
        e.preventDefault();
      }
      return;
    }
    // Ctrl+S / Cmd+S で dirty なエディタを Save (#375)。ブラウザ既定の「ページを保存」は
    // どの状態でも誤って出したくないため、入力欄 focus 中 (フォーム入力途中の確定 → save
    // も #375 の仕様) や保存対象が無い時も含め常に preventDefault する。このため
    // isEditableTarget guard より前に置く。Ctrl+Shift+S (ブラウザの別機能) は所有しない。
    if ((e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey && e.key.toLowerCase() === 's') {
      e.preventDefault();
      // 長押し repeat / 保存中の再入は無視: dirty クリアが fetch 完了後のため、再入する
      // と同じ configVersion で二重 POST になり直後に 409 conflict を踏む。
      if (e.repeat || saveShortcutInFlight) return;
      saveShortcutInFlight = true;
      saveViaShortcut().finally(() => { saveShortcutInFlight = false; });
      return;
    }
    if (e.key === 'Escape') {
      if (e.isComposing) return;
      if (routeEditor.editingIndex !== -1) {
        e.preventDefault();
        e.stopImmediatePropagation();
        routeEditor.formCancel();
        return;
      }
      if (poiEditor.editingIndex !== -1) {
        e.preventDefault();
        e.stopImmediatePropagation();
        if (mapViewer.cancelPoseToolPendingPosition()) return;
        poiEditor.formCancel();
        return;
      }
      if (poiEditor.placingMode) {
        e.preventDefault();
        e.stopImmediatePropagation();
        poiEditor.cancelPlacing();
        return;
      }
      if (mapViewer.isPoseToolActive()) return;
      if (isEditableTarget) return;
      const hadSelection = poiEditor.selectedIndex >= 0 || routeEditor.selectedIndex >= 0
        || navGoalSelect.value || navRouteSelect.value || navInitialPoseSelect.value;
      if (!hadSelection) return;
      e.preventDefault();
      e.stopImmediatePropagation();
      poiEditor.clearSelection();
      routeEditor.clearSelection();
      navGoalSelect.value = '';
      navRouteSelect.value = '';
      navInitialPoseSelect.value = '';
      return;
    }
    if (isEditableTarget) return;
    // 選択中 POI の Delete キー削除 (#374)。削除前 confirm は置かない: 削除は working
    // copy への変更で Save まで永続化されず Ctrl+Z (#300) で 1 手で戻せるため、
    // toast + undo 案内で足りる (issue #374 の選択肢のうち後者)。route 編集中は map
    // クリックが waypoint 操作の文脈なので発火しない (waypoint 削除への適用は scope 外)。
    // POI form 編集中・配置中も、フォーム入力途中の誤爆と選択対象の曖昧さを避け無視する。
    if (e.key === 'Delete' && !e.ctrlKey && !e.metaKey && !e.altKey && !e.shiftKey) {
      if (isRouteEditingActive() || isPoiEditingActive()) return;
      const index = poiEditor.selectedIndex;
      if (index < 0) return;
      e.preventDefault();
      const name = poiEditor.pois[index]?.name;
      poiEditor.deletePoi(index);
      MapoiToast.showToast(
        document, name ? `Deleted POI "${name}" (Ctrl+Z to undo)` : 'Deleted POI (Ctrl+Z to undo)');
      return;
    }
    // POI 位置ロック (L) / UI 一括表示 (U) の単キートグル (#390)。キーは対象の頭文字
    // (Lock / UI)。U は Hide/Show が状態で反転して頭文字にできない (H 案は棄却) ため。
    // 地図右上の既存ボタンの .click() を呼ぶだけの single code path (状態同期・永続化は
    // 各 click handler に集約済み)。Ctrl+L (アドレスバー) / Ctrl+U (ソース表示) 等の
    // ブラウザショートカットは所有しないため修飾キーなしに限定。toggle なので長押し
    // repeat で状態が暴れないよう e.repeat は無視する。toast は出さない: どちらの
    // ボタンも地図上に常時見えており (ui-hidden 中も #ui-controls は残る)、
    // icon / aria-pressed の変化で足りる。
    if (!e.ctrlKey && !e.metaKey && !e.altKey && !e.repeat) {
      const key = e.key.toLowerCase();
      if (key === 'l') {
        e.preventDefault();
        if (btnPoiLockToggle) btnPoiLockToggle.click();
        return;
      }
      if (key === 'u') {
        e.preventDefault();
        // 配線は ui-settings.js initDom。app.js は要素を掴むだけで状態を持たない。
        const btnUiToggle = document.getElementById('btn-ui-toggle');
        if (btnUiToggle) btnUiToggle.click();
        return;
      }
      // Help overlay (#391)。key ('?' は toLowerCase 不変) での判定は Shift+/ (US/JIS
      // とも) を含む layout 非依存 (e.code は見ない)。開くだけの idempotent 操作
      // (閉じるのは Escape / × / 背景 click)。shiftKey は '?' の入力自体に要るので
      // guard しない。
      if (key === '?') {
        e.preventDefault();
        MapoiHelp.open();
        return;
      }
    }
    if (!(e.ctrlKey || e.metaKey)) return;
    const key = e.key.toLowerCase();
    if (key === 'z' && !e.shiftKey) {
      e.preventDefault();
      if (routeEditor.editingIndex !== -1) routeEditor.undo();
      else poiEditor.undo();
    } else if ((key === 'z' && e.shiftKey) || key === 'y') {
      e.preventDefault();
      if (routeEditor.editingIndex !== -1) routeEditor.redo();
      else poiEditor.redo();
    }
  });

  // --- 未保存編集の離脱ガード (#380) ---
  // dirty なエディタ / 開いている edit form があるままタブを閉じる・F5 リロード・ページ
  // 遷移すると、編集内容と undo 履歴が黙って消える。map 切替 (既存 confirm) と SSE 全体
  // reload (#373 の reload guard) は既にガード済みで、タブ閉じ / リロードが最後の無防備
  // 経路。blocker の定義は reload-guard.js の collectReloadBlockers (「reload で失われる
  // もの」と同一 = dirty 3 editor + 開いている edit form) をそのまま再利用する。
  // clean 時は必ず素通し (常時ブロックはブラウザにダイアログ自体を抑止される + UX 悪)。
  // 文言はブラウザ標準でカスタム不可。preventDefault + returnValue 設定が互換のお作法。
  window.addEventListener('beforeunload', (e) => {
    const blockers = MapoiReloadGuard.collectReloadBlockers(currentEditorBlockerState());
    if (!MapoiReloadGuard.shouldBlockUnload(blockers)) return;
    e.preventDefault();
    e.returnValue = '';
  });

  // 初期化完了 (keydown listener 登録まで済み) の目印 (#375)。POI marker は初期化途中
  // (loadMaps 内の loadPois) で描画されるため、marker 出現だけを待って直後に keyboard
  // 操作すると listener 未登録に当たる race がある。e2e はこの flag で完了を待つ。
  document.body.dataset.mapoiReady = '1';
})();
