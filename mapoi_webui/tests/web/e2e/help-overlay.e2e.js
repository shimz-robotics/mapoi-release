// Help overlay (#391) の browser smoke。
// 開閉配線 (help.js) と keydown funnel (app.js) の「Escape は help open が最優先」を
// e2e 層で pin する (document keydown を jsdom で実体テストしない方針 #300 の続き)。
const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, selectPoi, setSectionOpen } = require('./helpers');

function overlay(page) {
  return page.locator('#help-overlay');
}

function bodyHasClass(page, cls) {
  return page.locator('body').evaluate((el, c) => el.classList.contains(c), cls);
}

test.describe('Help overlay (#391)', () => {
  test('? ボタンで開き × で閉じる。ショートカット一覧に L/U/? が載っている', async ({ page }) => {
    await loadApp(page);
    await expect(overlay(page)).toBeHidden();

    await page.locator('#btn-help').click();
    await expect(overlay(page)).toBeVisible();

    // #390 とのメンテ規約 pin: funnel のショートカットが一覧に揃っている
    const table = page.locator('#help-shortcut-table');
    for (const key of ['Ctrl', 'Delete', 'Escape', 'L', 'U', '?']) {
      await expect(table.locator('kbd', { hasText: new RegExp(`^${key.replace('?', '\\?')}$`) }).first())
        .toBeVisible();
    }

    await page.locator('#btn-help-close').click();
    await expect(overlay(page)).toBeHidden();
  });

  test('? キーで開き Escape で閉じる', async ({ page }) => {
    await loadApp(page);

    await page.keyboard.press('?');
    await expect(overlay(page)).toBeVisible();

    await page.keyboard.press('Escape');
    await expect(overlay(page)).toBeHidden();
  });

  test('背景 click で閉じる。dialog 内 click では閉じない', async ({ page }) => {
    await loadApp(page);
    await page.keyboard.press('?');
    await expect(overlay(page)).toBeVisible();

    await page.locator('#help-dialog').click();
    await expect(overlay(page)).toBeVisible();

    // overlay の左上 padding 領域 = dialog の外 (背景)
    await overlay(page).click({ position: { x: 5, y: 5 } });
    await expect(overlay(page)).toBeHidden();
  });

  test('編集フォーム open 中の Escape は Help だけ閉じ、フォームは残る (優先順位の pin)', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.keyboard.press('?');
    await expect(overlay(page)).toBeVisible();

    // 1 回目: Help open が最優先 — Help のみ閉じる
    await page.keyboard.press('Escape');
    await expect(overlay(page)).toBeHidden();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    // 2 回目: 従来どおりフォーム Cancel に落ちる
    await page.keyboard.press('Escape');
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('Help 表示中は他のショートカット (L / U / Delete) が不発になる', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');

    await page.keyboard.press('?');
    await expect(overlay(page)).toBeVisible();

    // 閲覧中の誤操作防止: toggle も選択 POI の削除も走らない
    await page.keyboard.press('l');
    await page.keyboard.press('u');
    await page.keyboard.press('Delete');
    // Ctrl+S も保存経路に入らない (clean 時の 'No unsaved changes' toast (#375) が
    // 出ないことで gate を検証。ブラウザ保存ダイアログの抑止自体は e2e では見えない)
    await page.keyboard.press('Control+s');

    await expect(page.locator('#btn-poi-lock-toggle')).toHaveAttribute('aria-pressed', 'true');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(page.locator('.toast')).toHaveCount(0);

    // 閉じれば従来どおり効く
    await page.keyboard.press('Escape');
    await page.keyboard.press('l');
    await expect(page.locator('#btn-poi-lock-toggle')).toHaveAttribute('aria-pressed', 'false');
  });

  test('入力欄 focus 中の ? は無視され、文字入力として通る', async ({ page }) => {
    await loadApp(page);

    await page.locator('#poi-search').focus();
    await page.keyboard.press('?');

    await expect(page.locator('#poi-search')).toHaveValue('?');
    await expect(overlay(page)).toBeHidden();
  });

  test('ui-hidden 中も ? ボタンから Help に到達できる', async ({ page }) => {
    await loadApp(page);
    await page.keyboard.press('u');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(true);

    await expect(page.locator('#btn-help')).toBeVisible();
    await page.locator('#btn-help').click();
    await expect(overlay(page)).toBeVisible();
  });
});
