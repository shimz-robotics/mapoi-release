// mapoi_webui e2e 共有ヘルパ (#284)。*.e2e.js ではないので Playwright のテスト対象にならない。
const { expect } = require('@playwright/test');

// app 起動を明示待ちする。SSE (/api/events) が開きっぱなしで 'networkidle' は来ないため、
// 代わりに「全 POI marker が描画される」= loadMaps→switchMap→loadPois→showPois 完了 を待つ。
// marker は初期化の途中 (loadPois) で描画され、keydown listener 登録は IIFE 末尾なので、
// load 直後に keyboard 操作するテストが race しないよう data-mapoi-ready も併せて待つ (#375)。
async function loadApp(page) {
  const resp = await page.goto('/');
  expect(resp.status()).toBe(200);
  await expect(page.locator('.poi-arrow-icon')).toHaveCount(5);
  await expect(page.locator('body[data-mapoi-ready="1"]')).toHaveCount(1);
}

// POI 名の完全一致で card を返す。'poi_wedge' が 'poi_wedge2' に部分一致しないよう anchored regex。
// 名前に regex メタ文字が来ても壊れないようエスケープする。
function poiCard(page, name) {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return page.locator('.poi-card').filter({
    has: page.locator('.poi-card-name', { hasText: new RegExp(`^${escaped}$`) }),
  });
}

// card 本体クリックで selectPoi → onSelectionChange → highlightPoi (marker 強調 + draggable + yaw ハンドル)。
// Edit/Copy/Del/checkbox は stopPropagation するので、name 要素をクリックして card の click に委ねる。
async function selectPoi(page, name) {
  await poiCard(page, name).locator('.poi-card-name').click();
  await expect(poiCard(page, name)).toHaveClass(/(^|\s)selected(\s|$)/);
}

// computed display を返す ('' ではなく実値 block/flex/none)。
function displayOf(page, selector) {
  return page.locator(selector).evaluate((el) => getComputedStyle(el).display);
}

// 指定 section を望む開閉状態にする (toggle ボタンを必要時だけ押す)。
async function setSectionOpen(page, toggleId, bodyId, wantOpen) {
  const isClosed = (await displayOf(page, bodyId)) === 'none';
  if (wantOpen === isClosed) {
    await page.locator(toggleId).click();
  }
}

// POI 位置編集ロックは既定 ON (#333)。drag / yaw ハンドルを実際に動かすテストは
// 事前にこれで解除する (route 編集中の抑止とは独立な条件なので個別に外す必要がある)。
async function unlockPoiPosition(page) {
  const btn = page.locator('#btn-poi-lock-toggle');
  if ((await btn.getAttribute('aria-pressed')) === 'true') {
    await btn.click();
  }
}

module.exports = { loadApp, poiCard, selectPoi, displayOf, setSectionOpen, unlockPoiPosition };
