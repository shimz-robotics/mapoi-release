// 自タブ save 起因の SSE config_changed で reload しない (#384) の browser smoke。
// mock server の POST /test/sse-event (test 専用) で config_changed を注入する。
// 注入は接続中の全 SSE client への broadcast で並走テストの page にも届くため、
// この spec は専用 project (chromium-sse、他テスト完了後に単独実行) に隔離されている
// (playwright.config.js)。
const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, setSectionOpen } = require('./helpers');

// EventSource の接続完了 (clients >= 1) を待ってから実 event を 1 回だけ送る。
// 接続前に送ると event は誰にも届かず、reload を待つ側のテストが timeout する。
// 接続確認の poll には frontend が無視する type を使い、二重配信の副作用を避ける。
async function sendSse(page, event) {
  await expect.poll(async () => {
    const resp = await page.request.post('/test/sse-event', { data: { type: 'test_ping' } });
    return (await resp.json()).clients;
  }).toBeGreaterThan(0);
  const resp = await page.request.post('/test/sse-event', { data: event });
  expect(resp.ok()).toBeTruthy();
}

async function fixtureConfigVersion(page) {
  const resp = await page.request.get('/api/pois');
  return (await resp.json()).config_version;
}

// 共通前段: POI を 1 件削除して Save し、「自タブが version を既知として保持する」状態を作る。
// mock の POST /api/pois は固定 state の config_version を返すので、実 backend の
// save 応答 (新 version 受領) と同型になる。
async function deletePoiAndSave(page) {
  await poiCard(page, 'poi_wedge').locator('.btn-delete').click();
  await page.locator('#btn-save').click();
  await expect(page.locator('#btn-save')).toBeDisabled();
  await expect(page.locator('.poi-card')).toHaveCount(4);
}

test.describe('SSE self-change skip (#384)', () => {
  test('既知 version の config_changed では reload せず undo 履歴と作業状態が残る', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    const version = await fixtureConfigVersion(page);

    await deletePoiAndSave(page);
    await expect(page.locator('#btn-undo')).toBeEnabled(); // save 後も undo 履歴は残る (#300)

    await sendSse(page, {
      type: 'config_changed',
      payload: { map_name: 'test_map', config_version: version },
    });

    // reload されると mock の固定 state から POI が 5 件に戻り undo も無効化される。
    // debounce (200ms) を確実に跨いでから「何も起きていない」ことを pin する。
    await page.waitForTimeout(800);
    await expect(page.locator('.poi-card')).toHaveCount(4);
    await expect(poiCard(page, 'poi_wedge')).toHaveCount(0);
    await expect(page.locator('#btn-undo')).toBeEnabled();
  });

  test('未知 version の config_changed では従来どおり reload する', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await deletePoiAndSave(page);

    await sendSse(page, {
      type: 'config_changed',
      payload: { map_name: 'test_map', config_version: 'external-version-differs' },
    });

    // 全体 reload で mock の固定 state (POI 5 件) に戻り、loadPois が undo 履歴を破棄する。
    await expect(page.locator('.poi-card')).toHaveCount(5);
    await expect(page.locator('#btn-undo')).toBeDisabled();
  });

  test('version 無しの config_changed は従来どおり reload に回す (後方互換)', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await deletePoiAndSave(page);

    await sendSse(page, { type: 'config_changed', payload: { map_name: 'test_map' } });

    await expect(page.locator('.poi-card')).toHaveCount(5);
    await expect(page.locator('#btn-undo')).toBeDisabled();
  });
});
