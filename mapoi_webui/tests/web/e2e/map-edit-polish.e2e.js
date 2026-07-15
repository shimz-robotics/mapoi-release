const { test, expect } = require('@playwright/test');
const { loadApp, poiCard, selectPoi, setSectionOpen } = require('./helpers');

function routeItem(page, name) {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  return page.locator('.route-item').filter({
    has: page.locator('.route-item-name', { hasText: new RegExp(`^${escaped}$`) }),
  });
}

async function markerZ(locator) {
  return locator.evaluate((el) => Number.parseInt(getComputedStyle(el).zIndex || '0', 10) || 0);
}

async function maxMarkerZ(page, selector) {
  return page.locator(selector).evaluateAll((els) =>
    Math.max(...els.map((el) => Number.parseInt(getComputedStyle(el).zIndex || '0', 10) || 0)));
}

async function markerCenter(page, index) {
  const box = await page.locator('.poi-arrow-icon').nth(index).boundingBox();
  expect(box).not.toBeNull();
  return { x: box.x + box.width / 2, y: box.y + box.height / 2 };
}

function distance(a, b) {
  return Math.hypot(a.x - b.x, a.y - b.y);
}

test.describe('Map edit polish', () => {
  test('sidebar sections are ordered Navigation, Tags, POIs, Routes, Display', async ({ page }) => {
    await loadApp(page);

    const ids = await page.locator('#poi-panel > .panel-section').evaluateAll((els) =>
      els.map((el) => el.id));

    expect(ids).toEqual(['nav-section', 'tag-section', 'poi-section', 'route-section', 'display-section']);
  });

  test('Escape clears POI and route selection without opening edit forms', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);

    await selectPoi(page, 'poi_wedge');
    await routeItem(page, 'test_route').locator('.route-item-name').click();
    await expect(routeItem(page, 'test_route')).toHaveClass(/(^|\s)selected(\s|$)/);
    await expect(page.locator('#nav-goal-select')).toHaveValue('poi_wedge');
    await expect(page.locator('#nav-route-select')).toHaveValue('test_route');

    await page.keyboard.press('Escape');

    await expect(poiCard(page, 'poi_wedge')).not.toHaveClass(/(^|\s)selected(\s|$)/);
    await expect(routeItem(page, 'test_route')).not.toHaveClass(/(^|\s)selected(\s|$)/);
    await expect(page.locator('#nav-goal-select')).toHaveValue('');
    await expect(page.locator('#nav-route-select')).toHaveValue('');
    await expect(page.locator('#nav-initialpose-select')).toHaveValue('');
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#route-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('Escape cancels active POI and Route editing modes', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await page.keyboard.press('Escape');
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(poiCard(page, 'poi_wedge')).toHaveClass(/(^|\s)selected(\s|$)/);

    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);
    await routeItem(page, 'test_route').locator('.btn-edit').click();
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await page.locator('#route-name').focus();
    await page.keyboard.press('Escape');
    await expect(page.locator('#route-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(routeItem(page, 'test_route')).toHaveClass(/(^|\s)selected(\s|$)/);
  });

  test('Escape clears pending POI pose point before canceling POI edit', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.locator('#map').click({ position: { x: 260, y: 180 } });
    await page.keyboard.press('Escape');
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.keyboard.press('Escape');
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('POI edit cancel restores marker preview position', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    const original = await markerCenter(page, 0);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.locator('#map').click({ position: { x: 320, y: 180 } });
    await page.locator('#map').click({ position: { x: 360, y: 180 } });
    const preview = await markerCenter(page, 0);
    expect(distance(preview, original)).toBeGreaterThan(8);

    await page.locator('#btn-form-cancel').click();
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    const restored = await markerCenter(page, 0);
    expect(distance(restored, original)).toBeLessThan(2);
  });

  test('Escape cancels POI placing mode before map click', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    await page.locator('#btn-add-poi').click();
    await expect(page.locator('#btn-add-poi')).toContainText('Click map...');
    await expect(page.locator('#btn-add-poi')).toHaveClass(/(^|\s)placing(\s|$)/);

    await page.keyboard.press('Escape');

    await expect(page.locator('#btn-add-poi')).toContainText('+ Add POI');
    await expect(page.locator('#btn-add-poi')).not.toHaveClass(/(^|\s)placing(\s|$)/);
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('POI edit and Route edit are mutually exclusive from sidebar actions', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);

    await routeItem(page, 'test_route').locator('.btn-edit').click();
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);

    let routeEditMessage = '';
    page.once('dialog', async (dialog) => {
      routeEditMessage = dialog.message();
      await dialog.accept();
    });
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    expect(routeEditMessage).toContain('route editing');
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#poi-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);

    await page.locator('#btn-route-form-cancel').click();
    await setSectionOpen(page, '#btn-poi-toggle', '#poi-body', true);
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);

    let poiEditMessage = '';
    page.once('dialog', async (dialog) => {
      poiEditMessage = dialog.message();
      await dialog.accept();
    });
    await routeItem(page, 'test_route').locator('.btn-edit').click();
    expect(poiEditMessage).toContain('POI editing');
    await expect(page.locator('#poi-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#route-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
  });

  test('selected and editing routes show linked landmark POIs', async ({ page }) => {
    await loadApp(page);
    await setSectionOpen(page, '#btn-route-toggle', '#route-body', true);

    await routeItem(page, 'test_route').locator('.route-item-name').click();
    await expect(page.locator('.route-landmark-badge')).toHaveCount(1);
    await expect(page.locator('.route-landmark-radius-ring')).toHaveCount(1);

    await routeItem(page, 'test_route').locator('.route-item-name').click();
    await expect(page.locator('.route-landmark-badge')).toHaveCount(0);
    await expect(page.locator('.route-landmark-radius-ring')).toHaveCount(0);

    await routeItem(page, 'test_route').locator('.btn-edit').click();
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('.route-landmark-badge')).toHaveCount(1);
    await expect(page.locator('.route-landmark-radius-ring')).toHaveCount(1);
  });

  test('marker layer priority follows editing and navigation modes', async ({ page }) => {
    await loadApp(page);
    await expect(page.locator('.robot-icon')).toHaveCount(1);

    const defaultRobotZ = await markerZ(page.locator('.robot-icon'));
    const defaultPoiZ = await maxMarkerZ(page, '.poi-arrow-icon');
    expect(defaultRobotZ).toBeGreaterThan(defaultPoiZ);

    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    const editingRobotZ = await markerZ(page.locator('.robot-icon'));
    const editingPoiZ = await maxMarkerZ(page, '.poi-arrow-icon');
    expect(editingPoiZ).toBeGreaterThan(editingRobotZ);

    await page.locator('#btn-form-cancel').click();
    await page.locator('#nav-route-select').selectOption('test_route');
    await page.locator('#btn-nav-run').click();
    await expect(page.locator('#nav-status-text')).toContainText('Navigating');

    const navRobotZ = await markerZ(page.locator('.robot-icon'));
    const navPoiZ = await maxMarkerZ(page, '.poi-arrow-icon');
    expect(navRobotZ).toBeGreaterThan(navPoiZ);
  });
});
