// Playwright e2e (browser smoke) config for mapoi_webui frontend (#284).
//
// 既存の vitest unit (jsdom, *.test.js) とは別レイヤー。jsdom では原理的に
// 動かない Leaflet 直結部 (marker drag / yaw ハンドル DOM / section 折りたたみの
// app.js 実適用) と「全 js 200 配信」を、実ブラウザ (headless chromium) で smoke する。
//
// テストファイルは *.e2e.js とし、vitest の既定 include (*.test.js / *.spec.js) と
// 衝突させない (vitest config は変更不要のまま据え置く)。
//
// 配信は ROS 非依存の Python モックサーバ (e2e/server/mock_server.js は無く .py)。
// 実 web/ を静的配信し /api/* を fixture JSON で返すため、rclpy / ROS 環境なしで
// CI/ローカルとも動く。実 Flask backend (rclpy 依存) は再利用しない (#284 配信方針)。
const { defineConfig, devices } = require('@playwright/test');

const PORT = process.env.MOCK_PORT || '8799';
const BASE_URL = `http://127.0.0.1:${PORT}`;

module.exports = defineConfig({
  testDir: './e2e',
  testMatch: '**/*.e2e.js',
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 1 : 0,
  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : 'list',
  use: {
    baseURL: BASE_URL,
    viewport: { width: 1280, height: 800 },
    trace: 'on-first-retry',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'] },
      testIgnore: '**/sse-*.e2e.js',
    },
    // SSE 注入 (POST /test/sse-event) は接続中の全 client への broadcast で、並走中の
    // 他テストの page にも届いて reload を誘発し得る。chromium project の完了後に
    // 単独 (直列) で走らせて隔離する (#384)。
    {
      name: 'chromium-sse',
      use: { ...devices['Desktop Chrome'] },
      testMatch: '**/sse-*.e2e.js',
      dependencies: ['chromium'],
      fullyParallel: false,
    },
  ],
  webServer: {
    command: `python3 e2e/server/mock_server.py --port ${PORT}`,
    url: `${BASE_URL}/`,
    reuseExistingServer: !process.env.CI,
    timeout: 30000,
    stdout: 'pipe',
    stderr: 'pipe',
  },
});
