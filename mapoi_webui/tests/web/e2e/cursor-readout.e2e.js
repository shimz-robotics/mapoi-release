// Map cursor の world 座標 readout (#381) の browser smoke。
// mousemove → latLngToWorld → 表示更新の Leaflet 配線は jsdom で動かないため e2e 層で
// pin する (フォーマット自体は poi-filter.test.js の formatCursorCoords で unit 済)。
const { test, expect } = require('@playwright/test');
const { loadApp } = require('./helpers');

test.describe('cursor coordinate readout (#381)', () => {
  test('shows world coordinates while hovering the map and hides on leave', async ({ page }) => {
    await loadApp(page);
    const readout = page.locator('#cursor-readout');
    await expect(readout).toBeHidden();

    const box = await page.locator('#map').boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await expect(readout).toBeVisible();
    // 桁は POI yaml の丸め (POSE_XY_DIGITS = 3) に固定
    await expect(readout).toHaveText(/^x: -?\d+\.\d{3}, y: -?\d+\.\d{3}$/);

    // カーソル移動で値が追従する
    const first = await readout.textContent();
    await page.mouse.move(box.x + box.width / 4, box.y + box.height / 4);
    await expect(readout).not.toHaveText(first);

    // map 外 (ヘッダ側) へ出たら非表示に戻る
    await page.mouse.move(box.x + box.width / 2, 5);
    await expect(readout).toBeHidden();
  });
});
