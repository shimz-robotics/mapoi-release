const { test, expect } = require('@playwright/test');
const { loadApp, setSectionOpen } = require('./helpers');

const LANDMARK_DESC = 'Reference POI used for route context and radius monitoring; not sent to Nav2 as a navigation target';

function tagItem(page, name) {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return page.locator('.tag-item').filter({
    has: page.locator('.tag-item-name', { hasText: new RegExp(`^${escaped}$`) }),
  });
}

test.describe('Tag description disclosure', () => {
  test('desktop keeps compact layout while exposing full description by title and keyboard expansion', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-tag-toggle', '#tag-body', true);

    const desc = tagItem(page, 'landmark').locator('.tag-item-desc');
    await expect(desc).toHaveAttribute('title', LANDMARK_DESC);
    await expect(desc).toHaveAttribute('aria-expanded', 'false');
    await expect(desc).toHaveCSS('white-space', 'nowrap');

    await desc.focus();
    await page.keyboard.press('Enter');

    await expect(desc).toHaveAttribute('aria-expanded', 'true');
    await expect(desc).toHaveCSS('white-space', 'normal');

    await desc.focus();
    await page.keyboard.press('Enter');
    await expect(desc).toHaveAttribute('aria-expanded', 'false');
    await expect(desc).toHaveCSS('white-space', 'nowrap');
  });

  test('mobile can tap a tag description to expand and collapse the full text inline', async ({ page }) => {
    await page.setViewportSize({ width: 390, height: 900 });
    await loadApp(page);
    await setSectionOpen(page, '#btn-tag-toggle', '#tag-body', true);

    const desc = tagItem(page, 'landmark').locator('.tag-item-desc');
    await expect(desc).toHaveAttribute('aria-expanded', 'false');
    await expect(desc).toHaveCSS('white-space', 'nowrap');

    await desc.click();
    await expect(desc).toHaveAttribute('aria-expanded', 'true');
    await expect(desc).toHaveCSS('white-space', 'normal');

    await desc.click();
    await expect(desc).toHaveAttribute('aria-expanded', 'false');
    await expect(desc).toHaveCSS('white-space', 'nowrap');
  });
});
