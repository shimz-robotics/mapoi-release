/**
 * Route editor: list display, edit form, dirty state tracking.
 * Mirrors PoiEditor structure for routes.
 */
class RouteEditor {
  constructor() {
    this.routes = [];
    this.originalRoutes = [];
    this.dirty = false;
    // 楽観的競合検出用 yaml ハッシュ (#241 の展開, #343)。GET 時に backend から受け取り、
    // Save 時に送り返す。poi-editor.js の configVersion と同じ役割。
    this.configVersion = null;
    this.editingIndex = -1; // -1 = closed, >=0 = editing, -2 = new route
    this.selectedIndex = -1; // -1 = none selected
    this.visibleRoutes = new Set();
    this.poiNames = [];          // navigable POI names (waypoint candidate)
    this.landmarkNames = [];     // landmark POI names (route.landmarks candidate, #143)
    this.editingWaypoints = []; // working waypoint list for form
    this.editingLandmarks = []; // working landmark list for form (#143)
    this.undoStack = [];
    this.redoStack = [];
    this.HISTORY_CAP = 50;
    this.dragWaypointIndex = -1;
    this.selectedWaypointIndex = -1;
    this.mapInsertMode = false;

    this.onDirtyChange = null;
    this.onVisibilityChange = null;
    this.onEditingChange = null; // callback(isEditing) - fired when editing starts/stops
    this.onSelectionChange = null; // callback(index) - fired when selection changes
    this.onWaypointFocus = null; // callback(poiName) - fired when waypoint UI previews a POI
    this.onLandmarkFocus = null; // callback(poiName) - fired when landmark UI previews a POI
    this.onEditFormVisibilityChange = null; // callback(isOpen)
    this.onBeforeEditStart = null; // callback(action) -> false blocks route edit/add/copy/delete
    // 409 受信時に app.js 側で全体 reload を発火させるためのフック (#343)。
    // poi-editor.js の onConflictReload と同じ役割。
    this.onConflictReload = null;  // async callback() — full reload on version_mismatch

    // DOM references
    this.listEl = document.getElementById('route-list');
    this.formEl = document.getElementById('route-edit-form');
    this.formTitle = document.getElementById('route-form-title');
    this.btnSaveRoutes = document.getElementById('btn-save-routes');
    this.btnDiscardRoutes = document.getElementById('btn-discard-routes');
    this.btnAddRoute = document.getElementById('btn-add-route');
    this.dirtyIndicator = document.getElementById('route-dirty-indicator');
    this.btnUndo = document.getElementById('btn-route-undo');
    this.btnRedo = document.getElementById('btn-route-redo');

    // Form inputs - waypoints
    this.inputRouteName = document.getElementById('route-name');
    this.waypointListEl = document.getElementById('route-waypoint-list');
    this.waypointSelect = document.getElementById('route-wp-select');
    this.btnAddWaypoint = document.getElementById('btn-add-waypoint');
    this.mapInsertToggle = document.getElementById('route-map-insert-toggle');

    // Form inputs - landmarks (#143)
    this.landmarkListEl = document.getElementById('route-landmark-list');
    this.landmarkSelect = document.getElementById('route-lm-select');
    this.btnAddLandmark = document.getElementById('btn-add-landmark');

    // Bind button events
    document.getElementById('btn-route-form-ok').addEventListener('click', () => this.formOk());
    document.getElementById('btn-route-form-cancel').addEventListener('click', () => this.formCancel());
    this.btnAddRoute.addEventListener('click', () => this.startAddRoute());
    this.btnSaveRoutes.addEventListener('click', () => this.save());
    this.btnDiscardRoutes.addEventListener('click', () => this.discard());
    this.btnAddWaypoint.addEventListener('click', () => this.addWaypointFromSelect());
    if (this.mapInsertToggle) {
      this.mapInsertToggle.addEventListener('change', () => {
        this.mapInsertMode = this.mapInsertToggle.checked;
      });
    }
    this.waypointSelect.addEventListener('change', () => this.focusWaypointName(this.waypointSelect.value));
    this.waypointSelect.addEventListener('focus', () => this.focusWaypointName(this.waypointSelect.value));
    if (this.landmarkSelect) {
      this.landmarkSelect.addEventListener('change', () => this.focusLandmarkName(this.landmarkSelect.value));
      this.landmarkSelect.addEventListener('focus', () => this.focusLandmarkName(this.landmarkSelect.value));
    }
    if (this.btnUndo) this.btnUndo.addEventListener('click', () => this.undo());
    if (this.btnRedo) this.btnRedo.addEventListener('click', () => this.redo());
    if (this.btnAddLandmark) {
      this.btnAddLandmark.addEventListener('click', () => this.addLandmarkFromSelect());
    }
  }

