// UI 表示コントロールの実ブラウザ配線 (#323 半透明化 / #324 UI オン/オフトグル)。
// ui-settings.js の pure helper は unit test で pin 済み。ここでは index.html /
// style.css / ui-settings.js を通した DOM 配線 (class toggle・CSS 変数・localStorage
// 永続) を実ブラウザでのみ通せる経路として検証する。
const { test, expect } = require('@playwright/test');
const { loadApp, displayOf } = require('./helpers');

function bodyHasClass(page, cls) {
  return page.locator('body').evaluate((el, c) => el.classList.contains(c), cls);
}

function cssVar(page, name) {
  return page.evaluate(
    (n) => getComputedStyle(document.body).getPropertyValue(n).trim(),
    name,
  );
}

function positionOf(page, selector) {
  return page.locator(selector).evaluate((el) => getComputedStyle(el).position);
}

// range input は fill() 非対応なので value 直接設定 + input/change を明示 dispatch。
async function setAlphaPercent(page, percent) {
  await page.locator('#ui-alpha-slider').evaluate((el, p) => {
    el.value = String(p);
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }, percent);
}

test.describe('UI 表示コントロール (#323 / #324)', () => {
  test('トグルで header/panel を隠し、再クリックで戻す。#ui-controls は残り pressed 状態が同期する', async ({ page }) => {
    await loadApp(page);
    const toggleBtn = page.locator('#btn-ui-toggle');
    expect(await displayOf(page, 'header')).not.toBe('none');
    expect(await displayOf(page, '#poi-panel')).not.toBe('none');
    // アイコンは固定の ☰ (#390、swap しない)。状態は aria-pressed と title で示し、
    // pressed = UI 表示中 (#449) なので既定 (表示) で青
    await expect(toggleBtn.locator('.ui-control-glyph')).toHaveText('☰');
    await expect(toggleBtn).toHaveAttribute('aria-pressed', 'true');
    await expect(toggleBtn).toHaveAttribute('title', 'Hide UI (U)');

    await toggleBtn.click();
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(true);
    expect(await displayOf(page, 'header')).toBe('none');
    expect(await displayOf(page, '#poi-panel')).toBe('none');
    // UI を復帰できるようコントロール自身は残し、非表示中は消灯 + title が「戻す」側へ同期
    expect(await displayOf(page, '#ui-controls')).not.toBe('none');
    await expect(toggleBtn.locator('.ui-control-glyph')).toHaveText('☰');
    await expect(toggleBtn).toHaveAttribute('aria-pressed', 'false');
    await expect(toggleBtn).toHaveAttribute('title', 'Show UI (U)');

    await toggleBtn.click();
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(false);
    expect(await displayOf(page, 'header')).not.toBe('none');
    expect(await displayOf(page, '#poi-panel')).not.toBe('none');
  });

  test('Display section は既定折畳で、開くと overlay チェックで panel が地図上に絶対配置される', async ({ page }) => {
    await loadApp(page);
    expect(await positionOf(page, '#poi-panel')).not.toBe('absolute');
    // 既定は折畳 (地図上でなくパネル内に置いた表示設定)
    expect(await displayOf(page, '#display-body')).toBe('none');

    await page.locator('#btn-display-toggle').click();
    expect(await displayOf(page, '#display-body')).not.toBe('none');
    await page.locator('#ui-overlay-toggle').check();

    expect(await bodyHasClass(page, 'ui-overlay')).toBe(true);
    expect(await positionOf(page, '#poi-panel')).toBe('absolute');
  });

  test('不透明度スライダーが --ui-panel-alpha と表示ラベルを更新する', async ({ page }) => {
    await loadApp(page);
    expect(await cssVar(page, '--ui-panel-alpha')).toBe('1');

    await page.locator('#btn-display-toggle').click();
    await setAlphaPercent(page, 50);

    expect(await cssVar(page, '--ui-panel-alpha')).toBe('0.5');
    expect(await page.locator('#ui-alpha-value').textContent()).toBe('50%');
  });

  test('設定が reload をまたいで localStorage に永続化される', async ({ page }) => {
    await loadApp(page);
    await page.locator('#btn-display-toggle').click();
    await setAlphaPercent(page, 40);
    await page.locator('#btn-ui-toggle').click(); // hidden = true
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(true);

    await loadApp(page); // reload (同一 context で localStorage は保持)
    expect(await bodyHasClass(page, 'ui-hidden')).toBe(true);
    expect(await cssVar(page, '--ui-panel-alpha')).toBe('0.4');
  });
});
