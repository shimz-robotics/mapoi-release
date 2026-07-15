// POI 位置編集ロックトグルと地図ズームボタン削除 (#333)。
// 位置編集は「たまに行う設定変更」なので既定ロック (安全側)。対象は選択中 POI の
// drag + yaw ハンドルのみ (Add POI 配置クリック等、事前に明示ボタンが要るフローは対象外)。
const { test, expect } = require('@playwright/test');
const { loadApp, selectPoi } = require('./helpers');

const DRAGGABLE_ARROW = '.poi-arrow-icon.leaflet-marker-draggable';

test.describe('POI 位置ロック (#333)', () => {
  test('既定はロックで選択してもドラッグ不可、トグルで解除・再ロックできる', async ({ page }) => {
    await loadApp(page);

    const lockBtn = page.locator('#btn-poi-lock-toggle');
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');

    await selectPoi(page, 'poi_wedge');
    // ロック中は選択しても draggable にならない。
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(0);

    await lockBtn.click();
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'false');
    // 解除後は選択中 marker が draggable になる (再選択不要、その場で反映)。
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(1);

    await lockBtn.click();
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);
    await expect(page.locator('.poi-yaw-handle')).toHaveCount(0);
  });

  test('route 編集中はロック解除していてもドラッグ不可 (既存の抑止と AND)', async ({ page }) => {
    await loadApp(page);
    await page.locator('#btn-poi-lock-toggle').click(); // unlock
    await selectPoi(page, 'poi_wedge');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);

    await page.locator('#btn-add-route').click();
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);

    await page.locator('#btn-route-form-cancel').click();
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);
  });
});

test.describe('地図ズームボタン削除 (#333)', () => {
  test('Leaflet 既定の +/- ズームコントロールが存在しない', async ({ page }) => {
    await loadApp(page);
    await expect(page.locator('.leaflet-control-zoom')).toHaveCount(0);
  });

  test('ボタンが無くてもホイールでズームできる (Codex review low 対応)', async ({ page }) => {
    await loadApp(page);

    const mapEl = page.locator('#map');
    // 初期表示 (loadMap → fitBounds) で 'zoom' イベントが発火し zoom が反映されるのを待つ。
    await expect(mapEl).toHaveAttribute('data-zoom', /.+/);
    const before = await mapEl.getAttribute('data-zoom');

    const box = await mapEl.boundingBox();
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.wheel(0, -300); // 上スクロール = ズームイン (Leaflet 既定)

    await expect
      .poll(async () => mapEl.getAttribute('data-zoom'))
      .not.toBe(before);
  });
});
