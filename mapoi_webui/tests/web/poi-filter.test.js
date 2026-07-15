// vitest unit tests for the tolerance / rounding helpers in
// mapoi_webui/web/js/poi-filter.js (#138 / #242 / #267).
//
// 対象: MapoiPoiFilter.{parseTolerance, degToRad, radToDeg, formatToleranceYawDeg, roundTo}
//   + 桁数定数 (POSE_*_DIGITS / TOLERANCE_*_DIGITS / TOLERANCE_MIN)。
//
// テスト方針 (#193 PR2 で致命核に集約):
// - tolerance の clamp / 桁数丸め / deg↔rad round-trip は yaml データ整合の致命核。
//   backend yaml_handler._round_poi_floats (test_yaml_handler_round.py) と桁数契約を
//   両層で揃え、編集なし save でも churn しないことを pin する。
// - landmark filter / POI tag validation 等の dropdown 出し分け UI 系 test は致命核外
//   として削除した (#193 PR2)。DOM / window 依存ゼロの pure JS。

import { describe, it, expect } from 'vitest';
import * as filter from '../../web/js/poi-filter.js';

const {
  parseTolerance,
  degToRad,
  radToDeg,
  formatToleranceYawDeg,
  matchesPoiName,
  formatCursorCoords,
  roundTo,
  TOLERANCE_MIN,
  POSE_XY_DIGITS,
  POSE_YAW_DIGITS,
  TOLERANCE_XY_DIGITS,
  TOLERANCE_YAW_DIGITS,
} = filter;

describe('degToRad / radToDeg', () => {
  it('round-trips deg → rad → deg (#138)', () => {
    [0.1, 1, 45, 90, 180, 359].forEach((deg) => {
      expect(radToDeg(degToRad(deg))).toBeCloseTo(deg, 6);
    });
  });
});

describe('formatToleranceYawDeg (#267)', () => {
  // editor は rad を保存し度数で表示する。保存値は roundTo(degToRad(deg), 4) なので、
  // 区切りの良い度数は綺麗に表示され編集なし save でも churn しないことを pin する。
  it('round-trips round degrees without drift (deg → rad(4桁) → deg)', () => {
    [30, 45, 90, 120, 180].forEach((deg) => {
      const storedRad = roundTo(degToRad(deg), TOLERANCE_YAW_DIGITS);
      const shownDeg = formatToleranceYawDeg(storedRad);
      const resavedRad = roundTo(degToRad(shownDeg), TOLERANCE_YAW_DIGITS);
      expect(resavedRad).toBe(storedRad);
    });
  });
});

describe('formatCursorCoords (#381)', () => {
  it('formats with fixed POSE_XY_DIGITS decimals (幅が揺れない)', () => {
    // toFixed 固定桁: 末尾ゼロを保持し、mousemove 中の文字幅ちらつきを防ぐ
    expect(formatCursorCoords(1.2, -3.45678)).toBe('x: 1.200, y: -3.457');
    expect(formatCursorCoords(0, 0)).toBe('x: 0.000, y: 0.000');
  });

  it('returns empty string for non-finite input (呼び出し側は readout を隠す)', () => {
    expect(formatCursorCoords(NaN, 1)).toBe('');
    expect(formatCursorCoords(1, Infinity)).toBe('');
    expect(formatCursorCoords(undefined, 1)).toBe('');
  });
});

describe('roundTo (#242)', () => {
  it('exposes save-path digit constants', () => {
    // backend yaml_handler._POSE_XY_DIGITS 等と必ず一致させる (両層 defense-in-depth)
    expect(POSE_XY_DIGITS).toBe(3);
    expect(POSE_YAW_DIGITS).toBe(4);
    expect(TOLERANCE_XY_DIGITS).toBe(3);
    expect(TOLERANCE_YAW_DIGITS).toBe(4);
  });

  it('rounds finite numbers to the requested digits (yaml churn 防止の核)', () => {
    // 実 float 演算で「保存桁数に丸めれば clean な値に収束する」= 編集なし save で
    // yaml が churn しない frontend 側の保証。桁数定数 (上) だけでなく丸め演算自体を pin
    // しないと、poi-editor の drag→roundTo 委譲経路の整合 (= 致命核 yaml round-trip) が
    // どこにも残らない (backend _round_poi_floats は別実装の defense-in-depth)。
    expect(roundTo(0.10000038482226709, POSE_XY_DIGITS)).toBe(0.1);
    expect(roundTo(3.1399991679827224, POSE_YAW_DIGITS)).toBe(3.14);
    expect(roundTo(0.7850002283279937, TOLERANCE_YAW_DIGITS)).toBe(0.785);
    expect(roundTo(1.2345, 2)).toBe(1.23);
    expect(roundTo(1.2355, 2)).toBe(1.24);
  });
});

