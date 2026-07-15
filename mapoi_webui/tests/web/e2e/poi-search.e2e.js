// POI list 検索ボックス (#383) の browser smoke。
// 絞り込み判定 (matchesPoiName) は poi-filter.test.js で unit 済。ここでは
// input → renderList の配線、実 index 整合 (選択 / nav dropdown 連動)、
// map marker の可視状態と独立であること、Escape / 編集フォームとの共存を pin する。
const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, selectPoi, setSectionOpen } = require('./helpers');

test.describe('POI list search box (#383)', () => {
  test('narrows the list by name without touching map markers', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await expect(page.locator('.poi-card')).toHaveCount(5);

    await page.locator('#poi-search').fill('wedge');
    await expect(page.locator('.poi-card')).toHaveCount(2);
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(1);
    await expect(poiCard(page, 'poi_wedge2')).toHaveCount(1);
    // 絞り込みは list 表示のみ: marker は全件のまま
    await expect(page.locator('.poi-arrow-icon')).toHaveCount(5);

    // 大文字小文字不区別
    await page.locator('#poi-search').fill('WEDGE');
    await expect(page.locator('.poi-card')).toHaveCount(2);

    // 一致なしは 0 件表示 (marker は変わらず)
    await page.locator('#poi-search').fill('zzz');
    await expect(page.locator('.poi-card')).toHaveCount(0);
    await expect(page.locator('.poi-arrow-icon')).toHaveCount(5);

    // クリアで全件復帰
    await page.locator('#poi-search').fill('');
    await expect(page.locator('.poi-card')).toHaveCount(5);
  });

  test('selecting a filtered card selects the right POI (index integrity)', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await page.locator('#poi-search').fill('pause');
    await expect(page.locator('.poi-card')).toHaveCount(1);
    await poiCard(page, 'poi_pause').locator('.poi-card-name').click();
    await expect(poiCard(page, 'poi_pause')).toHaveClass(/(^|\s)selected(\s|$)/);
    // card は実 index ベース (#383): 選択が nav goal dropdown 連動 (#111) でも正しい
    // POI を指すことを、絞り込み表示中の選択で確認する。
    await expect(page.locator('#nav-goal-select')).toHaveValue('poi_pause');
  });

  test('Escape clears a non-empty query without dropping the selection', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_wedge');

    await page.locator('#poi-search').fill('wedge');
    await expect(page.locator('.poi-card')).toHaveCount(2);
    await page.locator('#poi-search').press('Escape');
    await expect(page.locator('#poi-search')).toHaveValue('');
    await expect(page.locator('.poi-card')).toHaveCount(5);
    // クリアは選択解除 (#309) より優先: 選択はそのまま残る
    await expect(poiCard(page, 'poi_wedge')).toHaveClass(/(^|\s)selected(\s|$)/);
  });

  test('filtering out the selected POI hides its card but keeps the selection', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await selectPoi(page, 'poi_pause');
    await expect(page.locator('#nav-goal-select')).toHaveValue('poi_pause');

    await page.locator('#poi-search').fill('wedge');
    await expect(poiCard(page, 'poi_pause')).toHaveCount(0);
    // 絞り込みは list の見た目だけ (一貫規則): map の highlight (選択 marker は
    // stroke #e67e22) と nav dropdown は選択を保持し続ける
    await expect(page.locator('.poi-arrow-icon:has(path[stroke="#e67e22"])')).toHaveCount(1);
    await expect(page.locator('#nav-goal-select')).toHaveValue('poi_pause');

    // クリアすると card が選択状態のまま戻る
    await page.locator('#poi-search').fill('');
    await expect(poiCard(page, 'poi_pause')).toHaveClass(/(^|\s)selected(\s|$)/);
  });

  test('the search box hides while the POI edit form is open', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#poi-search')).toBeHidden();

    await page.locator('#btn-form-cancel').click();
    await expect(page.locator('#poi-search')).toBeVisible();
  });
});
