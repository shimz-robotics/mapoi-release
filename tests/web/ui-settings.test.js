// vitest unit tests for the pure UI 表示設定 helpers in
// mapoi_webui/web/js/ui-settings.js (#323 半透明化 / #324 UI オン/オフトグル)。
//
// 対象: MapoiUiSettings.{clampAlpha, alphaToPercent, percentToAlpha,
//   normalizeSettings, readSettings, writeSettings, applySettings}
//
// テスト方針: localStorage 永続 (read/write) と body への反映 (applySettings) は
//   「設定が壊れる / 既定が現行の見た目からずれる」に直結する核なので pin する。
//   alpha の clamp / % 往復は slider 値契約。private mode 等の storage 例外を
//   握り潰して既定に落ちることも確認する。DOM は fake object で表現し jsdom 非依存。
import { describe, it, expect } from 'vitest';
import * as ui from '../../web/js/ui-settings.js';

const {
  ALPHA_MIN,
  ALPHA_MAX,
  ALPHA_DEFAULT,
  STORAGE_KEYS,
  clampAlpha,
  alphaToPercent,
  percentToAlpha,
  normalizeSettings,
  readSettings,
  writeSettings,
  applySettings,
} = ui;

/** localStorage 互換の in-memory fake。throwing=true で全操作を投げる。 */
function makeStorage(initial, throwing) {
  const m = new Map(Object.entries(initial || {}));
  return {
    getItem(k) {
      if (throwing) throw new Error('blocked');
      return m.has(k) ? m.get(k) : null;
    },
    setItem(k, v) {
      if (throwing) throw new Error('blocked');
      m.set(k, String(v));
    },
    _map: m,
  };
}

/** body 要素の最小 fake (classList.toggle/contains + style.setProperty)。 */
function makeBody() {
  const classes = new Set();
  const props = {};
  return {
    classList: {
      toggle(name, on) {
        if (on) classes.add(name);
        else classes.delete(name);
        return classes.has(name);
      },
      contains: (n) => classes.has(n),
    },
    style: {
      setProperty(k, v) {
        props[k] = v;
      },
    },
    _classes: classes,
    _props: props,
  };
}

describe('clampAlpha', () => {
  it('範囲内はそのまま', () => {
    expect(clampAlpha(0.5)).toBe(0.5);
    expect(clampAlpha(ALPHA_MIN)).toBe(ALPHA_MIN);
    expect(clampAlpha(ALPHA_MAX)).toBe(ALPHA_MAX);
  });
  it('範囲外は clamp', () => {
    expect(clampAlpha(0)).toBe(ALPHA_MIN);
    expect(clampAlpha(-1)).toBe(ALPHA_MIN);
    expect(clampAlpha(2)).toBe(ALPHA_MAX);
  });
  it('数値化できない値は既定 1.0', () => {
    expect(clampAlpha('x')).toBe(ALPHA_DEFAULT);
    expect(clampAlpha(NaN)).toBe(ALPHA_DEFAULT);
    expect(clampAlpha(undefined)).toBe(ALPHA_DEFAULT);
  });
  it('数値文字列は解釈する', () => {
    expect(clampAlpha('0.6')).toBeCloseTo(0.6, 6);
  });
});

describe('alpha ⇔ percent', () => {
  it('alphaToPercent は 0-100 の整数', () => {
    expect(alphaToPercent(1)).toBe(100);
    expect(alphaToPercent(0.3)).toBe(30);
    expect(alphaToPercent(0.555)).toBe(56);
  });
  it('percentToAlpha は clamp 付きで割り戻す', () => {
    expect(percentToAlpha(100)).toBe(1);
    expect(percentToAlpha(30)).toBeCloseTo(0.3, 6);
    expect(percentToAlpha(0)).toBe(ALPHA_MIN); // 30% 未満は clamp
    expect(percentToAlpha('x')).toBe(ALPHA_DEFAULT);
  });
  it('slider 刻み (5%) の往復で値が保たれる', () => {
    for (let p = 30; p <= 100; p += 5) {
      expect(alphaToPercent(percentToAlpha(p))).toBe(p);
    }
  });
});

