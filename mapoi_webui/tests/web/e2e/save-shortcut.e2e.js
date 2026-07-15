// Ctrl+S で dirty なエディタを Save (#375) の browser smoke。
// document keydown IIFE (app.js) は jsdom で実体テストしない方針 (#300 risk-based) のため、
// Delete (#374 poi-delete-key) / Escape / Ctrl+Z と同じく e2e 層で配線を pin する。
const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, setSectionOpen } = require('./helpers');

// POI working copy を dirty にする最短経路 (Del ボタン → 削除は Save まで永続化されない)。
async function makePoiDirty(page) {
  await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
  await poiCard(page, 'poi_wedge').locator('.btn-delete').click();
  await expect(page.locator('#btn-save')).toBeEnabled();
}

test.describe('Save shortcut Ctrl+S (#375)', () => {
  test('Ctrl+S saves a dirty POI editor', async ({ page }) => {
    await loadApp(page);
    await makePoiDirty(page);

    const post = page.waitForRequest((req) =>
      req.url().includes('/api/pois') && req.method() === 'POST');
    await page.keyboard.press('Control+s');
    await post;

    // save 成功 → dirty 解消 (既存 save() 経路: originalPois 更新 + setDirty(false))。
    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(page.locator('#dirty-indicator')).toBeEmpty();
  });

  test('Ctrl+S works while an input field has focus', async ({ page }) => {
    await loadApp(page);
    await makePoiDirty(page);
    await setSectionOpen(page, '#btn-tag-toggle', '#tag-body', true);
    await page.locator('#tag-add-name').focus();

    const post = page.waitForRequest((req) =>
      req.url().includes('/api/pois') && req.method() === 'POST');
    await page.keyboard.press('Control+s');
    await post;

    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('Ctrl+S with nothing dirty shows a feedback toast and does not POST', async ({ page }) => {
    await loadApp(page);

    let posted = false;
    page.on('request', (req) => {
      if (req.method() === 'POST' && req.url().includes('/api/')) posted = true;
    });
    await page.keyboard.press('Control+s');

    await expect(page.locator('.toast')).toContainText('No unsaved changes');
    expect(posted).toBe(false);
  });

  test('Ctrl+S commits an open POI edit form (OK 相当) and then saves', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await page.locator('#poi-name').fill('poi_wedge_renamed');

    const post = page.waitForRequest((req) =>
      req.url().includes('/api/pois') && req.method() === 'POST');
    await page.keyboard.press('Control+s');
    await post;

    // フォームが閉じて (formOk)、リネームが working copy 経由で保存済みになる。
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(poiCard(page, 'poi_wedge_renamed')).toHaveCount(1);
    await expect(page.locator('#btn-save')).toBeDisabled();
  });

  test('Ctrl+S does not save when the open form fails validation', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await page.locator('#poi-name').fill('');

    let alertMessage = '';
    page.once('dialog', async (dialog) => {
      alertMessage = dialog.message();
      await dialog.accept();
    });
    let posted = false;
    page.on('request', (req) => {
      if (req.method() === 'POST' && req.url().includes('/api/pois')) posted = true;
    });
    await page.keyboard.press('Control+s');

    // formOk の validation alert が出てフォームは開いたまま、save へは進まない。
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    expect(alertMessage).toContain('required');
    expect(posted).toBe(false);
  });

  test('Ctrl+S does not save when the open route form fails validation', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);
    await page.locator('.route-item .btn-edit').first().click();
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await page.locator('#route-name').fill('');

    let alertMessage = '';
    page.once('dialog', async (dialog) => {
      alertMessage = dialog.message();
      await dialog.accept();
    });
    let posted = false;
    page.on('request', (req) => {
      if (req.method() === 'POST' && req.url().includes('/api/routes')) posted = true;
    });
    await page.keyboard.press('Control+s');

    // routeEditor.formOk の validation alert が出てフォームは開いたまま、save へは進まない。
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    expect(alertMessage).toContain('required');
    expect(posted).toBe(false);
  });

  test('Ctrl+S saves a dirty tag editor', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-tag-toggle', '#tag-body', true);
    await page.locator('#tag-add-name').fill('temp_tag');
    await page.locator('#btn-tag-add').click();
    await expect(page.locator('#btn-save-tags')).toBeEnabled();

    const post = page.waitForRequest((req) =>
      req.url().includes('/api/custom-tags') && req.method() === 'POST');
    await page.keyboard.press('Control+s');
    await post;

    await expect(page.locator('#btn-save-tags')).toBeDisabled();
  });

  test('Ctrl+S saves multiple dirty editors in one press', async ({ page }) => {
    await loadApp(page);
    // route を先に dirty にする: rename して OK (フォームは閉じるが Save routes はまだ)。
    // POI 削除を先にすると formOk が missing waypoint の confirm を出し得るため順序固定。
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);
    await page.locator('.route-item .btn-edit').first().click();
    await page.locator('#route-name').fill('test_route_renamed');
    await page.locator('#btn-route-form-ok').click();
    await expect(page.locator('#btn-save-routes')).toBeEnabled();
    await makePoiDirty(page);

    const poiPost = page.waitForRequest((req) =>
      req.url().includes('/api/pois') && req.method() === 'POST');
    const routePost = page.waitForRequest((req) =>
      req.url().includes('/api/routes') && req.method() === 'POST');
    await page.keyboard.press('Control+s');
    await Promise.all([poiPost, routePost]);

    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(page.locator('#btn-save-routes')).toBeDisabled();
  });
});
