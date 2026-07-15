// vitest unit tests for the pure geometry helpers in
// mapoi_webui/web/js/geometry.js (#129).
//
// 対象: MapoiGeometry.offsetPointsPx (parallel-offset polyline in pixel space)
//
// テスト方針:
// - ロジックの幾何的不変条件 (perpendicular 距離、canonical 法線、cap、退化、
//   no-op) を確認することで、map-viewer.js の `_offsetLatLngs` が将来 regress
//   しても CI で検出できるようにする。
// - Leaflet / DOM / window への依存はゼロで pure JS のみ。

import { describe, it, expect } from 'vitest';
import * as geom from '../../web/js/geometry.js';

const { offsetPointsPx } = geom;

const EPS = 1e-9;

function nearlyEqual(a, b, tol = 1e-6) {
  return Math.abs(a - b) < tol;
}

function distancePointToLine(p, a, b) {
  // |(b-a) x (a-p)| / |b-a|
  const ax = a.x;
  const ay = a.y;
  const bx = b.x;
  const by = b.y;
  const numer = Math.abs((bx - ax) * (ay - p.y) - (ax - p.x) * (by - ay));
  const denom = Math.hypot(bx - ax, by - ay);
  if (denom < EPS) return Number.NaN;
  return numer / denom;
}

