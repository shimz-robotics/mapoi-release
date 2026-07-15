/**
 * POI editor: list display, edit form, dirty state tracking.
 */
class PoiEditor {
  constructor() {
    this.pois = [];          // current POI list (working copy)
    this.originalPois = [];  // snapshot for dirty detection
    this.dirty = false;
    this.selectedIndex = -1;
    this.editingIndex = -1;  // -1 = closed, >=0 = editing existing, -2 = new POI
    this.placingMode = false;
    this.visiblePois = new Set(); // indices of visible POIs
    // 楽観的競合検出用 yaml ハッシュ (#241)。GET 時に backend から受け取り、Save 時に送り返す。
    this.configVersion = null;

    // Undo/Redo: capture-before-mutate スナップショットの stack (#300)。
    // 変異メソッド先頭で _pushUndo() し、redo は新編集で無効化される (poi-history.js)。
    // undo/redo 後の dirty は履歴深さではなく、保存済み POI との内容比較で導出する。
    this.undoStack = [];
    this.redoStack = [];
    this.HISTORY_CAP = 50;

    this.tagDefinitions = [];  // [{name, description, is_system}, ...]
    this.onDirtyChange = null;     // callback(isDirty)
    this.onSelectionChange = null; // callback(index)
    this.onPlacingModeChange = null; // callback(isPlacing)
    this.onVisibilityChange = null;  // callback(visibleSet)
    this.onBeforeEditStart = null; // callback(action) -> false blocks POI edit/add/copy/delete
    this.onEditCancel = null; // callback(previousEditingIndex) after discarding form-only edits
    // 編集フォームの開閉 (#240)。開いたら app.js 側で他セクションを畳んでフォームに集中
    // させ、閉じたら元の表示状態へ戻す。showForm/hideForm で必ず発火する。
    this.onEditFormVisibilityChange = null; // callback(isOpen)
    // 409 受信時に app.js 側で全体 reload (loadMaps → loadPois/Routes) を発火させるためのフック。
    // poi-editor 単独では route-editor の state を巻き戻せないため、解決は呼び出し側に委ねる (#241)。
    this.onConflictReload = null;  // async callback() — full reload on version_mismatch
    // undo スタックの有無が変わるたびに発火 (#332)。モバイルでパネルが畳まれていても
    // 押せるフローティング Undo ボタンの表示/非表示を app.js 側で切り替えるためのフック。
    this.onHistoryChange = null;  // callback(canUndo)

    // DOM references
    this.listEl = document.getElementById('poi-list');
    this.formEl = document.getElementById('poi-edit-form');
    this.formTitle = document.getElementById('form-title');
    this.btnSave = document.getElementById('btn-save');
    this.btnDiscard = document.getElementById('btn-discard');
    this.btnAddPoi = document.getElementById('btn-add-poi');
    this.dirtyIndicator = document.getElementById('dirty-indicator');
    this.btnUndo = document.getElementById('btn-undo');
    this.btnRedo = document.getElementById('btn-redo');
    // POI list の名前絞り込み (#383)。query の真実は入力欄の DOM value 一本
    // (_filterQuery で読む) で、editor 側に鏡写しの state を持たない — 二重管理は
    // プログラム的な value 変更で不整合の温床になる。renderList が card 生成を skip
    // する判定 (matchesPoiName) にだけ使い、map marker の可視状態 (visiblePois) とは
    // 独立。入力欄は DOM harness 等で無くてもインスタンス化できるよう null guard
    // (#300 と同流儀)。type="search" のクリアボタン (×) も input event を発火するので
    // この配線だけで同期する。
    this.inputSearch = document.getElementById('poi-search');
    if (this.inputSearch) {
      this.inputSearch.addEventListener('input', () => this.renderList());
      // Escape は入力があればクリア (選択解除 #309 には流さない)。空なら素通しさせ、
      // app.js 側の isEditableTarget guard で無視される (= 何も起きない)。
      this.inputSearch.addEventListener('keydown', (e) => {
        if (e.key !== 'Escape' || e.isComposing || !this.inputSearch.value) return;
        e.preventDefault();
        e.stopPropagation();
        this.inputSearch.value = '';
        this.renderList();
      });
    }

    // Form inputs
    this.inputName = document.getElementById('poi-name');
    this.inputX = document.getElementById('poi-x');
    this.inputY = document.getElementById('poi-y');
    this.inputYaw = document.getElementById('poi-yaw');
    this.inputToleranceXy = document.getElementById('poi-tolerance-xy');
    this.inputToleranceYaw = document.getElementById('poi-tolerance-yaw');
    this.inputTags = document.getElementById('poi-tags');
    this.inputDescription = document.getElementById('poi-description');

    // Bind button events
    document.getElementById('btn-form-ok').addEventListener('click', () => this.formOk());
    document.getElementById('btn-form-cancel').addEventListener('click', () => this.formCancel());
    this.btnAddPoi.addEventListener('click', () => this.startAddPoi());
    this.btnSave.addEventListener('click', () => this.save());
    this.btnDiscard.addEventListener('click', () => this.discard());
    // Undo/Redo ボタンは無くても (DOM harness 等) インスタンス化できるよう null guard (#300)。
    if (this.btnUndo) this.btnUndo.addEventListener('click', () => this.undo());
    if (this.btnRedo) this.btnRedo.addEventListener('click', () => this.redo());
  }

