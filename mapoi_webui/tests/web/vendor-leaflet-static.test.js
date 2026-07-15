// #394: Leaflet vendoring の静的回帰チェック。
// - index.html に CDN 参照 (unpkg 等) が再混入していないこと (オフライン動作の release blocker 再発防止)
// - vendor/leaflet の必須ファイルが揃っていること (CMake は install(DIRECTORY web) で丸ごと配布する
//   ため per-file の install 漏れは起きないが、git への追加漏れ・誤削除はここで検知する)
// - leaflet.js の sourceMappingURL が指す map が同梱されていること (DevTools での 404 防止)
import { describe, it, expect } from 'vitest';
import { readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const webDir = join(dirname(fileURLToPath(import.meta.url)), '../../web');
const vendorDir = join(webDir, 'vendor/leaflet');

describe('leaflet vendoring (#394)', () => {
  it('index.html has no CDN references and points at the vendored files', () => {
    const html = readFileSync(join(webDir, 'index.html'), 'utf8');
    expect(html).not.toMatch(/unpkg\.com|cdnjs|jsdelivr|leaflet@/);
    expect(html).toContain('/vendor/leaflet/leaflet.css');
    expect(html).toContain('/vendor/leaflet/leaflet.js');
  });

  it('vendor/leaflet ships all required files', () => {
    const required = [
      'leaflet.js',
      'leaflet.js.map',
      'leaflet.css',
      'LICENSE',
      'images/marker-icon.png',
      'images/marker-icon-2x.png',
      'images/marker-shadow.png',
      'images/layers.png',
      'images/layers-2x.png',
    ];
    for (const f of required) {
      expect(existsSync(join(vendorDir, f)), `missing: ${f}`).toBe(true);
    }
  });

  it('sourceMappingURL in leaflet.js resolves to a shipped file', () => {
    const js = readFileSync(join(vendorDir, 'leaflet.js'), 'utf8');
    const m = js.match(/sourceMappingURL=(\S+)/);
    expect(m).not.toBeNull();
    expect(existsSync(join(vendorDir, m[1])), `missing map: ${m[1]}`).toBe(true);
  });
});