describe('offsetPointsPx', () => {
  it('returns the same reference when offsetPx === 0 (no-op)', () => {
    const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }];
    expect(offsetPointsPx(pts, 0)).toBe(pts);
  });

  it('returns the same reference for fewer than 2 points', () => {
    const empty = [];
    expect(offsetPointsPx(empty, 5)).toBe(empty);
    const single = [{ x: 1, y: 1 }];
    expect(offsetPointsPx(single, 5)).toBe(single);
  });

  it('returns the same reference for null / undefined input', () => {
    expect(offsetPointsPx(null, 5)).toBeNull();
    expect(offsetPointsPx(undefined, 5)).toBeUndefined();
  });

  it('produces perpendicular distance offsetPx for a horizontal segment', () => {
    const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }];
    const out = offsetPointsPx(pts, 5);
    expect(out).toHaveLength(2);
    // canonical direction: (0,0) → (10,0) (lex order先頭は (0,0))。
    // 90° CW rotation = (0, -1) なので offsetPx=+5 で y が -5 に動く。
    expect(nearlyEqual(out[0].x, 0)).toBe(true);
    expect(nearlyEqual(out[0].y, -5)).toBe(true);
    expect(nearlyEqual(out[1].x, 10)).toBe(true);
    expect(nearlyEqual(out[1].y, -5)).toBe(true);
  });

  it('reverses offset direction for negative offsetPx', () => {
    const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }];
    const out = offsetPointsPx(pts, -5);
    expect(nearlyEqual(out[0].y, 5)).toBe(true);
    expect(nearlyEqual(out[1].y, 5)).toBe(true);
  });

  it('uses canonical (lex-order) normal so reversed routes share the same offset axis', () => {
    // A→B route と B→A route で同じ offsetPx を渡したとき、両者は同じ方向に offset される
    // (canonical 法線が同じため)。これにより呼び出し側 (showRoutes) が offsetPx の符号で
    // 分けるだけで両 route が反対側に並行配置される。
    const forward = [{ x: 0, y: 0 }, { x: 10, y: 0 }];
    const reverse = [{ x: 10, y: 0 }, { x: 0, y: 0 }];
    const forwardOut = offsetPointsPx(forward, 5);
    const reverseOut = offsetPointsPx(reverse, 5);
    // 両者の y はどちらも -5 (canonical normal が同じ)
    expect(nearlyEqual(forwardOut[0].y, -5)).toBe(true);
    expect(nearlyEqual(forwardOut[1].y, -5)).toBe(true);
    expect(nearlyEqual(reverseOut[0].y, -5)).toBe(true);
    expect(nearlyEqual(reverseOut[1].y, -5)).toBe(true);
    // 逆向き route 側の符号を反転すると反対側に配置される
    const reverseOpp = offsetPointsPx(reverse, -5);
    expect(nearlyEqual(reverseOpp[0].y, 5)).toBe(true);
    expect(nearlyEqual(reverseOpp[1].y, 5)).toBe(true);
  });

  it('keeps perpendicular distance offsetPx from each segment at 90° corners', () => {
    // L 字: (0,0) → (10,0) → (10,10)
    const pts = [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }];
    const offsetPx = 5;
    const out = offsetPointsPx(pts, offsetPx);
    // 端点 0 (segment 0 のみ): perpendicular 距離が segment(pts[0]→pts[1]) で offsetPx
    expect(nearlyEqual(distancePointToLine(out[0], pts[0], pts[1]), offsetPx)).toBe(true);
    // 端点 2 (segment 1 のみ): perpendicular 距離が segment(pts[1]→pts[2]) で offsetPx
    expect(nearlyEqual(distancePointToLine(out[2], pts[1], pts[2]), offsetPx)).toBe(true);
    // 中点 (corner): bisector 計算により両 segment との perpendicular 距離が両方 offsetPx
    expect(nearlyEqual(distancePointToLine(out[1], pts[0], pts[1]), offsetPx)).toBe(true);
    expect(nearlyEqual(distancePointToLine(out[1], pts[1], pts[2]), offsetPx)).toBe(true);
  });

  it('caps the offset magnitude at 4 * |offsetPx| for sharp corners', () => {
    // canonical 法線が大きく異なる入力で bisector を短くする V 字: 縦長の山
    // (0,0) → (1,1000) → (2,0)。
    // canonical seg0: (0,0)→(1,1000) で normal ≈ (1, -1e-3)
    // canonical seg1: (1,1000)→(2,0)   で normal ≈ (-1, -1e-3) (canonical は lex order で
    //   (1,1000) → (2,0)、direction (1,-1000)/|·| → normal (-1000,-1)/|·| ≈ (-1, -1e-3))
    // bisector ≈ (0, -2e-3) → bLen ≈ 2e-3、uncapped factor = 2*offsetPx/bLen² ≈ 1.5e6
    // → uncapped offset 距離は ~3000、これを 4*|offsetPx|=12 にキャップする branch が走る。
    const pts = [{ x: 0, y: 0 }, { x: 1, y: 1000 }, { x: 2, y: 0 }];
    const offsetPx = 3;
    const out = offsetPointsPx(pts, offsetPx);
    const dx = out[1].x - pts[1].x;
    const dy = out[1].y - pts[1].y;
    const cornerOffset = Math.hypot(dx, dy);
    const cap = 4 * Math.abs(offsetPx);
    // cap が無いと ~3000 になる。cap=12 に張り付く (epsilon は数値誤差用)
    expect(cornerOffset).toBeLessThanOrEqual(cap + 1e-6);
    expect(cornerOffset).toBeGreaterThan(cap - 1e-6);
  });

  it('falls back to neighbor normal when a segment is degenerate', () => {
    // 退化セグメント (length≈0) を中央に挟む: (0,0) → (5,0) → (5,0) → (10,0)
    // pts[1]==pts[2] の segment 1 は length≈0 → segNormal[1] = null
    // 結果として中央 vertex は隣接 normal を使う。全体は直線として offset される。
    const pts = [
      { x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 0 }, { x: 10, y: 0 },
    ];
    const out = offsetPointsPx(pts, 4);
    // 全 vertex が y=-4 (canonical normal (0,-1)) に近いことを確認
    out.forEach((p) => {
      expect(nearlyEqual(p.y, -4, 1e-3)).toBe(true);
    });
  });

  it('returns input position for fully degenerate segment pair (no valid normal)', () => {
    // 全ての隣接 segment が degenerate な極端ケース: 同点 3 つ
    const pts = [{ x: 1, y: 1 }, { x: 1, y: 1 }, { x: 1, y: 1 }];
    const out = offsetPointsPx(pts, 5);
    // どの vertex も normal が null → 原点 (元位置) 維持
    expect(out).toHaveLength(3);
    out.forEach((p) => {
      expect(nearlyEqual(p.x, 1)).toBe(true);
      expect(nearlyEqual(p.y, 1)).toBe(true);
    });
  });

  it('treats segments shorter than epsilon (1e-3) as degenerate, not above', () => {
    // epsilon = 1e-3 は内部 const。境界条件を仕様として固定するために、短い
    // segment が **非共線 (kink)** な配置で、degenerate 扱いか normal 計算かで
    // 出力位置が観測できる差になるよう設計する (Codex round 2 low 対応)。
    //
    // 共通形状: 水平 → 短い垂直 → 水平 (L 字の小さい段差)。
    //
    // (a) 1e-4 の vertical segment: epsilon 未満で **degenerate**。中央 vertex は
    //   隣接 (水平 segment、normal=(0,-1)) にフォールバックする。
    //   水平 segment が両側で支配的なため offset 後 x は不変、y のみ offsetPx 分シフト。
    {
      const offsetPx = 4;
      const pts = [
        { x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 1e-4 }, { x: 10, y: 1e-4 },
      ];
      const out = offsetPointsPx(pts, offsetPx);
      // 全 vertex が x 不変、y は元値 - offsetPx
      expect(nearlyEqual(out[1].x, 5)).toBe(true);
      expect(nearlyEqual(out[1].y, -4)).toBe(true);
      expect(nearlyEqual(out[2].x, 5)).toBe(true);
      expect(nearlyEqual(out[2].y, 1e-4 - 4)).toBe(true);
    }

    // (b) 1e-2 の vertical segment: epsilon 超で **通常 normal**。中央 vertex は
    //   nIn=(0,-1) (水平) + nOut=(1,0) (垂直、canonical 方向 lex order で y 増加方向)
    //   の bisector を使う → factor=2*offsetPx/bLen²=2*4/2=4、offset=(4,-4)。
    //   結果として x が +4 動く ((a) と観測可能な差になる)。
    {
      const offsetPx = 4;
      const pts = [
        { x: 0, y: 0 }, { x: 5, y: 0 }, { x: 5, y: 1e-2 }, { x: 10, y: 1e-2 },
      ];
      const out = offsetPointsPx(pts, offsetPx);
      // vertex 1, 2 とも x が +4 動く (degenerate 扱いなら x は変わらないはず)
      expect(nearlyEqual(out[1].x, 9, 1e-3)).toBe(true);
      expect(nearlyEqual(out[1].y, -4, 1e-3)).toBe(true);
      expect(nearlyEqual(out[2].x, 9, 1e-3)).toBe(true);
      expect(nearlyEqual(out[2].y, 1e-2 - 4, 1e-3)).toBe(true);
    }
  });

  it('handles vertical segment (lex tie on x, ordered by y)', () => {
    // 同 x の縦線: (5, 0) → (5, 10)
    // canonical: (5,0) → (5,10) (y で lex 比較)、direction (0, 10)、normal (10, 0)/10 = (1, 0)
    // offsetPx=+3 で x が +3 に動く
    const pts = [{ x: 5, y: 0 }, { x: 5, y: 10 }];
    const out = offsetPointsPx(pts, 3);
    expect(nearlyEqual(out[0].x, 8)).toBe(true);
    expect(nearlyEqual(out[0].y, 0)).toBe(true);
    expect(nearlyEqual(out[1].x, 8)).toBe(true);
    expect(nearlyEqual(out[1].y, 10)).toBe(true);
  });
});