  /**
   * Set available landmark POI names for route.landmarks dropdown (#143).
   */
  setLandmarkNames(names) {
    this.landmarkNames = names || [];
  }

  /**
   * Load routes from server response and reset state.
   */
  loadRoutes(routes, configVersion) {
    this.routes = JSON.parse(JSON.stringify(routes));
    this.originalRoutes = JSON.parse(JSON.stringify(routes));
    this.editingIndex = -1;
    this.selectedIndex = -1;
    this.visibleRoutes = new Set(routes.map((r) => r.name));
    // 楽観的競合検出 token (#241 の展開, #343)。後続の save() で backend に送り返す。
    this.configVersion = configVersion || null;
    this._resetEditHistory();
    this.setDirty(false);
    this.hideForm();
    this.renderList();
  }

  /**
   * Set available POI names for waypoint dropdown.
   */
  setPoiNames(names) {
    this.poiNames = names || [];
  }

  isMapInsertModeEnabled() {
    return !!this.mapInsertMode;
  }

  /**
   * Render the route list.
   */
  renderList() {
    this.listEl.innerHTML = '';
    this.routes.forEach((route, idx) => {
      const color = this.getRouteColor(idx);
      const item = document.createElement('div');
      let cls = 'route-item';
      if (idx === this.editingIndex) cls += ' editing';
      if (idx === this.selectedIndex) cls += ' selected';
      item.className = cls;

      item.addEventListener('click', () => {
        this.selectRoute(idx);
      });

      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = this.visibleRoutes.has(route.name);
      cb.addEventListener('click', (e) => e.stopPropagation());
      cb.addEventListener('change', () => {
        if (cb.checked) {
          this.visibleRoutes.add(route.name);
        } else {
          this.visibleRoutes.delete(route.name);
        }
        if (this.onVisibilityChange) this.onVisibilityChange();
      });

      const swatch = document.createElement('span');
      swatch.className = 'route-color-swatch';
      swatch.style.background = color;

      const textWrap = document.createElement('span');
      textWrap.className = 'route-item-text';

      const nameSpan = document.createElement('span');
      nameSpan.className = 'route-item-name';
      // textContent と title を同じ正規化値で揃える (POI 側 pattern と一貫、
      // Codex PR #125 round 1 medium 対応)。空 / undefined の route 名は
      // "(unnamed)" として表示され tooltip も同値、ずれを防ぐ。
      const nameText = route.name || '(unnamed)';
      nameSpan.textContent = nameText;
      nameSpan.title = nameText;

      const wpSpan = document.createElement('span');
      wpSpan.className = 'route-item-detail';
      const wpText = (route.waypoints || []).join(' \u2192 ');
      wpSpan.textContent = wpText;
      wpSpan.title = wpText;

      textWrap.appendChild(nameSpan);
      textWrap.appendChild(wpSpan);

      const actions = document.createElement('span');
      actions.className = 'route-item-actions';

      const editBtn = document.createElement('button');
      editBtn.className = 'btn-edit';
      editBtn.textContent = 'Edit';
      editBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.editRoute(idx);
      });
      actions.appendChild(editBtn);

      const copyBtn = document.createElement('button');
      copyBtn.className = 'btn-copy';
      copyBtn.textContent = 'Copy';
      copyBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.copyRoute(idx);
      });
      actions.appendChild(copyBtn);

      const delBtn = document.createElement('button');
      delBtn.className = 'btn-delete';
      delBtn.textContent = 'Del';
      delBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.deleteRoute(idx);
      });
      actions.appendChild(delBtn);

      item.appendChild(cb);
      item.appendChild(swatch);
      item.appendChild(textWrap);
      item.appendChild(actions);
      this.listEl.appendChild(item);
    });
  }

  getRouteColor(idx) {
    const palette = ['#2980b9', '#8e44ad', '#16a085', '#d35400', '#c0392b', '#7f8c8d'];
    if (idx < palette.length) return palette[idx];
    const n = idx - palette.length;
    const hue = (n * 47 + 30) % 360;
    return `hsl(${hue}, 55%, 45%)`;
  }

  /**
   * Open edit form for a route.
   */
  editRoute(index) {
    if (!this._canStartEdit('edit')) return;
    this.editingIndex = index;
    this.selectedIndex = index;
    const route = this.routes[index];
    this.formTitle.textContent = 'Edit Route';
    this.fillForm(route);
    this._resetEditHistory();
    this.showForm();
    this.renderList();
    this._fireEditingChange();
  }

  /**
   * Delete a route.
   */
  deleteRoute(index) {
    if (!this._canStartEdit('delete')) return;
    const name = this.routes[index].name;
    this.routes.splice(index, 1);
    this.visibleRoutes.delete(name);
    this.selectedIndex = -1;
    this.setDirty(true);
    this.hideForm();
    this.renderList();
    if (this.onSelectionChange) this.onSelectionChange(-1);
    if (this.onVisibilityChange) this.onVisibilityChange();
  }

  /**
   * Copy a route (deep clone with "_copy" suffix, open in edit form).
   */
  copyRoute(index) {
    if (!this._canStartEdit('copy')) return;
    const original = this.routes[index];
    const copy = JSON.parse(JSON.stringify(original));
    copy.name = original.name + '_copy';
    this.routes.push(copy);
    const newIdx = this.routes.length - 1;
    this.visibleRoutes.add(copy.name);
    this.editingIndex = newIdx;
    this.selectedIndex = newIdx;
    this.formTitle.textContent = 'Edit Route (Copy)';
    this.fillForm(copy);
    this._resetEditHistory();
    this.showForm();
    this.setDirty(true);
    this.renderList();
    this._fireEditingChange();
  }

  /**
   * Start adding a new route.
   */
  startAddRoute() {
    if (!this._canStartEdit('add')) return;
    this.editingIndex = -2;
    this.formTitle.textContent = 'New Route';
    this.fillForm({ name: '', waypoints: [] });
    this._resetEditHistory();
    this.showForm();
    this.renderList();
    this._fireEditingChange();
  }

  /**
   * Fill the edit form with route data.
   */
  fillForm(route) {
    this.inputRouteName.value = route.name || '';
    this.editingWaypoints = [...(route.waypoints || [])];
    this.selectedWaypointIndex = this.editingWaypoints.length > 0 ? this.editingWaypoints.length - 1 : -1;
    this.editingLandmarks = [...(route.landmarks || [])];   // #143
    this.renderWaypointList();
    this.populateWaypointSelect();
    this.renderLandmarkList();
    this.populateLandmarkSelect();
  }

  /**
   * Populate the waypoint POI dropdown.
   */
  populateWaypointSelect() {
    // POI undo 等で候補が変わった後の再構築 (#334 round5) でも、ユーザーが選択中
    // だった値が新しい候補集合にまだ含まれるなら保持する (#334 round6: innerHTML
    // 入れ替えで無条件に空へ戻ると、選択済みの候補が黙って別物に変わってしまう)。
    const prevValue = this.waypointSelect.value;
    this.waypointSelect.innerHTML = '<option value="">-- Select POI --</option>';
    this.poiNames.forEach((name) => {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      this.waypointSelect.appendChild(opt);
    });
    if (prevValue && this.poiNames.includes(prevValue)) {
      this.waypointSelect.value = prevValue;
    }
  }

  /**
   * Render the ordered waypoint list in the form.
   */
  renderWaypointList() {
    this.waypointListEl.innerHTML = '';
    this._normalizeSelectedWaypointIndex();
    if (this.editingWaypoints.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'wp-empty';
      empty.textContent = 'No waypoints added';
      this.waypointListEl.appendChild(empty);
      return;
    }
    this.editingWaypoints.forEach((wp, i) => {
      const row = document.createElement('div');
      row.className = 'wp-item' + (i === this.selectedWaypointIndex ? ' selected' : '');
      row.tabIndex = 0;
      row.dataset.poiName = wp;
      row.setAttribute('aria-selected', i === this.selectedWaypointIndex ? 'true' : 'false');
      row.addEventListener('click', () => this.selectWaypoint(i));
      row.addEventListener('focus', () => this.selectWaypoint(i));
      row.addEventListener('dragover', (e) => this._onWaypointDragOver(e, row));
      row.addEventListener('dragleave', () => row.classList.remove('wp-drop-target'));
      row.addEventListener('drop', (e) => this._onWaypointDrop(e, i));

      const dragHandle = document.createElement('span');
      dragHandle.className = 'wp-drag-handle';
      dragHandle.textContent = '\u22EE\u22EE';
      dragHandle.title = 'Drag to reorder';
      dragHandle.draggable = true;
      dragHandle.addEventListener('click', (e) => e.stopPropagation());
      dragHandle.addEventListener('dragstart', (e) => this._onWaypointDragStart(e, i));
      dragHandle.addEventListener('dragend', () => this._clearWaypointDropTargets());

      const num = document.createElement('span');
      num.className = 'wp-num';
      num.textContent = (i + 1) + '.';

      const name = document.createElement('span');
      name.className = 'wp-name';
      name.textContent = wp;
      // Warn if POI doesn't exist
      if (this.poiNames.length > 0 && !this.poiNames.includes(wp)) {
        name.classList.add('wp-missing');
        name.title = 'POI not found';
      }

      const btns = document.createElement('span');
      btns.className = 'wp-btns';

      const upBtn = document.createElement('button');
      upBtn.type = 'button';
      upBtn.textContent = '\u25B2';
      upBtn.title = 'Move up';
      upBtn.disabled = i === 0;
      upBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.moveWaypoint(i, -1);
      });

      const downBtn = document.createElement('button');
      downBtn.type = 'button';
      downBtn.textContent = '\u25BC';
      downBtn.title = 'Move down';
      downBtn.disabled = i === this.editingWaypoints.length - 1;
      downBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.moveWaypoint(i, 1);
      });

      const removeBtn = document.createElement('button');
      removeBtn.type = 'button';
      removeBtn.textContent = '\u00D7';
      removeBtn.title = 'Remove';
      removeBtn.className = 'wp-remove';
      removeBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.removeWaypoint(i);
      });

      btns.appendChild(upBtn);
      btns.appendChild(downBtn);
      btns.appendChild(removeBtn);

      row.appendChild(dragHandle);
      row.appendChild(num);
      row.appendChild(name);
      row.appendChild(btns);
      this.waypointListEl.appendChild(row);
    });
  }

  addWaypointFromSelect() {
    const val = this.waypointSelect.value;
    if (!val) return;
    this.insertWaypointAfterSelected(val);
    this.waypointSelect.value = '';
  }

  /**
   * Add a waypoint by POI name (e.g. from map click).
   */
  addWaypointByName(name) {
    this.insertWaypointAfterSelected(name);
  }

  insertWaypointAfterSelected(name) {
    if (this.editingIndex === -1) return;
    this._pushEditUndo();
    const insertIndex = this._waypointInsertIndex();
    this.editingWaypoints.splice(insertIndex, 0, name);
    this.selectedWaypointIndex = insertIndex;
    if (this.waypointSelect) this.waypointSelect.value = name;
    if (this.landmarkSelect) this.landmarkSelect.value = '';
    this.renderWaypointList();
    this.focusWaypointName(name);
    this._fireEditingChange();
  }

  selectPoiCandidate(name) {
    if (!name) return '';
    if (this.poiNames.includes(name)) {
      if (this.waypointSelect) this.waypointSelect.value = name;
      if (this.landmarkSelect) this.landmarkSelect.value = '';
      this.focusWaypointName(name);
      return 'waypoint';
    }
    if (this.landmarkNames.includes(name)) {
      if (this.landmarkSelect) this.landmarkSelect.value = name;
      if (this.waypointSelect) this.waypointSelect.value = '';
      this.focusLandmarkName(name);
      return 'landmark';
    }
    return '';
  }

  moveWaypoint(index, dir) {
    this.moveWaypointTo(index, index + dir);
  }

  moveWaypointTo(index, newIndex) {
    if (index < 0 || index >= this.editingWaypoints.length) return;
    if (newIndex < 0 || newIndex >= this.editingWaypoints.length) return;
    if (index === newIndex) return;
    this._pushEditUndo();
    const [wp] = this.editingWaypoints.splice(index, 1);
    this.editingWaypoints.splice(newIndex, 0, wp);
    this._moveSelectedWaypointIndex(index, newIndex);
    this.renderWaypointList();
    this._fireEditingChange();
  }

  removeWaypoint(index) {
    if (index < 0 || index >= this.editingWaypoints.length) return;
    this._pushEditUndo();
    this.editingWaypoints.splice(index, 1);
    if (this.selectedWaypointIndex > index) {
      this.selectedWaypointIndex -= 1;
    } else if (this.selectedWaypointIndex === index) {
      this.selectedWaypointIndex = Math.min(index, this.editingWaypoints.length - 1);
    }
    this.renderWaypointList();
    this._fireEditingChange();
  }

  // ---- landmarks (#143) ----

  populateLandmarkSelect() {
    if (!this.landmarkSelect) return;
    // populateWaypointSelect と同じ理由で選択中の値を保持する (#334 round6)。
    const prevValue = this.landmarkSelect.value;
    this.landmarkSelect.innerHTML = '<option value="">-- Select landmark POI --</option>';
    this.landmarkNames.forEach((name) => {
      const opt = document.createElement('option');
      opt.value = name;
      opt.textContent = name;
      this.landmarkSelect.appendChild(opt);
    });
    if (prevValue && this.landmarkNames.includes(prevValue)) {
      this.landmarkSelect.value = prevValue;
    }
  }

  renderLandmarkList() {
    if (!this.landmarkListEl) return;
    this.landmarkListEl.innerHTML = '';
    if (this.editingLandmarks.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'wp-empty';
      empty.textContent = 'No landmarks added';
      this.landmarkListEl.appendChild(empty);
      return;
    }
    // landmarks は順序の意味が無いので waypoint と同じ表示でも num は省略しない
    // (waypoint との視覚的一貫性を優先)。
    this.editingLandmarks.forEach((lm, i) => {
      const row = document.createElement('div');
      row.className = 'wp-item';
      row.tabIndex = 0;
      row.dataset.poiName = lm;
      row.addEventListener('click', () => this.focusLandmarkName(lm));
      row.addEventListener('focus', () => this.focusLandmarkName(lm));

      const num = document.createElement('span');
      num.className = 'wp-num';
      num.textContent = (i + 1) + '.';

      const name = document.createElement('span');
      name.className = 'wp-name';
      name.textContent = lm;
      if (this.landmarkNames.length > 0 && !this.landmarkNames.includes(lm)) {
        name.classList.add('wp-missing');
        name.title = 'Landmark POI not found. This name is not linked to an existing landmark POI.';
      }

      const btns = document.createElement('span');
      btns.className = 'wp-btns';

      const removeBtn = document.createElement('button');
      removeBtn.type = 'button';
      removeBtn.textContent = '×';
      removeBtn.title = 'Remove';
      removeBtn.className = 'wp-remove';
      removeBtn.addEventListener('click', () => this.removeLandmark(i));

      btns.appendChild(removeBtn);

      row.appendChild(num);
      row.appendChild(name);
      row.appendChild(btns);
      this.landmarkListEl.appendChild(row);
    });
  }

  addLandmarkFromSelect() {
    if (!this.landmarkSelect) return;
    const val = this.landmarkSelect.value;
    if (!val) return;
    if (this.editingLandmarks.includes(val)) {
      this.landmarkSelect.value = '';
      return;  // 重複追加を防ぐ
    }
    this._pushEditUndo();
    this.editingLandmarks.push(val);
    this.landmarkSelect.value = '';
    this.renderLandmarkList();
    this.focusLandmarkName(val);
    this._fireEditingChange();
  }

  removeLandmark(index) {
    if (index < 0 || index >= this.editingLandmarks.length) return;
    this._pushEditUndo();
    this.editingLandmarks.splice(index, 1);
    this.renderLandmarkList();
    this._fireEditingChange();
  }

  /**
   * Confirm form edits.
   */
  formOk() {
    const name = this.inputRouteName.value.trim();
    if (!name) {
      alert('Route name is required.');
      return;
    }
    // Check uniqueness
    const dupIndex = this.routes.findIndex((r, i) => r.name === name && i !== this.editingIndex);
    if (dupIndex >= 0) {
      alert('Route name "' + name + '" already exists.');
      return;
    }
    if (this.editingWaypoints.length === 0) {
      alert('At least one waypoint is required.');
      return;
    }
    // Warn about missing POIs (non-blocking)
    if (this.poiNames.length > 0) {
      const missing = this.editingWaypoints.filter((wp) => !this.poiNames.includes(wp));
      if (missing.length > 0) {
        const proceed = confirm(
          'Warning: The following waypoints reference nonexistent POIs:\n' +
          missing.join(', ') + '\n\nContinue anyway?');
        if (!proceed) return;
      }
    }

    // #143: route.landmarks は optional。空 array の場合は yaml 出力でも省略しない
    // (frontend 側で常に key を含めて backend に送る)。yaml 上の見栄えは backend に任せる。
    const route = {
      name: name,
      waypoints: [...this.editingWaypoints],
      landmarks: [...this.editingLandmarks],
    };

    if (this.editingIndex === -2) {
      // New route
      this.routes.push(route);
      this.visibleRoutes.add(name);
    } else if (this.editingIndex >= 0) {
      // Update existing - handle name change for visibility
      const oldName = this.routes[this.editingIndex].name;
      if (this.visibleRoutes.has(oldName)) {
        this.visibleRoutes.delete(oldName);
        this.visibleRoutes.add(name);
      }
      this.routes[this.editingIndex] = route;
    }
    this.editingIndex = -1;
    this._resetEditHistory();
    this.setDirty(true);
    this.hideForm();
    this.renderList();
    this._fireEditingChange();
    if (this.onVisibilityChange) this.onVisibilityChange();
  }

  /**
   * Cancel form edits.
   */
  formCancel() {
    this.editingIndex = -1;
    this._resetEditHistory();
    this.hideForm();
    this.renderList();
    this._fireEditingChange();
  }

  showForm() {
    this.formEl.classList.remove('hidden');
    this.mapInsertMode = false;
    if (this.mapInsertToggle) this.mapInsertToggle.checked = false;
    if (this.onEditFormVisibilityChange) this.onEditFormVisibilityChange(true);
  }

  hideForm() {
    this.formEl.classList.add('hidden');
    this.mapInsertMode = false;
    this.selectedWaypointIndex = -1;
    if (this.mapInsertToggle) this.mapInsertToggle.checked = false;
    if (this.onEditFormVisibilityChange) this.onEditFormVisibilityChange(false);
  }

  setDirty(isDirty) {
    this.dirty = isDirty;
    this.btnSaveRoutes.disabled = !isDirty;
    this.btnDiscardRoutes.disabled = !isDirty;
    this.dirtyIndicator.textContent = isDirty ? 'Unsaved changes' : '';
    this._updateUndoButtons();
    if (this.onDirtyChange) this.onDirtyChange(isDirty);
  }

  focusWaypointName(name) {
    if (this.onWaypointFocus) this.onWaypointFocus(name || '');
  }

  selectWaypoint(index) {
    if (index < 0 || index >= this.editingWaypoints.length) return;
    this.selectedWaypointIndex = index;
    this._updateWaypointSelectionClasses();
    this.focusWaypointName(this.editingWaypoints[index]);
  }

  focusLandmarkName(name) {
    if (this.onLandmarkFocus) this.onLandmarkFocus(name || '');
  }

  _onWaypointDragStart(e, index) {
    this.dragWaypointIndex = index;
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', String(index));
    }
  }

  _onWaypointDragOver(e, row) {
    if (e.preventDefault) e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'move';
    row.classList.add('wp-drop-target');
  }

  _onWaypointDrop(e, targetIndex) {
    if (e.preventDefault) e.preventDefault();
    const sourceIndex = this._dragSourceIndexFromEvent(e);
    this._clearWaypointDropTargets();
    this.moveWaypointTo(sourceIndex, targetIndex);
  }

  _dragSourceIndexFromEvent(e) {
    if (this.dragWaypointIndex >= 0) return this.dragWaypointIndex;
    const raw = e.dataTransfer ? e.dataTransfer.getData('text/plain') : '';
    const index = Number.parseInt(raw, 10);
    return Number.isFinite(index) ? index : -1;
  }

  _clearWaypointDropTargets() {
    this.dragWaypointIndex = -1;
    if (!this.waypointListEl) return;
    this.waypointListEl.querySelectorAll('.wp-drop-target').forEach((el) => {
      el.classList.remove('wp-drop-target');
    });
  }

  _normalizeSelectedWaypointIndex() {
    if (!this.editingWaypoints || this.editingWaypoints.length === 0) {
      this.selectedWaypointIndex = -1;
      return;
    }
    if (!Number.isInteger(this.selectedWaypointIndex)
        || this.selectedWaypointIndex < 0
        || this.selectedWaypointIndex >= this.editingWaypoints.length) {
      this.selectedWaypointIndex = this.editingWaypoints.length - 1;
    }
  }

  _updateWaypointSelectionClasses() {
    if (!this.waypointListEl) return;
    this.waypointListEl.querySelectorAll('.wp-item').forEach((row, index) => {
      const selected = index === this.selectedWaypointIndex;
      row.classList.toggle('selected', selected);
      row.setAttribute('aria-selected', selected ? 'true' : 'false');
    });
  }

  _waypointInsertIndex() {
    this._normalizeSelectedWaypointIndex();
    if (this.selectedWaypointIndex < 0) return this.editingWaypoints.length;
    return this.selectedWaypointIndex + 1;
  }

  _moveSelectedWaypointIndex(fromIndex, toIndex) {
    if (this.selectedWaypointIndex === fromIndex) {
      this.selectedWaypointIndex = toIndex;
      return;
    }
    if (fromIndex < toIndex
        && this.selectedWaypointIndex > fromIndex
        && this.selectedWaypointIndex <= toIndex) {
      this.selectedWaypointIndex -= 1;
      return;
    }
    if (fromIndex > toIndex
        && this.selectedWaypointIndex >= toIndex
        && this.selectedWaypointIndex < fromIndex) {
      this.selectedWaypointIndex += 1;
    }
  }

  _resetEditHistory() {
    this.undoStack = [];
    this.redoStack = [];
    this._updateUndoButtons();
  }

  _updateUndoButtons() {
    const active = this.editingIndex !== -1;
    if (this.btnUndo) this.btnUndo.disabled = !active || this.undoStack.length === 0;
    if (this.btnRedo) this.btnRedo.disabled = !active || this.redoStack.length === 0;
  }

  _pushEditUndo() {
    if (this.editingIndex === -1) return;
    MapoiPoiHistory.recordChange(
      this.undoStack,
      this.redoStack,
      this._snapshotEditForm(),
      this.HISTORY_CAP,
    );
    this._updateUndoButtons();
  }

  _snapshotEditForm() {
    return {
      routeName: this.inputRouteName.value,
      editingWaypoints: [...this.editingWaypoints],
      editingLandmarks: [...this.editingLandmarks],
      selectedWaypointIndex: this.selectedWaypointIndex,
    };
  }

  _applyEditSnapshot(snap) {
    this.inputRouteName.value = snap.routeName || '';
    this.editingWaypoints = [...(snap.editingWaypoints || [])];
    this.editingLandmarks = [...(snap.editingLandmarks || [])];
    this.selectedWaypointIndex = Number.isInteger(snap.selectedWaypointIndex)
      ? snap.selectedWaypointIndex
      : -1;
    this.renderWaypointList();
    this.renderLandmarkList();
    this._updateUndoButtons();
    this._fireEditingChange();
  }

  undo() {
    if (this.editingIndex === -1) return;
    const snap = MapoiPoiHistory.stepUndo(
      this.undoStack,
      this.redoStack,
      this._snapshotEditForm(),
    );
    if (!snap) return;
    this._applyEditSnapshot(snap);
  }

  redo() {
    if (this.editingIndex === -1) return;
    const snap = MapoiPoiHistory.stepRedo(
      this.undoStack,
      this.redoStack,
      this._snapshotEditForm(),
    );
    if (!snap) return;
    this._applyEditSnapshot(snap);
  }

  /**
   * Save routes to server.
   */
  async save() {
    if (!this.dirty) return;
    this.btnSaveRoutes.textContent = 'Saving...';
    try {
      const result = await MapoiApi.saveRoutes(this.routes, this.configVersion);
      // 409 (version_mismatch): yaml が外部 (yaml 直編集 / 他クライアント) で変わった
      // (#241 の展開, #343)。poi-editor.js の save() と同じ確認ダイアログ経路。
      if (result.conflict) {
        this.btnSaveRoutes.textContent = 'Save';
        const shouldReload = window.confirm(
          'The YAML file was changed outside this page.'
          + '\nSaving now may overwrite newer changes.'
          + '\n\n[OK] Reload the latest file. Unsaved edits will be lost.'
          + '\n[Cancel] Keep editing. You will see this warning again on Save.'
        );
        if (shouldReload && this.onConflictReload) {
          await this.onConflictReload();
        }
        return;
      }
      if (result.ok && result.success) {
        this.originalRoutes = JSON.parse(JSON.stringify(this.routes));
        if (result.config_version) this.configVersion = result.config_version;
        this.setDirty(false);
        this.btnSaveRoutes.textContent = 'Saved!';
        setTimeout(() => { this.btnSaveRoutes.textContent = 'Save'; }, 1500);
      } else {
        alert('Save failed: ' + (result.error || 'unknown error'));
        this.btnSaveRoutes.textContent = 'Save';
      }
    } catch (e) {
      alert('Save error: ' + e.message);
      this.btnSaveRoutes.textContent = 'Save';
    }
  }

  /**
   * Discard all changes, reload from original.
   */
  discard() {
    if (!this.dirty) return;
    this.routes = JSON.parse(JSON.stringify(this.originalRoutes));
    this.editingIndex = -1;
    this.visibleRoutes = new Set(this.routes.map((r) => r.name));
    this._resetEditHistory();
    this.setDirty(false);
    this.hideForm();
    this.renderList();
    if (this.onVisibilityChange) this.onVisibilityChange();
  }

  setAllVisible() {
    this.visibleRoutes = new Set(this.routes.map((r) => r.name));
    this.renderList();
    if (this.onVisibilityChange) this.onVisibilityChange();
  }

  setAllHidden() {
    this.visibleRoutes = new Set();
    this.renderList();
    if (this.onVisibilityChange) this.onVisibilityChange();
  }

  /**
   * Select a route by index (toggle: re-click deselects).
   */
  selectRoute(index) {
    this.selectedIndex = this.selectedIndex === index ? -1 : index;
    this.renderList();
    if (this.onSelectionChange) this.onSelectionChange(this.selectedIndex);
  }

  clearSelection() {
    if (this.selectedIndex === -1) return;
    this.selectedIndex = -1;
    this.renderList();
    if (this.onSelectionChange) this.onSelectionChange(-1);
  }

  /**
   * Returns the editing route color, or null if not editing an existing route.
   */
  getEditingRouteColor() {
    if (this.editingIndex >= 0) return this.getRouteColor(this.editingIndex);
    if (this.editingIndex === -2) return this.getRouteColor(this.routes.length);
    return null;
  }

  /** @private */
  _fireEditingChange() {
    if (this.onEditingChange) this.onEditingChange(this.editingIndex !== -1);
  }

  _canStartEdit(action) {
    if (!this.onBeforeEditStart) return true;
    return this.onBeforeEditStart(action) !== false;
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = RouteEditor;
}
