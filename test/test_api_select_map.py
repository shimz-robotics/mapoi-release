"""Flask endpoint level test for `POST /api/editor/select-map` (#343, URL rename #340).

SelectMap.srv の response フィールド rename (`initial_poi_name` →
`resolved_initial_poi_name`, #343) を REST JSON key レベルでも pin する。
srv 側の rename は C++ の再コンパイルで検知されるが、REST key は文字列なので
リファクタで旧名に戻っても CI で気づけない — この test がその regression を塞ぐ。

`test_api_nav_endpoints.py` と同じく `create_flask_app` を duck-typed
`_FakeNode` に bind し、`_call_service_sync` を差し替えて service 応答を偽装する。
"""
import unittest
from types import SimpleNamespace

from mapoi_webui.mapoi_webui_node import MapoiWebNode


class _NullLogger:
    def info(self, *_a, **_k):
        pass

    def warn(self, *_a, **_k):
        pass

    def error(self, *_a, **_k):
        pass


class _FakeNode:
    """select_map endpoint 経路で参照される最小限の attr / method を持つ duck-typed node."""

    def __init__(self, *, service_response):
        self._service_response = service_response
        self.select_map_client_ = object()
        self.map_name_ = 'old_map'

    def _call_service_sync(self, _client, _req, _name, timeout_sec):
        return self._service_response

    def get_logger(self):
        return _NullLogger()


def _client(node):
    app = MapoiWebNode.create_flask_app(node)
    return app.test_client()


class TestApiUrlRouting(unittest.TestCase):
    """#340 の URL rename (editor/nav 階層分離 + kebab-case 統一) を routing 表で pin する。

    url_map の検査は handler closure を評価しないため、最小 FakeNode で全 endpoint の
    登録有無を一括確認できる。互換 alias なし (旧 URL は 404) が仕様の中心なので、
    新 URL の存在だけでなく**旧 URL の不在**も assert する (alias の意図しない復活防止)。
    """

    _RENAMES = {
        '/api/maps/select': '/api/editor/select-map',
        '/api/tag_definitions': '/api/tag-definitions',
        '/api/custom_tags': '/api/custom-tags',
        '/api/nav/initialpose': '/api/nav/initial-pose',
    }

    def test_renamed_urls_registered_and_old_urls_gone(self):
        app = MapoiWebNode.create_flask_app(_FakeNode(service_response=None))
        rules = {r.rule for r in app.url_map.iter_rules()}
        for old, new in self._RENAMES.items():
            self.assertIn(new, rules, f'renamed URL {new} not registered')
            self.assertNotIn(old, rules, f'old URL {old} still registered (#340 no-alias policy)')


class TestApiSelectMap(unittest.TestCase):

    def test_success_returns_resolved_initial_poi_name_key(self):
        """成功時の JSON key は resolved_initial_poi_name (#343 rename の REST 側 pin)。"""
        node = _FakeNode(service_response=SimpleNamespace(
            success=True, error_message='', config_path='/maps/m/mapoi_config.yaml',
            resolved_initial_poi_name='poi_start'))
        resp = _client(node).post('/api/editor/select-map', json={'map_name': 'm'})
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        payload = resp.get_json()
        self.assertEqual(payload['resolved_initial_poi_name'], 'poi_start')
        self.assertNotIn('initial_poi_name', payload, '旧 key が復活している (#343 regression)')
        self.assertEqual(node.map_name_, 'm')

    def test_service_failure_returns_400_with_error_message(self):
        node = _FakeNode(service_response=SimpleNamespace(
            success=False, error_message='boom', config_path='',
            resolved_initial_poi_name=''))
        resp = _client(node).post('/api/editor/select-map', json={'map_name': 'm'})
        self.assertEqual(resp.status_code, 400)
        body = resp.get_json()
        self.assertEqual(body['error'], 'boom')
        # 機械可読 code フィールド (#343): 400 系は invalid_request で統一。
        self.assertEqual(body.get('code'), 'invalid_request')
        self.assertEqual(node.map_name_, 'old_map', '失敗時に map_name_ が更新されている')

    def test_service_unavailable_returns_503(self):
        node = _FakeNode(service_response=None)
        resp = _client(node).post('/api/editor/select-map', json={'map_name': 'm'})
        self.assertEqual(resp.status_code, 503)
        self.assertEqual(resp.get_json().get('code'), 'service_unavailable')

    def test_missing_map_name_returns_400(self):
        node = _FakeNode(service_response=None)
        resp = _client(node).post('/api/editor/select-map', json={})
        self.assertEqual(resp.status_code, 400)
        self.assertEqual(resp.get_json().get('code'), 'invalid_request')


if __name__ == '__main__':
    unittest.main()
