# mapoi_webui Playwright e2e (browser smoke)

jsdom (vitest) では原理的に検証できない **Leaflet 直結部**と「**全 js 200 配信**」を、
headless chromium で smoke する層 (#284, #273 follow-up)。既存の vitest unit
(`../*.test.js`) とは別レイヤーで、テストファイルは `*.e2e.js`。

## 何を検証するか

- `all-js-200.e2e.js` — A: HTML の `<script>` 列挙 ⇔ `web/js/` 実ファイルの双方向整合、
  実ロードでの js/css 404 ゼロ、全 helper の global 定義 (「200 だが未定義」検知)。
- `poi-drag.e2e.js` — B: 選択中 POI marker だけ draggable (`leaflet-marker-draggable`)、
  ドラッグで dirty 化、route 編集中はドラッグ不可・Cancel で復帰。
- `yaw-handle.e2e.js` — C: 選択中 wedge POI に yaw ハンドル 1 個 / disc では無し、
  ハンドル drag で矢印 rotation が追従し dirty 化。
- `section-collapse.e2e.js` — D: 編集フォーム開で nav/tag/route と poi-list が畳まれ、
  閉じると「元の開閉状態」へ復元 (全開ではない)。

## サーバ構成 (ROS 非依存)

`server/mock_server.py` (Python 標準ライブラリのみ) が実 `web/` を静的配信し、`/api/*` を
`fixtures/state.json` 由来の決定論的 JSON で返す。実 Flask backend (`mapoi_webui_node.py`) は
import 時に rclpy / mapoi_interfaces を要し CI が重く、フィクスチャも制御できないため再利用しない。
map 画像は zlib で動的生成し、バイナリを git に置かない。Playwright の `webServer` が自動起動する。

> 配信元は source `web/`。CMakeLists が `install(DIRECTORY web)` で web/ を丸ごと配信するため
> per-file packaging drift は原理上起きず、source を試すのが正 (#284)。

## ローカル実行

`mapoi_webui/tests/web/` で:

```bash
npm ci                                    # 依存導入 (@playwright/test 含む)
npx playwright install --with-deps chromium  # 初回のみ: ブラウザバイナリ
npm run test:e2e                          # = playwright test (webServer 自動起動)
```

前提: Node 20 系、`python3` が PATH 上にあること (ROS source は不要)。
Leaflet は `web/vendor/leaflet/` に同梱しているため外部ネット接続不要。

デバッグ: `npx playwright test --headed` / `--debug` / `--ui`。
ポート衝突時は `MOCK_PORT=8801 npm run test:e2e`。

## CI

`.github/workflows/consistency-check.yml` の `webui_e2e` ジョブ (vitest の `webui_js_tests` と並走)。
headless chromium のため GL/GPU 不要 (#229 の GL 問題とは別レイヤー)。
