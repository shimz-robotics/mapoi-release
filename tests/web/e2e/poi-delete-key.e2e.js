// 選択中 POI の Delete キー削除 (#374) の browser smoke。
// document keydown IIFE (app.js) は jsdom で実体テストしない方針 (#300 risk-based) のため、
// Escape (#302 map-edit-polish) / Ctrl+Z (#332 poi-drag) と同じく e2e 層で配線を pin する。
const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, selectPoi, setSectionOpen } = require('./helpers');

function routeItem(page, name) {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return page.locator('.route-item').filter({
    has: page.locator('.route-item-name', { hasText: new RegExp(`^${escaped}$`) }),
  });
}

test.describe('POI delete key (#374)', () => {
  test('Delete removes the selected POI with an undo toast, Ctrl+Z restores it', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');

    await page.keyboard.press('Delete');

    // working copy から消え dirty 化する (確定は Save — 削除ボタンと同じ経路)。
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(0);
    await expect(page.locator('.poi-arrow-icon')).toHaveCount(4);
    await expect(page.locator('#btn-save')).toBeEnabled();
    // confirm の代わりに undo 案内つき toast (#374 の選択肢のうち confirm 無し案)。
    await expect(page.locator('.toast')).toContainText('Deleted POI "poi_wedge" (Ctrl+Z to undo)');

    await page.keyboard.press('Control+z');
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(page.locator('.poi-arrow-icon')).toHaveCount(5);
    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('Delete is ignored while an input field has focus', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');
    await setSectionOpen(page, '#btn-tag-toggle', '#tag-body', true);

    await page.locator('#tag-add-name').fill('temp');
    await page.locator('#tag-add-name').focus();
    await page.keyboard.press('Delete');

    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('Delete is ignored while route editing is active', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);
    await routeItem(page, 'test_route').locator('.btn-edit').click();
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.keyboard.press('Delete');

    // POI は消えず (alert も出ない)、route 編集はそのまま続行できる。
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('Delete is ignored while the POI edit form is open', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    // focus は Edit ボタン (入力欄ではない) のまま = isEditableTarget では弾かれない状態でも、
    // 編集中は削除しない。
    await page.keyboard.press('Delete');

    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#btn-save')).toBeDisabled();
  });
});
