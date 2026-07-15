// B. POI ドラッグ enable/disable の実適用 (#284, 優先度 高)。
// pure helper shouldEnablePoiDrag の bool を実際に L.marker.dragging.enable()/disable() へ
// 流した結果を実ブラウザで検証する。Leaflet は draggable な marker の icon に
// 'leaflet-marker-draggable' class を付けるので、これで「掴める/掴めない」を観測する。
const { test, expect } = require('@playwright/test');
const { loadApp, selectPoi, unlockPoiPosition, poiCard } = require('./helpers');

// 選択中 (highlighted) の POI arrow marker = draggable。yaw ハンドル (.poi-yaw-handle) も
// draggable だが class が異なるので、.poi-arrow-icon に限定すれば arrow だけ数えられる。
const DRAGGABLE_ARROW = '.poi-arrow-icon.leaflet-marker-draggable';

test.describe('B. POI ドラッグ enable/disable の実適用', () => {
  test('選択中 POI marker だけ draggable になり、ドラッグで dirty 化する', async ({ page }) => {
    await loadApp(page);
    // 初期は draggable な arrow marker 無し・未 dirty。
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);
    await expect(page.locator('#btn-save')).toBeDisabled();

    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)
    await selectPoi(page, 'poi_wedge');
    // 選択中 1 個だけ draggable。
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);

    // その marker を実ドラッグ → dragend → onPoiDragEnd → working copy 更新 + dirty。
    const marker = page.locator(DRAGGABLE_ARROW);
    const box = await marker.boundingBox();
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 45, cy + 30, { steps: 10 });
    await page.mouse.move(cx + 75, cy + 55, { steps: 10 });
    await page.mouse.up();

    await expect(page.locator('#btn-save')).toBeEnabled();
    await expect(page.locator('#dirty-indicator')).not.toBeEmpty();
  });

  test('route 編集中は選択中 POI でもドラッグ不可になり、Cancel で戻る', async ({ page }) => {
    await loadApp(page);
    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)
    await selectPoi(page, 'poi_wedge');
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);

    // route 編集開始 → routeEditor.onEditingChange → setPoiDraggingAllowed(false)。
    await page.locator('#btn-add-route').click();
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(0);

    // Cancel → setPoiDraggingAllowed(true) で再び draggable。
    await page.locator('#btn-route-form-cancel').click();
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);
  });
});

