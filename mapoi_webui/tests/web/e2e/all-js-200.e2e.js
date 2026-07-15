// A. 全 js 200 smoke (#284, #273 follow-up)。
// 配信欠落・読み込み順崩れ・実行時 404 の即時検知。回帰防止の本丸。
//
// 検知対象: (i) HTML の <script> 列挙 ⇔ web/js/ 実ファイルの双方向整合、
//           (ii) ブラウザ実ロードでの js/css 404 / JS 実行エラー、(iii) 全 helper の global 定義。
// 配信元は source web/ (CMakeLists が install(DIRECTORY web) で web/ 丸ごと配信するため
// per-file packaging drift は原理上起きない → source を試すのが正)。
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

// e2e/ から repo の web/js/ へ: e2e -> tests/web -> tests -> mapoi_webui -> web/js
const WEB_JS_DIR = path.resolve(__dirname, '../../../web/js');

// 各 helper が window もしくは lexical global として load 後に定義されているか。
// 注: const/class の top-level 宣言は window プロパティにならないため bare 名 + typeof で見る。
const EXPECTED_GLOBALS = [
  'MapoiApi', 'MapoiGeometry', 'MapoiPoiFilter', 'MapoiPoiInteractions',
  'MapoiNavControls', 'MapViewer', 'PoiEditor', 'RouteEditor',
];

test.describe('A. 全 js 200 smoke', () => {
  test('全 /js・/css が 200 で配信され、HTML⇔実ファイルが整合し、JS 実行エラーが無い', async ({ page }) => {
    const badResponses = [];
    page.on('response', (res) => {
      const p = new URL(res.url()).pathname;
      if ((p.startsWith('/js/') || p.startsWith('/css/') || p.startsWith('/vendor/')) && res.status() >= 400) {
        badResponses.push(`${res.status()} ${p}`);
      }
    });
    const consoleErrors = [];
    page.on('console', (msg) => {
      // SSE の onerror は console.warn なので error のみ拾う。
      if (msg.type() === 'error') consoleErrors.push(msg.text());
    });
    const pageErrors = [];
    page.on('pageerror', (err) => pageErrors.push(err.message));

    const indexResp = await page.goto('/');
    expect(indexResp.status()).toBe(200);
    // 初期化 (loadMaps→…→showPois) 完了の合図として POI marker 描画を待つ。
    await expect(page.locator('.poi-arrow-icon')).toHaveCount(5);

    // --- HTML が列挙する /js/*.js を抽出し各々 200 ---
    const htmlJs = await page.$$eval('script[src^="/js/"]', (els) =>
      els.map((e) => new URL(e.src).pathname));
    expect(htmlJs.length).toBeGreaterThan(0);
    for (const p of htmlJs) {
      const r = await page.request.get(p);
      expect(r.status(), `${p} は 200 であるべき`).toBe(200);
    }
    expect((await page.request.get('/css/style.css')).status()).toBe(200);
    expect((await page.request.get('/')).status()).toBe(200);

    // --- HTML 集合 ⇔ web/js/ 実ファイル集合 の双方向整合 ---
    const htmlSet = new Set(htmlJs.map((p) => p.replace(/^\/js\//, '')));
    const diskSet = new Set(fs.readdirSync(WEB_JS_DIR).filter((f) => f.endsWith('.js')));
    const missingFromHtml = [...diskSet].filter((f) => !htmlSet.has(f)); // disk にあるが HTML 未参照
    const missingFromDisk = [...htmlSet].filter((f) => !diskSet.has(f)); // HTML 参照だが disk になし
    expect(missingFromHtml, 'web/js/ にあるが index.html が読み込んでいない js').toEqual([]);
    expect(missingFromDisk, 'index.html が読むが web/js/ に存在しない js').toEqual([]);

    // --- 全 helper が load 後に global 定義済み (200 だが未定義 を間接検知) ---
    const undefinedGlobals = await page.evaluate((names) =>
      names.filter((n) => {
        // bare 名で typeof を見る (window 非依存)。
        try { return eval(`typeof ${n}`) === 'undefined'; } catch (_e) { return true; }
      }), EXPECTED_GLOBALS);
    expect(undefinedGlobals, 'load 後に未定義の helper global').toEqual([]);

    // --- 実ロードで js/css の 4xx/5xx ゼロ・JS 実行エラーゼロ ---
    expect(badResponses, 'js/css の 4xx/5xx 応答').toEqual([]);
    expect(pageErrors, 'pageerror (未捕捉例外)').toEqual([]);
    expect(consoleErrors, 'console.error').toEqual([]);
  });
});