describe('matchesPoiName (#383)', () => {
  // POI list 検索の絞り込み判定。card 生成 skip の是非を決める唯一の関数なので、
  // 大文字小文字不区別・部分一致・空 query 全件 match をここで pin する
  // (list DOM との index 整合は e2e poi-search.e2e.js 側)。
  it('matches case-insensitive substrings of the name', () => {
    const poi = { name: 'Kitchen_Door' };
    expect(matchesPoiName(poi, 'door')).toBe(true);
    expect(matchesPoiName(poi, 'KITCHEN')).toBe(true);
    expect(matchesPoiName(poi, 'hen_d')).toBe(true);
    expect(matchesPoiName(poi, 'dining')).toBe(false);
  });

  it('empty / whitespace-only / null query matches everything (絞り込みなし)', () => {
    expect(matchesPoiName({ name: 'a' }, '')).toBe(true);
    expect(matchesPoiName({ name: 'a' }, '   ')).toBe(true);
    expect(matchesPoiName({ name: 'a' }, null)).toBe(true);
    expect(matchesPoiName({}, undefined)).toBe(true);
  });

  it('unnamed POI does not match a non-empty query', () => {
    expect(matchesPoiName({}, 'a')).toBe(false);
    expect(matchesPoiName({ name: '' }, 'a')).toBe(false);
    expect(matchesPoiName(null, 'a')).toBe(false);
  });
});

describe('parseTolerance', () => {
  it('exposes msg-spec min value (#138)', () => {
    expect(TOLERANCE_MIN).toBe(0.001);
  });

  it('clamps xy < 0.001 to TOLERANCE_MIN (#138 spec)', () => {
    // 0 / 負値 / 0 寄り は無反応 POI を作るため明示的に min に補正
    expect(parseTolerance('0', 0.785)).toEqual({ xy: 0.001, yaw: 0.785 });
    expect(parseTolerance('-0.5', 0.785)).toEqual({ xy: 0.001, yaw: 0.785 });
    expect(parseTolerance('0.0005', 0.785)).toEqual({ xy: 0.001, yaw: 0.785 });
  });

  it('clamps yaw < 0.001 to TOLERANCE_MIN (#138 spec)', () => {
    expect(parseTolerance('0.5', 0)).toEqual({ xy: 0.5, yaw: 0.001 });
    expect(parseTolerance('0.5', -0.1)).toEqual({ xy: 0.5, yaw: 0.001 });
    expect(parseTolerance('0.5', 0.0005)).toEqual({ xy: 0.5, yaw: 0.001 });
  });

  it('falls back to TOLERANCE_MIN when input is empty / non-numeric / NaN', () => {
    expect(parseTolerance('', undefined)).toEqual({ xy: 0.001, yaw: 0.001 });
    expect(parseTolerance('abc', null)).toEqual({ xy: 0.001, yaw: 0.001 });
    expect(parseTolerance(NaN, NaN)).toEqual({ xy: 0.001, yaw: 0.001 });
  });

  it('preserves valid values above TOLERANCE_MIN', () => {
    expect(parseTolerance('0.5', 0.785)).toEqual({ xy: 0.5, yaw: 0.785 });
    expect(parseTolerance(0.42, Math.PI / 6)).toEqual({ xy: 0.42, yaw: Math.PI / 6 });
  });

  it('handles deg-converted yaw values (typical UI flow)', () => {
    // UI で 45° 入力 → degToRad(45) → parseTolerance に渡す流れ
    const yawRad = degToRad(45);
    const result = parseTolerance('0.5', yawRad);
    expect(result.xy).toBe(0.5);
    expect(result.yaw).toBeCloseTo(Math.PI / 4, 6);
  });
});