test.describe('モバイルの誤ドラッグ即取り消し (#332)', () => {
  test('#poi-body が畳まれたままでもフローティング Undo で drag を取り消せる', async ({ page }) => {
    // compactMediaQuery の初期判定は script 実行時の一発読みなので、navigation 前に
    // viewport を設定する (nav-controls.js initialSectionOpenStates)。
    await page.setViewportSize({ width: 390, height: 844 });
    await loadApp(page);

    // 前提: モバイルでは POI panel はデフォルト畳み (地図領域確保)。
    await expect(page.locator('#poi-body')).toBeHidden();
    const floatUndo = page.locator('#btn-poi-undo-float');
    await expect(floatUndo).toBeHidden();

    // 位置ロックは既定 ON (#333)。#ui-controls はパネル開閉と無関係に常時見えるので
    // モバイルでもパネルを開かず解除できる。
    await unlockPoiPosition(page);

    // パネルを開かず、地図上の marker を直接タップして選択 (#poi-list の click には依らない)。
    const marker = page.locator('.poi-arrow-icon').first();
    await marker.click();
    await expect(page.locator(DRAGGABLE_ARROW)).toHaveCount(1);

    const box = await marker.boundingBox();
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 40, cy + 30, { steps: 8 });
    await page.mouse.up();

    await expect(page.locator('#btn-save')).toBeEnabled();
    await expect(floatUndo).toBeVisible();
    // パネルはドラッグだけでは開かない。
    await expect(page.locator('#poi-body')).toBeHidden();

    await floatUndo.click();

    // undo で working copy が originalPois と一致 = dirty 解消、フローティング Undo も消える。
    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(floatUndo).toBeHidden();
    await expect(page.locator('#poi-body')).toBeHidden();
  });

  // POI undo/redo/discard/save はいずれも route 編集と無関係にいつでも実行できてよい
  // (route 側の状態でブロックする必要は無い)。危険なのは副作用として走る loadRoutes()
  // (サーバ最新状態で route working copy を丸ごと上書きする) の方なので、
  // poiEditor.onDirtyChange 側の呼び出しを route 編集フォームが開いている間・
  // OK 後 Save 前の未保存 route 変更が残っている間だけ skip する (Codex review #334
  // round 1-4: フローティング Undo・通常 Undo/Redo/Discard ボタン・Ctrl+Z・POI Save の
  // 全経路が同じ問題を踏むと判明したため、個々の入口ではなく発火元 1 箇所で一元的に守る)。
  test('route 編集フォームが開いている間の POI undo は即座に効くが、route working copy は守られる', async ({ page }) => {
    await loadApp(page);
    const floatUndo = page.locator('#btn-poi-undo-float');

    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)
    await selectPoi(page, 'poi_wedge');
    const marker = page.locator(DRAGGABLE_ARROW);
    const box = await marker.boundingBox();
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 40, cy + 30, { steps: 8 });
    await page.mouse.up();
    await expect(floatUndo).toBeVisible();

    // route 編集を開始 (未保存の name 入力あり、フォームは開いたまま)。
    await page.locator('#btn-add-route').click();
    await page.locator('#route-name').fill('未保存ルート');

    // ブロックはしない。フローティング Undo は route 編集中も見えて即座に効く。
    await expect(floatUndo).toBeVisible();
    await floatUndo.click();
    await expect(page.locator('#btn-save')).toBeDisabled();

    // だが route フォームはリセットされず (loadRoutes() が skip された)、未保存入力が残る。
    await expect(page.locator('#route-edit-form')).not.toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#route-name')).toHaveValue('未保存ルート');
  });

  test('route を OK して Save routes 前の間も、通常Undo/Ctrl+Z/POI Save が route working copy を守る (Codex #334 round 2-4 review)', async ({ page }) => {
    await loadApp(page);

    await unlockPoiPosition(page); // 位置ロックは既定 ON (#333)
    await selectPoi(page, 'poi_wedge');
    const marker = page.locator(DRAGGABLE_ARROW);
    const box = await marker.boundingBox();
    const cx = box.x + box.width / 2;
    const cy = box.y + box.height / 2;
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 40, cy + 30, { steps: 8 });
    await page.mouse.up();

    // route を新規作成して OK (フォームは閉じるが Save routes はまだ = routeEditor.dirty)。
    await page.locator('#btn-add-route').click();
    await page.locator('#route-name').fill('未保存ルート2');
    await page.locator('#route-wp-select').selectOption('poi_wedge');
    await page.locator('#btn-add-waypoint').click();
    await page.locator('#btn-route-form-ok').click();
    await expect(page.locator('#route-edit-form')).toHaveClass(/(^|\s)hidden(\s|$)/);
    await expect(page.locator('#btn-save-routes')).toBeEnabled();

    // 通常の Undo ボタンは route.dirty 中でも即座に効く。
    await page.locator('#btn-undo').click();
    await expect(page.locator('#btn-save')).toBeDisabled();
    // だが route の未保存変更は消えない (loadRoutes() が skip された)。
    await expect(page.locator('#btn-save-routes')).toBeEnabled();

    // 再度 POI を dirty にして、今度は Ctrl+Z 経路も同様に確認。
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 40, cy + 30, { steps: 8 });
    await page.mouse.up();
    await expect(page.locator('#btn-save')).toBeEnabled();
    await page.keyboard.press('Control+z');
    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(page.locator('#btn-save-routes')).toBeEnabled(); // route 未保存変更は健在

    // さらに POI Save 経路 (round 4 で発覚): dirty にしてから実際に Save しても、
    // 未保存 route が上書きされない。
    await page.mouse.move(cx, cy);
    await page.mouse.down();
    await page.mouse.move(cx + 40, cy + 30, { steps: 8 });
    await page.mouse.up();
    await expect(page.locator('#btn-save')).toBeEnabled();
    await page.locator('#btn-save').click();
    await expect(page.locator('#btn-save')).toBeDisabled();
    await expect(page.locator('#btn-save-routes')).toBeEnabled();

    // route 側を Save すれば dirty が解消され、以後の POI undo/save でも通常どおり
    // loadRoutes() が走る (壊すものが無いので実害は無い)。
    await page.locator('#btn-save-routes').click();
    await expect(page.locator('#btn-save-routes')).toBeDisabled();
  });

  test('loadRoutes() を skip した間も route フォームの waypoint select は POI 最新名に追従する (Codex #334 round 5 review)', async ({ page }) => {
    await loadApp(page);

    // POI 名を変更 (formOk が _pushUndo → dirty)。
    await selectPoi(page, 'poi_wedge');
    await poiCard(page, 'poi_wedge').locator('.btn-edit').click();
    await page.locator('#poi-name').fill('poi_renamed');
    await page.locator('#btn-form-ok').click();
    await expect(page.locator('#btn-save')).toBeEnabled();

    // route フォームを開く (loadRoutes() は route.dirty 監視外だが editingIndex !== -1
    // なので、この後の POI undo で skip される)。select は開いた時点の名前 (poi_renamed)。
    await page.locator('#btn-add-route').click();
    await expect(page.locator('#route-wp-select')).toContainText('poi_renamed');

    // POI undo で名前を poi_wedge に戻す。loadRoutes() は skip されるが、waypoint
    // select は poiEditor.pois の最新名に repopulate されるべき (route working copy
    // 保護のための skip が stale UI を生んではいけない)。
    const floatUndo = page.locator('#btn-poi-undo-float');
    await expect(floatUndo).toBeVisible();
    await floatUndo.click();
    await expect(page.locator('#btn-save')).toBeDisabled();

    const options = await page.locator('#route-wp-select option').allTextContents();
    expect(options).toContain('poi_wedge');
    expect(options).not.toContain('poi_renamed');
  });
});
