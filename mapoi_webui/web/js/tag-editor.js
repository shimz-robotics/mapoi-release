/**
 * Tag definitions editor: custom tag list display, add/delete, dirty tracking, save/discard.
 *
 * app.js の単一 IIFE に同居していたタグエディタ state/DOM 配線を切り出した (#346)。
 * poi-editor.js / route-editor.js と同じ構造 (DOM 配線 + dirty 追跡 + callback 経由で
 * 外部 (app.js) の reload をトリガーする) を踏襲する:
 * - `onReload`: save 成功 / discard 後の tag 定義再取得 (app.js の `loadTagDefinitions()`)
 * - `onConflictReload`: 409 (version_mismatch) 確認後の全体 reload
 *   (`loadTagDefinitions()` + `loadPois()` + `loadRoutes()`、poi-editor.js /
 *   route-editor.js の `onConflictReload` と同じ役割)
 *
 * Browser からは top-level の `TagEditor` グローバルとして使い (app.js が
 * `new TagEditor()`)、Node (vitest) からは `module.exports` 経由で import する
 * (poi-editor.js / route-editor.js と同じクラス export 形式)。
 */
class TagEditor {
  constructor() {
    this.tags = []; // working copy (system + custom), set via setTagDefinitions()
    this.dirty = false;
    // 楽観的競合検出用 yaml ハッシュ (#241 の展開, #343)。poi-editor.js / route-editor.js の
    // configVersion と同じ役割。
    this.configVersion = null;
    this.expandedDescriptions = new Set(); // description 展開 UI state (tag name/index key)

    this.onReload = null;         // async callback() — save 成功 / discard 後の再取得
    this.onConflictReload = null; // async callback() — 409 確認後の全体 reload
    this.onDirtyChange = null;    // callback(isDirty) — 保留中 SSE reload の再開判定 (#373)

    this.listEl = document.getElementById('tag-list');
    this.addNameEl = document.getElementById('tag-add-name');
    this.addDescEl = document.getElementById('tag-add-desc');
    this.btnAdd = document.getElementById('btn-tag-add');
    this.btnSave = document.getElementById('btn-save-tags');
    this.btnDiscard = document.getElementById('btn-discard-tags');
    this.dirtyIndicator = document.getElementById('tag-dirty-indicator');

    this.btnAdd.addEventListener('click', () => this._handleAdd());
    this.btnSave.addEventListener('click', () => this._handleSave());
    this.btnDiscard.addEventListener('click', () => this._handleDiscard());
  }

  /**
   * 最新の tag 定義で working copy を置き換えて再描画する (app.js の loadTagDefinitions() から)。
   */
  setTagDefinitions(tags, configVersion) {
    this.tags = (tags || []).map((t) => ({ ...t }));
    this.configVersion = configVersion || null;
    // 全 reload の単一 funnel (poi-editor.js loadPois / route-editor.js loadRoutes と同じ)。
    // working copy を置き換えたのに dirty (Save 有効) が残る非一貫を防ぐ (#373)。
    this.setDirty(false);
    this.render();
  }

  setDirty(dirty) {
    this.dirty = dirty;
    this.btnSave.disabled = !dirty;
    this.btnDiscard.disabled = !dirty;
    this.dirtyIndicator.textContent = dirty ? 'unsaved changes' : '';
    if (this.onDirtyChange) this.onDirtyChange(dirty);
  }

