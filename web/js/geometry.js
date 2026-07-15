/**
 * Pure geometry helpers for mapoi WebUI.
 *
 * Browser からは `MapoiGeometry.offsetPointsPx` 等のグローバルとして使い、
 * Node (vitest) からは `module.exports` 経由で import する dual export 形式。
 * 依存ゼロの pure 関数で、Leaflet / DOM / window 等を参照しない。
 *
 * 単体テスト想定: mapoi_webui/tests/web/ 配下の vitest テストから直接呼び出す。
 */
(function () {
  'use strict';

  /**
   * Compute parallel-offset polyline in pixel space.
   *
   * **Direction-independent (canonical) normal**: 各 segment の 2 端点を lex-order
   * (x, y 辞書順) で並べ、その方向の 90° 時計回り回転を法線とする。これにより
   * A→B route と B→A route が同じ segment を共有しても法線方向が一致し、
   * `offsetPx` を符号付きで分けるだけで両 route が同じ canonical 軸の反対側に
   * 並行配置される (#69 PR-2)。
   *
   * 各 vertex で前後 segment 法線 (nIn / nOut) の和 = bisector を取り、
   * `bisector × (2*offsetPx / |bisector|^2)` で位置を求める。直線・浅い角度では
   * perpendicular 距離 `offsetPx` を保つが、鋭角で `|bisector|` が小さくなると
   * factor が blow up するため offset 距離を `4*|offsetPx|` でキャップ。**キャップ
   * 域では「`offsetPx` の perpendicular 距離」保証は崩れる**点に注意: 視覚分離の
   * 優先度を「コーナーで線同士が刺さらない」 > 「厳密な等距離」と割り切った設計。
   *
   * 端点 (i=0 / i=last) は片側 segment のみで normal を直接使う。退化 segment
   * (length < epsilon) は normal を null とし、隣接 normal にフォールバック。
   *
   * **anti-parallel fallback** (`bLen < epsilon`): canonical normal 化後では
   * 通常の vertex で nIn = -nOut になる組み合わせは構築できない (lex-order
   * canonical 方向は両 segment で一致するため)。fallback コードは degenerate な
   * 数値誤差や将来の normal 計算仕様変更に対する保険として残す。
   *
   * @param {Array<{x: number, y: number}>} points - 入力経路 (pixel 空間)
   * @param {number} offsetPx - 正負で canonical 法線に対する offset 方向、
   *                            0 なら no-op で同じ参照を返す
   * @returns {Array<{x: number, y: number}>}
   */
  function offsetPointsPx(points, offsetPx) {
    if (!points || points.length < 2 || offsetPx === 0) return points;
    const epsilon = 1e-3;
    const segNormals = [];
    for (let i = 0; i < points.length - 1; i++) {
      const a = points[i];
      const b = points[i + 1];
      const aFirst = a.x < b.x || (a.x === b.x && a.y < b.y);
      const dx = aFirst ? b.x - a.x : a.x - b.x;
      const dy = aFirst ? b.y - a.y : a.y - b.y;
      const len = Math.hypot(dx, dy);
      // canonical 方向の 90° 時計回り回転 = (dy, -dx) / len
      segNormals.push(len < epsilon ? null : { x: dy / len, y: -dx / len });
    }
    const maxOffset = 4 * Math.abs(offsetPx);
    return points.map((p, i) => {
      const nIn = i > 0 ? segNormals[i - 1] : null;
      const nOut = i < points.length - 1 ? segNormals[i] : null;
      if (nIn && nOut) {
        const bx = nIn.x + nOut.x;
        const by = nIn.y + nOut.y;
        const bLen = Math.hypot(bx, by);
        if (bLen < epsilon) {
          // anti-parallel fallback (canonical normal 化後では通常到達しない)
          return { x: p.x + nOut.x * offsetPx, y: p.y + nOut.y * offsetPx };
        }
        let factor = (2 * offsetPx) / (bLen * bLen);
        const offMag = Math.abs(factor) * bLen;
        if (offMag > maxOffset) {
          factor *= maxOffset / offMag;
        }
        return { x: p.x + bx * factor, y: p.y + by * factor };
      }
      const single = nIn || nOut;
      if (!single) return { x: p.x, y: p.y }; // 完全退化
      return { x: p.x + single.x * offsetPx, y: p.y + single.y * offsetPx };
    });
  }

  const api = { offsetPointsPx };

  if (typeof window !== 'undefined') {
    window.MapoiGeometry = window.MapoiGeometry || {};
    Object.assign(window.MapoiGeometry, api);
  }
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
}());
