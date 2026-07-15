// beforeunload 未保存ガード (#380) の browser smoke。
// 判定 (collectReloadBlockers / shouldBlockUnload) は reload-guard.test.js で unit 済。
// ここでは window beforeunload の実配線を pin する: dirty / form open でブラウザ標準の
// 離脱確認が出ること、clean では素通しでタブが閉じられることの両面。
//
// Playwright では page.close({ runBeforeUnload: true }) が beforeunload を発火させ、
// ガードが働くと type 'beforeunload' の dialog が来る。dialog listener を登録しない場合
// Playwright は dialog を自動 dismiss する (= dismiss は「ページに留まる」) ため、
// clean ケースは「close イベントが来る = ダイアログが出ていない」ことの証明になる。
const { test, expect } = require('@playwright/test');
const { loadApp, selectPoi, setSectionOpen, poiCard } = require('./helpers');

test.describe('beforeunload unsaved-edit guard (#380)', () => {
  test('dirty POI edit triggers the leave confirmation; dismiss keeps the page', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');
    await page.keyboard.press('Delete');
    await expect(page.locator('#btn-save')).toBeEnabled();

    const dialogPromise = page.waitForEvent('dialog');
    await page.close({ runBeforeUnload: true });
    const dialog = await dialogPromise;
    expect(dialog.type()).toBe('beforeunload');
    await dialog.dismiss();

    // 留まったページはそのまま操作できる (dirty も保持)
    await expect(page.locator('#btn-save')).toBeEnabled();
  });

  test('an open edit form (not dirty yet) also triggers the confirmation', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    // form 内の入力は OK まで dirty にならないが、unload は入力を失わせる
    await expect(page.locator('#btn-save')).toBeDisabled();

    const dialogPromise = page.waitForEvent('dialog');
    await page.close({ runBeforeUnload: true });
    const dialog = await dialogPromise;
    expect(dialog.type()).toBe('beforeunload');
    await dialog.dismiss();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('clean state closes without any confirmation', async ({ page }) => {
    await loadApp(page);
    // dialog listener を登録しない: ガードが誤発火すると Playwright の自動 dismiss で
    // ページが閉じられず、close イベント待ちが timeout して fail する。
    const closed = page.waitForEvent('close');
    await page.close({ runBeforeUnload: true });
    await closed;
  });

  test('undo back to clean removes the guard again', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');
    await page.keyboard.press('Delete');
    await expect(page.locator('#btn-save')).toBeEnabled();
    await page.keyboard.press('Control+z');
    await expect(page.locator('#btn-save')).toBeDisabled();

    const closed = page.waitForEvent('close');
    await page.close({ runBeforeUnload: true });
    await closed;
  });
});