  /**
   * Load POIs from server response and reset state.
   */
  loadPois(pois, configVersion) {
    this.pois = JSON.parse(JSON.stringify(pois));
    this.originalPois = JSON.parse(JSON.stringify(pois));
    this.selectedIndex = -1;
    this.editingIndex = -1;
    this.visiblePois = new Set(pois.map((_, i) => i));
    // 楽観的競合検出 token (#241)。後続の save() で backend に送り返す。
    this.configVersion = configVersion || null;
    // loadPois は全 reload (初期 load / map 切替 / 409 onConflictReload / SSE config_changed) の
    // 単一 funnel。ここで履歴を捨て、別 map や conflict 前バージョンの POI を undo で
    // 復元させない (#300 安全要件)。
    this.undoStack = [];
    this.redoStack = [];
    this.setDirty(false);
    this.hideForm();
    this.renderList();
  }

  /**
   * Render the POI card list.
   */
  /**
   * 検索ボックスの現在 query (#383)。真実は DOM value のみ (入力欄が無い harness では
   * 常に '' = 絞り込みなし)。
   */
  _filterQuery() {
    return this.inputSearch ? this.inputSearch.value : '';
  }

  renderList() {
    this.listEl.innerHTML = '';
    const query = this._filterQuery();
    this.pois.forEach((poi, i) => {
      // 検索絞り込み (#383): 名前部分一致 (大文字小文字不区別)。card 生成を skip する
      // だけで forEach の実 index (i) は保たれるので、selectPoi / deletePoi / visibility
      // checkbox の index 整合はずれない。選択中 POI が query に一致しない場合も選択は
      // 解除しない (card は隠れるが map の highlight / nav dropdown は選択を保持する —
      // 絞り込みは list の見た目だけ、が一貫規則)。
      if (!MapoiPoiFilter.matchesPoiName(poi, query)) return;
      const card = document.createElement('div');
      card.className = 'poi-card' + (i === this.selectedIndex ? ' selected' : '');
      card.dataset.index = i;

      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.className = 'poi-card-checkbox';
      cb.checked = this.visiblePois.has(i);
      cb.addEventListener('click', (e) => e.stopPropagation());
      cb.addEventListener('change', () => {
        if (cb.checked) {
          this.visiblePois.add(i);
        } else {
          this.visiblePois.delete(i);
        }
        if (this.onVisibilityChange) {
          this.onVisibilityChange(this.visiblePois);
        }
      });
      card.appendChild(cb);

      const info = document.createElement('div');
      info.className = 'poi-card-info';

      const name = document.createElement('div');
      name.className = 'poi-card-name';
      const nameText = poi.name || '(unnamed)';
      name.textContent = nameText;
      // ellipsis 切り捨てに備え hover tooltip で full text 確認可能に (#122 PR-3)。
      name.title = nameText;
      info.appendChild(name);

      const detail = document.createElement('div');
      detail.className = 'poi-card-detail';
      const pose = poi.pose || {};
      const tags = (poi.tags || []).join(', ');
      const desc = poi.description || '';
      const detailText = `(${(pose.x||0).toFixed(2)}, ${(pose.y||0).toFixed(2)}) yaw=${(pose.yaw||0).toFixed(2)} ${tags} ${desc}`;
      detail.textContent = detailText;
      detail.title = detailText;
      info.appendChild(detail);

      card.appendChild(info);

      const actions = document.createElement('div');
      actions.className = 'poi-card-actions';

      const editBtn = document.createElement('button');
      editBtn.className = 'btn-edit';
      editBtn.textContent = 'Edit';
      editBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.editPoi(i);
      });
      actions.appendChild(editBtn);

      const copyBtn = document.createElement('button');
      copyBtn.className = 'btn-copy';
      copyBtn.textContent = 'Copy';
      copyBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.copyPoi(i);
      });
      actions.appendChild(copyBtn);

      const delBtn = document.createElement('button');
      delBtn.className = 'btn-delete';
      delBtn.textContent = 'Del';
      delBtn.title = 'Delete POI (Delete)';
      delBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        this.deletePoi(i);
      });
      actions.appendChild(delBtn);

      card.appendChild(actions);

      card.addEventListener('click', () => this.selectPoi(i));
      this.listEl.appendChild(card);
    });
  }

  /**
   * Select a POI by index (highlight in list and on map).
   */
  selectPoi(index) {
    this.selectedIndex = index;
    this.renderList();
    if (this.onSelectionChange) {
      this.onSelectionChange(index);
    }
  }

  clearSelection() {
    if (this.selectedIndex === -1) return;
    this.selectedIndex = -1;
    this.renderList();
    if (this.onSelectionChange) {
      this.onSelectionChange(-1);
    }
  }

  /**
   * Open edit form for a POI.
   */
  editPoi(index) {
    if (!this._canStartEdit('edit')) return;
    this.editingIndex = index;
    const poi = this.pois[index];
    this.formTitle.textContent = 'Edit POI';
    this.fillForm(poi);
    this.showForm();
    this.selectPoi(index);
  }

  /**
   * Delete a POI.
   */
  deletePoi(index) {
    if (!this._canStartEdit('delete')) return;
    this._pushUndo();
    this.pois.splice(index, 1);
    if (this.selectedIndex === index) this.selectedIndex = -1;
    if (this.selectedIndex > index) this.selectedIndex--;
    // Rebuild visiblePois with shifted indices
    const newVisible = new Set();
    this.visiblePois.forEach((vi) => {
      if (vi < index) newVisible.add(vi);
      else if (vi > index) newVisible.add(vi - 1);
    });
    this.visiblePois = newVisible;
    this.setDirty(true);
    this.hideForm();
    this.renderList();
    if (this.onSelectionChange) {
      this.onSelectionChange(this.selectedIndex);
    }
  }

  /**
   * Copy a POI (deep clone with "_copy" suffix, open in edit form).
   */
  copyPoi(index) {
    if (!this._canStartEdit('copy')) return;
    this._pushUndo();
    const original = this.pois[index];
    const copy = JSON.parse(JSON.stringify(original));
    copy.name = original.name + '_copy';
    this.pois.push(copy);
    const newIdx = this.pois.length - 1;
    this.visiblePois.add(newIdx);
    this.editingIndex = newIdx;
    this.formTitle.textContent = 'Edit POI (Copy)';
    this.fillForm(copy);
    this.showForm();
    this.setDirty(true);
    this.selectPoi(newIdx);
  }

  /**
   * Start adding a new POI — enter placing mode.
   */
  startAddPoi() {
    if (this.placingMode) {
      this.cancelPlacing();
      return;
    }
    if (!this._canStartEdit('add')) return;
    this.placingMode = true;
    this.btnAddPoi.textContent = 'Click map...';
    this.btnAddPoi.classList.add('placing');
    if (this.onPlacingModeChange) {
      this.onPlacingModeChange(true);
    }
  }

  cancelPlacing() {
    this.placingMode = false;
    this.btnAddPoi.textContent = '+ Add POI';
    this.btnAddPoi.classList.remove('placing');
    if (this.onPlacingModeChange) {
      this.onPlacingModeChange(false);
    }
  }

  /**
   * Called when map is clicked in placing mode.
   */
  placeNewPoi(x, y) {
    if (!this.placingMode) return;
    this.cancelPlacing();

    const newPoi = {
      name: `poi_${this.pois.length}`,
      pose: {
        x: MapoiPoiFilter.roundTo(x, MapoiPoiFilter.POSE_XY_DIGITS),
        y: MapoiPoiFilter.roundTo(y, MapoiPoiFilter.POSE_XY_DIGITS),
        yaw: 0.0,
      },
      // default tolerance: xy=0.5 m / yaw=π/4 (45°) — sample yaml と整合 (#138)
      tolerance: {
        xy: 0.5,
        yaw: MapoiPoiFilter.roundTo(Math.PI / 4, MapoiPoiFilter.TOLERANCE_YAW_DIGITS),
      },
      tags: ['waypoint'],
      description: '',
    };

    this.editingIndex = -2; // new
    this.formTitle.textContent = 'New POI';
    this.fillForm(newPoi);
    this.showForm();
  }

  /**
   * Set available tag definitions for tag helper UI.
   */
  setTagDefinitions(tags) {
    this.tagDefinitions = tags || [];
  }

  /**
   * Render tag chips below the tags input.
   */
  renderTagChips() {
    let container = document.getElementById('tag-chips');
    if (!container) {
      container = document.createElement('div');
      container.id = 'tag-chips';
      container.className = 'tag-chips';
      this.inputTags.parentElement.appendChild(container);
    }
    container.innerHTML = '';

    if (this.tagDefinitions.length === 0) return;

    const currentTags = this.inputTags.value.split(',').map(t => t.trim()).filter(t => t);

    this.tagDefinitions.forEach((td) => {
      const chip = document.createElement('span');
      const isActive = currentTags.includes(td.name);
      chip.className = 'tag-chip' +
        (isActive ? ' active' : '') +
        (td.is_system ? ' system' : ' user');
      chip.textContent = td.name;
      chip.title = td.description + (td.is_system ? ' (system)' : '');
      chip.addEventListener('click', () => {
        this.toggleTag(td.name);
      });
      container.appendChild(chip);
    });
  }

  /**
   * Toggle a tag in the tags input field.
   */
  toggleTag(tagName) {
    let tags = this.inputTags.value.split(',').map(t => t.trim()).filter(t => t);
    const idx = tags.indexOf(tagName);
    if (idx >= 0) {
      tags.splice(idx, 1);
    } else {
      tags.push(tagName);
    }
    this.inputTags.value = tags.join(', ');
    this.renderTagChips();
  }

  /**
   * Fill the edit form with POI data.
   */
  fillForm(poi) {
    this.inputName.value = poi.name || '';
    const pose = poi.pose || {};
    // 入力欄表示時点で yaml の有効桁数に丸める (#242)。display と Save 値の不一致を防ぐ。
    const xRounded = MapoiPoiFilter.roundTo(pose.x, MapoiPoiFilter.POSE_XY_DIGITS);
    const yRounded = MapoiPoiFilter.roundTo(pose.y, MapoiPoiFilter.POSE_XY_DIGITS);
    const yawRounded = MapoiPoiFilter.roundTo(pose.yaw, MapoiPoiFilter.POSE_YAW_DIGITS);
    this.inputX.value = Number.isFinite(xRounded) ? xRounded : 0;
    this.inputY.value = Number.isFinite(yRounded) ? yRounded : 0;
    this.inputYaw.value = Number.isFinite(yawRounded) ? yawRounded : 0;
    // tolerance: xy は m そのまま、yaw は rad → deg 変換して UI に表示 (#138)。
    // `||` だと意図的な小さい値も default で上書きされるので Number.isFinite ベースで判定。
    // yaw は formatToleranceYawDeg で 2 桁丸め + 末尾ゼロ除去し、30/45/90/180 等の区切りの
    // 良い度数を綺麗に表示する (#267)。区切りの良い度数は rad 4 桁保存と完全に round-trip し
    // 未編集 save で churn しない (旧 toFixed(4) は #139 で churn 回避のため採用したが
    // "180.0002" 等と汚かった)。非 round 値はごく稀に ≤0.0001 rad ≒ 0.006° drift しうるが
    // yaml 4 桁粒度内で実害なし。
    const xyVal = poi.tolerance && poi.tolerance.xy;
    this.inputToleranceXy.value = Number.isFinite(xyVal)
      ? MapoiPoiFilter.roundTo(xyVal, MapoiPoiFilter.TOLERANCE_XY_DIGITS)
      : 0.5;
    const yawValRad = poi.tolerance && poi.tolerance.yaw;
    const yawDeg = MapoiPoiFilter.formatToleranceYawDeg(yawValRad);
    this.inputToleranceYaw.value = Number.isFinite(yawDeg) ? yawDeg : 45;
    this.inputTags.value = (poi.tags || []).join(', ');
    this.inputDescription.value = poi.description || '';
    this.renderTagChips();
  }

  /**
   * Read POI data from the form.
   */
  readForm() {
    // pose / tolerance を yaml の有効桁数に丸めて Save する (#242)。
    // 丸めないと parseFloat の浮動小数点誤差 (例: 0.1 → 0.10000038...) や
    // deg ↔ rad 変換の累積誤差で yaml 値が膨らみ、git diff / 可読性が劣化する。
    // 桁数定数は poi-filter.js に集約し、backend (yaml_handler.save_pois) と一致させる。
    const xRaw = MapoiPoiFilter.roundTo(this.inputX.value, MapoiPoiFilter.POSE_XY_DIGITS);
    const yRaw = MapoiPoiFilter.roundTo(this.inputY.value, MapoiPoiFilter.POSE_XY_DIGITS);
    const yawRaw = MapoiPoiFilter.roundTo(this.inputYaw.value, MapoiPoiFilter.POSE_YAW_DIGITS);
    // tolerance: xy は m そのまま、yaw は UI deg → rad 変換して dict / yaml に保存 (#138)。
    // parseTolerance が finite 判定 + min 0.001 強制 (HTML min を bypass する経路の防御) を担う。
    const tol = MapoiPoiFilter.parseTolerance(
      this.inputToleranceXy.value,
      MapoiPoiFilter.degToRad(this.inputToleranceYaw.value),
    );
    return {
      name: this.inputName.value.trim(),
      pose: {
        x: Number.isFinite(xRaw) ? xRaw : 0,
        y: Number.isFinite(yRaw) ? yRaw : 0,
        yaw: Number.isFinite(yawRaw) ? yawRaw : 0,
      },
      tolerance: {
        xy: MapoiPoiFilter.roundTo(tol.xy, MapoiPoiFilter.TOLERANCE_XY_DIGITS),
        yaw: MapoiPoiFilter.roundTo(tol.yaw, MapoiPoiFilter.TOLERANCE_YAW_DIGITS),
      },
      tags: this.inputTags.value.split(',').map(t => t.trim()).filter(t => t),
      description: this.inputDescription.value.trim(),
    };
  }

  /**
   * Confirm form edits.
   */
  formOk() {
    const poi = this.readForm();
    // name required + uniqueness check (#109)。route-editor の formOk と同 pattern。
    // editingIndex === -2 (new POI) は全 POI と比較、editingIndex >= 0 (edit) は
    // 自分以外と比較する。trim 済 (readForm 内) を前提、case-sensitive 判定。
    if (!poi.name) {
      alert('POI name is required.');
      return;
    }
    const dupIndex = this.pois.findIndex(
      (p, i) => p.name === poi.name && i !== this.editingIndex,
    );
    if (dupIndex >= 0) {
      alert('POI name "' + poi.name + '" already exists.');
      return;
    }
    // landmark 排他 check (#85): goal+landmark / initial_pose+landmark は不可。
    const tagValidation = MapoiPoiFilter.validatePoiTags(poi);
    if (!tagValidation.ok) {
      alert(tagValidation.error);
      return;
    }
    // validation を全て通過し実際に変異する直前で履歴に積む。alert で早期 return した
    // 経路 (変異なし) では積まない (#300)。
    this._pushUndo();
    if (this.editingIndex === -2) {
      // New POI
      this.pois.push(poi);
      this.selectedIndex = this.pois.length - 1;
      this.visiblePois.add(this.selectedIndex);
    } else if (this.editingIndex >= 0) {
      // Merge: keep unknown fields from original
      this.pois[this.editingIndex] = { ...this.pois[this.editingIndex], ...poi };
    }
    this.editingIndex = -1;
    this.setDirty(true);
    this.hideForm();
    this.renderList();
    if (this.onSelectionChange) {
      this.onSelectionChange(this.selectedIndex);
    }
  }

  /**
   * Cancel form edits.
   */
  formCancel() {
    const previousEditingIndex = this.editingIndex;
    this.editingIndex = -1;
    this.hideForm();
    if (this.onSelectionChange) {
      this.onSelectionChange(this.selectedIndex);
    }
    if (this.onEditCancel) {
      this.onEditCancel(previousEditingIndex);
    }
  }

  showForm() {
    this.formEl.classList.remove('hidden');
    // 編集フォーム中は app.js の sidebar focus (#240) が #poi-list を隠すので、
    // list 専用の検索ボックス (#383) も一緒に隠して form に集中させる。
    if (this.inputSearch) this.inputSearch.classList.add('hidden');
    if (this.onEditFormVisibilityChange) this.onEditFormVisibilityChange(true);
  }

  hideForm() {
    this.formEl.classList.add('hidden');
    if (this.inputSearch) this.inputSearch.classList.remove('hidden');
    if (this.onEditFormVisibilityChange) this.onEditFormVisibilityChange(false);
  }

  /**
   * 編集フォームを閉じて編集モードを抜ける (#240)。formCancel と違い onSelectionChange は
   * 呼ばない — 呼び出し側 (app.js) が直後に selectPoi で選択遷移を制御する前提。フォームの
   * 未確定入力は破棄される (Cancel 相当)。
   */
  exitEditMode() {
    if (this.editingIndex === -1) return;
    const previousEditingIndex = this.editingIndex;
    this.editingIndex = -1;
    this.hideForm();
    if (this.onEditCancel) {
      this.onEditCancel(previousEditingIndex);
    }
  }

  /**
   * Drag で移動した POI の position を working copy に反映 (#239)。yaw / tolerance / tags は
   * 変更しない。値は yaml 桁数に丸めて dirty にする (確定は Save ボタン — pose tool / #241 と一貫)。
   * 当該 POI を編集フォームで開いていれば X/Y 入力欄も同期する。
   */
  updateDraggedPosition(index, x, y) {
    const poi = this.pois[index];
    if (!poi || !poi.pose) return;
    this._pushUndo();
    poi.pose.x = MapoiPoiFilter.roundTo(x, MapoiPoiFilter.POSE_XY_DIGITS);
    poi.pose.y = MapoiPoiFilter.roundTo(y, MapoiPoiFilter.POSE_XY_DIGITS);
    if (this.editingIndex === index) {
      this.inputX.value = poi.pose.x;
      this.inputY.value = poi.pose.y;
    }
    this.setDirty(true);
    this.renderList();
  }

  /**
   * 扇形ハンドルで回転した POI の yaw を working copy に反映 (#275)。position / tolerance /
   * tags は不変。値は yaml 桁数 (POSE_YAW_DIGITS) に丸めて dirty にする (確定は Save —
   * pose tool / #241 と一貫)。当該 POI を編集フォームで開いていれば Yaw 入力欄も同期。
   */
  updateDraggedYaw(index, yaw) {
    const poi = this.pois[index];
    if (!poi || !poi.pose) return;
    this._pushUndo();
    poi.pose.yaw = MapoiPoiFilter.roundTo(yaw, MapoiPoiFilter.POSE_YAW_DIGITS);
    if (this.editingIndex === index) {
      this.inputYaw.value = poi.pose.yaw;
    }
    this.setDirty(true);
    this.renderList();
  }

  /**
   * Update form X/Y from map click (when editing).
   */
  updateFormPosition(x, y) {
    if (this.editingIndex === -1) return;
    this.inputX.value = MapoiPoiFilter.roundTo(x, MapoiPoiFilter.POSE_XY_DIGITS);
    this.inputY.value = MapoiPoiFilter.roundTo(y, MapoiPoiFilter.POSE_XY_DIGITS);
  }

  /**
   * Update form Yaw from map click direction.
   */
  updateFormYaw(yaw) {
    if (this.editingIndex === -1) return;
    this.inputYaw.value = MapoiPoiFilter.roundTo(yaw, MapoiPoiFilter.POSE_YAW_DIGITS);
  }

  setDirty(isDirty) {
    this.dirty = isDirty;
    this.btnSave.disabled = !isDirty;
    this.btnDiscard.disabled = !isDirty;
    this.dirtyIndicator.textContent = isDirty ? 'Unsaved changes' : '';
    this._updateUndoButtons();
    if (this.onDirtyChange) {
      this.onDirtyChange(isDirty);
    }
  }

  /**
   * Undo/Redo ボタンの有効/無効を stack 長に同期する (#300)。setDirty が全変異 /
   * undo / redo / load / save / discard で必ず呼ばれるので、ここを単一同期点にする。
   */
  _updateUndoButtons() {
    if (this.btnUndo) this.btnUndo.disabled = this.undoStack.length === 0;
    if (this.btnRedo) this.btnRedo.disabled = this.redoStack.length === 0;
    if (this.onHistoryChange) this.onHistoryChange(this.undoStack.length > 0);
  }

  /**
   * 変異直前に現在状態の snapshot を undo stack へ積む (#300)。redo の無効化は
   * poi-history.recordChange 内で行われる。各変異メソッドの「変異が確定する直前」で呼ぶ。
   */
  _pushUndo() {
    MapoiPoiHistory.recordChange(
      this.undoStack,
      this.redoStack,
      this._snapshot(),
      this.HISTORY_CAP,
    );
  }

  /**
   * undo/redo で積む・復元する state の deep clone。pois 本体に加え、delete の index
   * shift を replay 無しで巻き戻せるよう selectedIndex / visiblePois も含める (#300)。
   */
  _snapshot() {
    return {
      pois: JSON.parse(JSON.stringify(this.pois)),
      selectedIndex: this.selectedIndex,
      visiblePois: Array.from(this.visiblePois),
    };
  }

  _isPoiContentDirty() {
    return JSON.stringify(this.pois) !== JSON.stringify(this.originalPois);
  }

  /**
   * snapshot を working copy へ復元し、map / list を再描画する (#300)。
   * 開いている編集フォームは閉じる (stale な入力値が残るのを避ける MVP 方針)。
   * dirty は保存済み POI との内容差で導出し、history cap で stack 深さが変化しても
   * Save ボタン状態が実データとずれないようにする。onSelectionChange / onVisibilityChange
   * を発火して app.js 側に Leaflet marker の再描画 (showPois / highlightPoi) を委ねる。
   */
  _applySnapshot(snap) {
    this.pois = JSON.parse(JSON.stringify(snap.pois));
    this.selectedIndex = snap.selectedIndex;
    this.visiblePois = new Set(snap.visiblePois);
    this.editingIndex = -1;
    this.hideForm();
    this.renderList();
    this.setDirty(this._isPoiContentDirty());
    if (this.onSelectionChange) this.onSelectionChange(this.selectedIndex);
    if (this.onVisibilityChange) this.onVisibilityChange(this.visiblePois);
  }

  /**
   * 1 手戻す。戻せる履歴が無ければ no-op (#300)。
   */
  undo() {
    const snap = MapoiPoiHistory.stepUndo(this.undoStack, this.redoStack, this._snapshot());
    if (!snap) return;
    this._applySnapshot(snap);
  }

  /**
   * 1 手やり直す。やり直せる履歴が無ければ no-op (#300)。
   */
  redo() {
    const snap = MapoiPoiHistory.stepRedo(this.undoStack, this.redoStack, this._snapshot());
    if (!snap) return;
    this._applySnapshot(snap);
  }

  /**
   * Save POIs to server.
   */
  async save() {
    if (!this.dirty) return;
    this.btnSave.textContent = 'Saving...';
    try {
      const result = await MapoiApi.savePois(this.pois, this.configVersion);
      // 409 (version_mismatch): yaml が外部 (yaml 直編集 / 他クライアント) で変わった (#241)。
      // 黙って上書きせず、user に reload するかどうか確認する。
      // reload を選んだら未保存の dirty 編集は失われる旨を明示。
      if (result.conflict) {
        this.btnSave.textContent = 'Save';
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
        this.originalPois = JSON.parse(JSON.stringify(this.pois));
        // save 後も undo 履歴は残す。undo/redo 後の dirty 判定は originalPois との
        // 内容比較で行うため、履歴上限で古い stack entry が drop されても clean/dirty がずれない。
        // backend が再計算した最新 version を frontend に反映 (#241)。
        if (result.config_version) this.configVersion = result.config_version;
        this.setDirty(false);
        this.btnSave.textContent = 'Saved!';
        setTimeout(() => { this.btnSave.textContent = 'Save'; }, 1500);
      } else {
        alert('Save failed: ' + (result.error || 'unknown error'));
        this.btnSave.textContent = 'Save';
      }
    } catch (e) {
      alert('Save error: ' + e.message);
      this.btnSave.textContent = 'Save';
    }
  }

  /**
   * Discard all changes, reload from original.
   */
  discard() {
    if (!this.dirty) return;
    this.pois = JSON.parse(JSON.stringify(this.originalPois));
    this.selectedIndex = -1;
    this.editingIndex = -1;
    // クリーン基準へ戻すので履歴も捨てる (discard を跨いで undo させない) (#300)。
    this.undoStack = [];
    this.redoStack = [];
    this.setDirty(false);
    this.hideForm();
    this.renderList();
    if (this.onSelectionChange) {
      this.onSelectionChange(-1);
    }
  }

  /**
   * Show all POIs on the map.
   */
  setAllVisible() {
    this.visiblePois = new Set(this.pois.map((_, i) => i));
    this.renderList();
    if (this.onVisibilityChange) this.onVisibilityChange(this.visiblePois);
  }

  /**
   * Hide all POIs from the map.
   */
  setAllHidden() {
    this.visiblePois = new Set();
    this.renderList();
    if (this.onVisibilityChange) this.onVisibilityChange(this.visiblePois);
  }

  _canStartEdit(action) {
    if (!this.onBeforeEditStart) return true;
    return this.onBeforeEditStart(action) !== false;
  }
}

// Browser からは top-level の `PoiEditor` グローバルとして使い (app.js が `new PoiEditor()`)、
// Node (vitest) からは `module.exports` 経由で import して jsdom 上で実体テストする (#273)。
// ドラッグ反映 (updateDraggedPosition / updateDraggedYaw) や編集フォーム開閉
// (showForm / hideForm / exitEditMode) は DOM / インスタンス依存で pure-helper に切り出せないため、
// クラスごと export して最小 DOM harness 上で検証する。browser では module 未定義で no-op。
if (typeof module !== 'undefined' && module.exports) {
  module.exports = PoiEditor;
}
