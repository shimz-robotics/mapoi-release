// POI 位置ロック (L) / UI 一括表示 (U) の単キーショートカット (#390) の browser smoke。
// document keydown IIFE (app.js) は jsdom で実体テストしない方針 (#300 risk-based) のため、
// Delete (#374) / Ctrl+S (#375) と同じく e2e 層で配線を pin する。
// 実装は既存ボタンの .click() 再利用 (single code path) なので、ボタン側の挙動
// (poi-position-lock.e2e.js / ui-settings.e2e.js) は再検証せず、キー→ボタンの配線と
// guard (入力欄 focus / e.repeat) だけを見る。
const { test, expect } = require('@playwright/test');
const { loadApp, selectPoi } = require('./helpers');

const DRAGGABLE_ARROW = '.poi-arrow-icon.leaflet-marker-draggable';

function bodyHasClass(page, cls) {
  return page.locator('body').evaluate((el, c) => el.classList.contains(c), cls);
}

// Playwright の keyboard.press は repeat イベントを合成できないため、
// e.repeat=true の keydown を直接 dispatch する (listener は document なので届く)。
function dispatchRepeatKeydown(page, key) {
  return page.evaluate((k) => {
    document.body.dispatchEvent(
      new KeyboardEvent('keydown', { key: k, repeat: true, bubbles: true }));
  }, key);
}

test.describe('L/U キーボードショートカット (#390)', () => {
  test('L がロックボタンと同経路でトグルし、選択中 POI の draggable に反映される', async ({ page }) => {
    await loadApp(page);

    const lockBtn = page.locator('#btn-poi-lock-toggle');
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');
    // ショートカットの発見手段は title 表記 (#375 の (Ctrl+S) と同じ流儀)
    await expect(lockBtn).toHaveAttribute('title', /\(L\)$/);

    await selectPoi(page, 'poi_wedge');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);

    await page.keyboard.press('l');
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'false');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);

    // 大文字 (Shift+L) でも同じ (Caps Lock / Shift 状態に依存しない)
    await page.keyboard.press('Shift+L');
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);
  });

  test('U が Hide UI ボタンと同経路で body.ui-hidden をトグルする', async ({ page }) => {
    await loadApp(page);
    await expect(page.locator('#btn-ui-toggle')).toHaveAttribute('title', /\(U\)$/);
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);

    await page.keyboard.press('u');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(true);
    // ui-hidden 中も #ui-controls は残る仕様 (復帰手段) — U での復帰もその一部。
    // 大文字 (Shift+U) も L 側と対称に pin する (Cursor review low 対応)
    await page.keyboard.press('Shift+U');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);
  });

  test('入力欄 focus 中は L/U とも無視され、文字入力として通る', async ({ page }) => {
    await loadApp(page);
    const lockBtn = page.locator('#btn-poi-lock-toggle');

    await page.locator('#poi-search').focus();
    await page.keyboard.press('l');
    await page.keyboard.press('u');

    await expect(page.locator('#poi-search')).toHaveValue('lu');
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);
  });

  test('Ctrl+L / Ctrl+U / Cmd+L / Cmd+U (ブラウザショートカット) は所有しない', async ({ page }) => {
    await loadApp(page);

    // headless では実ブラウザ機能 (アドレスバー等) は発火しないが、app 側の
    // ハンドラが修飾キー付きを無視することは合成 dispatch で検証できる。
    // metaKey (Mac の Cmd) / altKey も同じ guard を通ることを pin する (Cursor review 対応)。
    await page.evaluate(() => {
      for (const mod of [{ ctrlKey: true }, { metaKey: true }, { altKey: true }]) {
        for (const key of ['l', 'u']) {
          document.body.dispatchEvent(
            new KeyboardEvent('keydown', { key, bubbles: true, ...mod }));
        }
      }
    });

    await expect(page.locator('#btn-poi-lock-toggle')).toHaveAttribute('aria-pressed', 'true');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);
  });

  test('長押し repeat (e.repeat) では状態が変わらない', async ({ page }) => {
    await loadApp(page);
    const lockBtn = page.locator('#btn-poi-lock-toggle');

    await dispatchRepeatKeydown(page, 'l');
    await dispatchRepeatKeydown(page, 'u');

    await expect(lockBtn).toHaveAttribute('aria-pressed', 'true');
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);

    // 非 repeat の合成 keydown は通る = dispatch 経路自体が有効なことの対照
    await page.evaluate(() => {
      document.body.dispatchEvent(
        new KeyboardEvent('keydown', { key: 'l', bubbles: true }));
    });
    await expect(lockBtn).toHaveAttribute('aria-pressed', 'false');
  });
});
