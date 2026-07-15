// C. yaw 回転ハンドルの DOM 生成と drag 追従 (#284, 優先度 高)。
// 選択中 wedge POI のときだけ弧先端に .poi-yaw-handle marker が 1 個生成され、disc POI や
// 未選択では出ない。ハンドルを掴んで回すと矢印が追従し dragend で yaw が dirty 化する。
const { test, expect } = require('@playwright/test');
const { loadApp, selectPoi, unlockPoiPosition } = require('./helpers');

const DRAGGABLE_ARROW = '.poi-arrow-icon.leaflet-marker-draggable';

test.describe('C. yaw 回転ハンドルの DOM 生成と drag 追従', () => {
  test('wedge 選択でハンドル 1 個、disc 選択ではハンドル無し', async ({ page }) => {
    await loadApp(page);
    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)。yaw ハンドルも対象。
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(0);

    await selectPoi(page, 'poi_wedge'); // 0 < yawTol < pi → wedge
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(1);

    await selectPoi(page, 'poi_disc'); // yawTol >= pi → disc (yaw 不問)
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(0);
  });

  test('yaw ハンドルをドラッグすると矢印 rotation が変化し dirty 化する', async ({ page }) => {
    await loadApp(page);
    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)
    await selectPoi(page, 'poi_wedge');

    const handle = page.locator('.poi-yaw-handle');
    await expect(handle).toHaveCount(1);

    // 選択中 arrow の SVG 初期 transform (rotate) を記録。
    const arrowSvg = page.locator(`${DRAGGABLE_ARROW} svg`);
    const before = await arrowSvg.getAttribute('style');

    const box = await handle.boundingBox();
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx - 45, cy + 45, { steps: 10 });
    await page.mouse.move(cx - 70, cy + 15, { steps: 10 });
    await page.mouse.up();

    // drag 追従で arrow rotation が変わり、dragend で onPoiYawDragEnd → dirty。
    await expect(page.locator('#btn-save')).toBeEnabled();
    const after = await page.locator(`${DRAGGABLE_ARROW} svg`).getAttribute('style');
    expect(after).not.toBe(before);
  });
});