describe('normalizeSettings', () => {
  it('未指定は既定 (全 false / alpha=1)', () => {
    expect(normalizeSettings(undefined)).toEqual({ hidden: false, overlay: false, alpha: 1 });
    expect(normalizeSettings({})).toEqual({ hidden: false, overlay: false, alpha: 1 });
  });
  it('真偽は文字列/真値の両方を解釈', () => {
    expect(normalizeSettings({ hidden: 'true', overlay: '1' })).toMatchObject({ hidden: true, overlay: true });
    expect(normalizeSettings({ hidden: true, overlay: false })).toMatchObject({ hidden: true, overlay: false });
    expect(normalizeSettings({ hidden: 'false' })).toMatchObject({ hidden: false });
  });
  it('alpha は clamp される', () => {
    expect(normalizeSettings({ alpha: 5 }).alpha).toBe(ALPHA_MAX);
    expect(normalizeSettings({ alpha: 0 }).alpha).toBe(ALPHA_MIN);
  });
});

describe('readSettings', () => {
  it('storage=null は既定', () => {
    expect(readSettings(null)).toEqual({ hidden: false, overlay: false, alpha: ALPHA_DEFAULT });
  });
  it('保存値を読み出す', () => {
    const s = makeStorage({
      [STORAGE_KEYS.hidden]: 'true',
      [STORAGE_KEYS.overlay]: 'false',
      [STORAGE_KEYS.alpha]: '0.5',
    });
    expect(readSettings(s)).toEqual({ hidden: true, overlay: false, alpha: 0.5 });
  });
  it('欠損キーは既定で補完', () => {
    const s = makeStorage({ [STORAGE_KEYS.alpha]: '0.7' });
    expect(readSettings(s)).toEqual({ hidden: false, overlay: false, alpha: 0.7 });
  });
  it('壊れた alpha は clamp/既定に落ちる', () => {
    expect(readSettings(makeStorage({ [STORAGE_KEYS.alpha]: 'garbage' })).alpha).toBe(ALPHA_DEFAULT);
    expect(readSettings(makeStorage({ [STORAGE_KEYS.alpha]: '0.01' })).alpha).toBe(ALPHA_MIN);
  });
  it('storage が例外を投げても既定で握り潰す', () => {
    expect(readSettings(makeStorage({}, true))).toEqual({
      hidden: false,
      overlay: false,
      alpha: ALPHA_DEFAULT,
    });
  });
});

describe('writeSettings', () => {
  it('正規化した文字列を書き込む', () => {
    const s = makeStorage({});
    writeSettings(s, { hidden: true, overlay: false, alpha: 0.6 });
    expect(s._map.get(STORAGE_KEYS.hidden)).toBe('true');
    expect(s._map.get(STORAGE_KEYS.overlay)).toBe('false');
    expect(s._map.get(STORAGE_KEYS.alpha)).toBe('0.6');
  });
  it('storage 例外は握り潰す (throw しない)', () => {
    expect(() => writeSettings(makeStorage({}, true), { hidden: true, alpha: 1 })).not.toThrow();
    expect(() => writeSettings(null, { hidden: true, alpha: 1 })).not.toThrow();
  });
  it('read→write の往復で値が保たれる', () => {
    const s = makeStorage({});
    const original = { hidden: true, overlay: true, alpha: 0.45 };
    writeSettings(s, original);
    expect(readSettings(s)).toEqual({ hidden: true, overlay: true, alpha: 0.45 });
  });
});

describe('applySettings', () => {
  it('hidden/overlay class を反映し alpha を CSS 変数に設定', () => {
    const body = makeBody();
    applySettings(body, { hidden: true, overlay: false, alpha: 0.5 });
    expect(body._classes.has('ui-hidden')).toBe(true);
    expect(body._classes.has('ui-overlay')).toBe(false);
    expect(body._props['--ui-panel-alpha']).toBe('0.5');
  });
  it('既定 (alpha=1, 全 false) では class 無し・変数 1', () => {
    const body = makeBody();
    applySettings(body, {});
    expect(body._classes.size).toBe(0);
    expect(body._props['--ui-panel-alpha']).toBe('1');
  });
  it('再適用で class が正しく off に戻る', () => {
    const body = makeBody();
    applySettings(body, { hidden: true, overlay: true, alpha: 0.4 });
    applySettings(body, { hidden: false, overlay: false, alpha: 1 });
    expect(body._classes.has('ui-hidden')).toBe(false);
    expect(body._classes.has('ui-overlay')).toBe(false);
    expect(body._props['--ui-panel-alpha']).toBe('1');
  });
  it('body=null は no-op (throw しない)', () => {
    expect(() => applySettings(null, { hidden: true })).not.toThrow();
  });
});
