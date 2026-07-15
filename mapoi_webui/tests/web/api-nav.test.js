// MapoiApi の navigation 系 fetch 契約 (#199)。
// navSwitchMap / mode が叩く endpoint・method・body・戻り値を、global fetch を
// stub して固定する。DOM 非依存なので default (node) 環境で回す。
// backend 側の validation は test_api_nav_endpoints.py (#279) が pin 済で、ここは
// client が「正しい URL に正しい payload を投げ、JSON をそのまま返す」ことだけを見る。
//
// saveRoutes / saveCustomTags の楽観的競合検出 (#241 の展開, #343) も同じファイルで
// pin する: backend 側の version check 自体は test_api_save_pois_version_conflict.py
// (TestApiSaveRoutesVersionConflict / TestApiSaveCustomTagsVersionConflict) が持つので、
// ここでは client が expected_version を送り、{ ok, status, conflict } を正しく組み立てる
// ことだけを見る。

import { describe, it, expect, vi, afterEach } from 'vitest';
import * as MapoiApi from '../../web/js/api.js';

afterEach(() => {
  vi.restoreAllMocks();
  delete globalThis.fetch;
});

function mockFetch(payload, { status = 200, ok = status < 400 } = {}) {
  const fetchMock = vi.fn().mockResolvedValue({
    ok,
    status,
    json: vi.fn().mockResolvedValue(payload),
  });
  globalThis.fetch = fetchMock;
  return fetchMock;
}

describe('MapoiApi.navSwitchMap', () => {
  it('POST /api/nav/switch-map に map_name を JSON body で送る', async () => {
    const fetchMock = mockFetch({ ok: true });
    const res = await MapoiApi.navSwitchMap('floor1');

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, opts] = fetchMock.mock.calls[0];
    expect(url).toBe('/api/nav/switch-map');
    expect(opts.method).toBe('POST');
    expect(opts.headers).toMatchObject({ 'Content-Type': 'application/json' });
    expect(JSON.parse(opts.body)).toEqual({ map_name: 'floor1' });
    expect(res).toEqual({ ok: true });
  });

  it('backend の warning/error 応答をそのまま透過する', async () => {
    mockFetch({ warning: 'no subscriber' });
    expect(await MapoiApi.navSwitchMap('floor1')).toEqual({ warning: 'no subscriber' });
  });
});

describe('MapoiApi.mode', () => {
  it('GET /api/mode を叩き JSON をそのまま返す', async () => {
    const fetchMock = mockFetch({ mode: 'navigation' });
    const res = await MapoiApi.mode();

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, opts] = fetchMock.mock.calls[0];
    expect(url).toBe('/api/mode');
    expect(opts).toBeUndefined(); // GET は options なし
    expect(res).toEqual({ mode: 'navigation' });
  });
});

describe('MapoiApi.saveRoutes', () => {
  it('POST /api/routes に routes と expected_version を JSON body で送る', async () => {
    const fetchMock = mockFetch({ success: true, config_version: 'v2' });
    const routes = [{ name: 'r1', waypoints: [] }];
    const res = await MapoiApi.saveRoutes(routes, 'v1');

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, opts] = fetchMock.mock.calls[0];
    expect(url).toBe('/api/routes');
    expect(opts.method).toBe('POST');
    expect(JSON.parse(opts.body)).toEqual({ routes, expected_version: 'v1' });
    // ok/status/conflict を savePois と同じ形で付加しつつ backend body を透過する (#343)。
    expect(res).toEqual({
      ok: true, status: 200, conflict: false, success: true, config_version: 'v2',
    });
  });

  it('409 (version_mismatch) を conflict: true として組み立てる', async () => {
    mockFetch(
      { error: 'reload required', code: 'version_mismatch', current_version: 'v2' },
      { status: 409 },
    );
    const res = await MapoiApi.saveRoutes([{ name: 'r1' }], 'stale');

    expect(res.ok).toBe(false);
    expect(res.status).toBe(409);
    expect(res.conflict).toBe(true);
    expect(res.code).toBe('version_mismatch');
  });
});

describe('MapoiApi.saveCustomTags', () => {
  it('POST /api/custom-tags に custom_tags と expected_version を JSON body で送る', async () => {
    const fetchMock = mockFetch({ success: true, config_version: 'v2' });
    const customTags = [{ name: 'zone_a', description: 'Zone A' }];
    const res = await MapoiApi.saveCustomTags(customTags, 'v1');

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, opts] = fetchMock.mock.calls[0];
    expect(url).toBe('/api/custom-tags');
    expect(opts.method).toBe('POST');
    expect(JSON.parse(opts.body)).toEqual({ custom_tags: customTags, expected_version: 'v1' });
    expect(res).toEqual({
      ok: true, status: 200, conflict: false, success: true, config_version: 'v2',
    });
  });

  it('409 (version_mismatch) を conflict: true として組み立てる', async () => {
    mockFetch(
      { error: 'reload required', code: 'version_mismatch', current_version: 'v2' },
      { status: 409 },
    );
    const res = await MapoiApi.saveCustomTags([{ name: 'zone_a' }], 'stale');

    expect(res.ok).toBe(false);
    expect(res.status).toBe(409);
    expect(res.conflict).toBe(true);
    expect(res.code).toBe('version_mismatch');
  });
});

// #340 で rename した URL の pin (editor/nav 階層分離 + kebab-case 統一)。
// backend 側は test_api_select_map.py の url_map 検査が旧 URL 不在まで pin する。
// ここでは frontend の fetch 先が誤 revert しないことだけを固定する。
describe('MapoiApi renamed URLs (#340)', () => {
  it('selectMap は POST /api/editor/select-map を叩く', async () => {
    const fetchMock = mockFetch({ success: true });
    await MapoiApi.selectMap('m');
    expect(fetchMock.mock.calls[0][0]).toBe('/api/editor/select-map');
    expect(fetchMock.mock.calls[0][1].method).toBe('POST');
  });

  it('getTagDefinitions は GET /api/tag-definitions を叩く', async () => {
    const fetchMock = mockFetch({ tags: [], config_version: 'v1' });
    await MapoiApi.getTagDefinitions();
    expect(fetchMock.mock.calls[0][0]).toBe('/api/tag-definitions');
  });

  it('navInitialPose は POST /api/nav/initial-pose を叩く', async () => {
    const fetchMock = mockFetch({ success: true });
    await MapoiApi.navInitialPose('p1');
    expect(fetchMock.mock.calls[0][0]).toBe('/api/nav/initial-pose');
    expect(fetchMock.mock.calls[0][1].method).toBe('POST');
  });
});