  render() {
    this.listEl.innerHTML = '';
    this.tags.forEach((tag, i) => {
      const div = document.createElement('div');
      div.className = 'tag-item ' + (tag.is_system ? 'tag-item-system' : 'tag-item-custom');
      const tagKey = tag.name || String(i);
      const description = tag.description || '';
      const isExpanded = this.expandedDescriptions.has(tagKey);
      if (isExpanded) div.classList.add('tag-desc-expanded');

      if (tag.is_system) {
        const icon = document.createElement('span');
        icon.className = 'tag-lock';
        icon.textContent = '\u{1F512}';
        div.appendChild(icon);
      }

      const name = document.createElement('span');
      name.className = 'tag-item-name';
      name.textContent = tag.name;
      name.title = tag.name;
      div.appendChild(name);

      const desc = document.createElement('button');
      desc.type = 'button';
      desc.className = 'tag-item-desc';
      desc.textContent = description;
      desc.title = description;
      desc.disabled = !description;
      desc.setAttribute('aria-label', description ? `Tag description: ${description}` : 'No tag description');
      desc.setAttribute('aria-expanded', isExpanded ? 'true' : 'false');
      if (description) desc.classList.add('has-description');
      if (isExpanded) desc.classList.add('expanded');
      desc.addEventListener('click', (e) => {
        e.stopPropagation();
        if (!description) return;
        if (this.expandedDescriptions.has(tagKey)) {
          this.expandedDescriptions.delete(tagKey);
        } else {
          this.expandedDescriptions.add(tagKey);
        }
        this.render();
      });
      div.appendChild(desc);

      const typeLabel = document.createElement('span');
      typeLabel.className = tag.is_system
        ? 'tag-type-label tag-type-system'
        : 'tag-type-label tag-type-custom';
      typeLabel.textContent = tag.is_system ? 'system' : 'custom';
      div.appendChild(typeLabel);

      const deleteBtn = document.createElement('button');
      deleteBtn.className = 'tag-delete-btn';
      deleteBtn.title = tag.is_system ? 'System tags cannot be deleted' : 'Delete tag';
      deleteBtn.innerHTML = '&#10005;';
      if (tag.is_system) {
        deleteBtn.disabled = true;
      } else {
        deleteBtn.dataset.index = String(i);
      }
      div.appendChild(deleteBtn);
      this.listEl.appendChild(div);
    });
    // Attach delete handlers
    this.listEl.querySelectorAll('.tag-delete-btn:not(:disabled)').forEach((btn) => {
      btn.addEventListener('click', (e) => {
        const idx = parseInt(e.currentTarget.dataset.index);
        this.tags.splice(idx, 1);
        this.setDirty(true);
        this.render();
      });
    });
  }

  _handleAdd() {
    const name = this.addNameEl.value.trim();
    const desc = this.addDescEl.value.trim();
    if (!name) { this.addNameEl.focus(); return; }
    // Check duplicate (case-insensitive)
    if (this.tags.some((t) => t.name.toLowerCase() === name.toLowerCase())) {
      alert('Tag "' + name + '" already exists.');
      return;
    }
    this.tags.push({ name, description: desc, is_system: false });
    this.addNameEl.value = '';
    this.addDescEl.value = '';
    this.setDirty(true);
    this.render();
  }

  /**
   * Ctrl+S (#375) 用の公開 entry。Save ボタンと同じ経路 (_handleSave) で、
   * poi-editor.js / route-editor.js の save() と同じく dirty でなければ no-op。
   */
  async save() {
    if (!this.dirty) return;
    await this._handleSave();
  }

  async _handleSave() {
    const customTags = this.tags
      .filter((t) => !t.is_system)
      .map((t) => ({ name: t.name, description: t.description }));
    const result = await MapoiApi.saveCustomTags(customTags, this.configVersion);
    // 409 (version_mismatch): yaml が外部で変わった (#241 の展開, #343)。
    // poi-editor.js / route-editor.js の save() と同じ確認ダイアログ経路。
    if (result.conflict) {
      const shouldReload = window.confirm(
        'The YAML file was changed outside this page.'
        + '\nSaving now may overwrite newer changes.'
        + '\n\n[OK] Reload the latest file. Unsaved edits will be lost.'
        + '\n[Cancel] Keep editing. You will see this warning again on Save.'
      );
      if (shouldReload) {
        this.setDirty(false);
        if (this.onConflictReload) await this.onConflictReload();
      }
      return;
    }
    if (result.error) {
      alert('Failed to save tags: ' + result.error);
      return;
    }
    this.setDirty(false);
    // onReload (= loadTagDefinitions()) が最新 config_version も取り直すため、
    // configVersion をここで個別更新する必要は無い (#343)。
    if (this.onReload) await this.onReload();
  }

  async _handleDiscard() {
    if (this.dirty && !confirm('Discard unsaved tag changes?')) return;
    this.setDirty(false);
    if (this.onReload) await this.onReload();
  }
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = TagEditor;
}
