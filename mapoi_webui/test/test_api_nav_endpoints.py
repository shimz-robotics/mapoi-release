"""Flask endpoint level test for navigation API (#199).

`/api/nav/switch-map` の validation 契約と、`/api/mode` / `/api/nav/status` が
navigation capabilities を正しく包んで返すことを Flask `test_client` で固定する:

- `/api/nav/switch-map`
    * body が dict でない / `map_name` 不在 / 空白のみ → 400 `map_name required`
    * `get_maps_list()` に存在しない map → 404 `unknown map: ...`
    * maps list が空 (maps_path 未設定等) → membership check skip して 200
    * 正常時は `mapoi/nav/switch_map` に strip 済 map_name を publish して 200
    * publish helper の warning を payload に透過
- `/api/mode`        → `{'navigation': <capabilities>}`
- `/api/nav/status`  → status / target / robot_pose / robot_radius / navigation を同梱

`test_api_save_pois_version_conflict.py` と同じく `create_flask_app` を duck-typed
`_FakeNode` に bind する。各 endpoint closure は request 時にしか評価されないため、
本 test が叩く経路で参照される attr / method だけを `_FakeNode` に持たせれば足りる。
`get_navigation_capabilities` 自体のロジックは `test_get_navigation_capabilities.py`
で別途 pin しているので、ここでは sentinel dict の透過だけを確認する。
"""
import unittest

from mapoi_webui.mapoi_webui_node import MapoiWebNode


class _NullLogger:
    def info(self, *_a, **_k):
        pass

    def warn(self, *_a, **_k):
        pass

    def error(self, *_a, **_k):
        pass


class _FakeNode:
    """nav endpoint 経路で参照される最小限の attr / method を持つ duck-typed node."""

    def __init__(self, *, maps=None, warning=None, nav_caps=None,
                 nav_status='idle', nav_target=None, robot_pose=None,
                 robot_radius=0.3):
        self._maps = [] if maps is None else list(maps)
        self._warning = warning
        self._nav_caps = nav_caps if nav_caps is not None else {
            'navigation_available': False}
        self.nav_status_ = nav_status
        self.nav_status_target_ = nav_target
        self.robot_pose_ = robot_pose
        self.robot_radius_ = robot_radius
        # publish_with_subscriber_check に渡されるだけで中身は使わない
        self.switch_map_pub_ = object()
        # publish された (topic, data) を記録して assert する
        self.published = []

    def get_maps_list(self):
        return list(self._maps)

    def publish_with_subscriber_check(self, pub, msg, topic_name):
        self.published.append((topic_name, getattr(msg, 'data', None)))
        return (True, self._warning)

    def get_navigation_capabilities(self):
        return self._nav_caps

    def get_logger(self):
        return _NullLogger()


def _client(node):
    app = MapoiWebNode.create_flask_app(node)
    return app.test_client()


class TestApiNavSwitchMap(unittest.TestCase):

    def test_non_dict_body_returns_400(self):
        node = _FakeNode(maps=['mapA'])
        resp = _client(node).post('/api/nav/switch-map', json=[1, 2])
        self.assertEqual(resp.status_code, 400, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertIn('map_name', body.get('error', ''))
        # 機械可読 code フィールド (#343): 400 系は invalid_request で統一。
        self.assertEqual(body.get('code'), 'invalid_request')
        self.assertEqual(node.published, [])  # publish されていない

    def test_missing_map_name_returns_400(self):
        node = _FakeNode(maps=['mapA'])
        resp = _client(node).post('/api/nav/switch-map', json={})
        self.assertEqual(resp.status_code, 400, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertIn('map_name', body.get('error', ''))
        self.assertEqual(body.get('code'), 'invalid_request')
        self.assertEqual(node.published, [])

    def test_whitespace_map_name_returns_400(self):
        node = _FakeNode(maps=['mapA'])
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': '   '})
        self.assertEqual(resp.status_code, 400, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertIn('map_name', body.get('error', ''))
        self.assertEqual(body.get('code'), 'invalid_request')
        self.assertEqual(node.published, [])

    def test_non_string_map_name_returns_400(self):
        """非文字列 map_name は coerce せず 400 で弾く (#199 follow-up)。

        旧実装は `str(data['map_name']).strip()` で coerce していたため、
        `null` が literal 'None'、数値が '123' になり、maps_path 未設定時の
        membership skip 経路でそのまま publish される潜在バグがあった。
        """
        for bad in (None, 123, 1.5, True, False, ['mapA'], {'name': 'mapA'}):
            node = _FakeNode(maps=[])  # membership skip 経路でも publish させない
            resp = _client(node).post('/api/nav/switch-map', json={'map_name': bad})
            self.assertEqual(
                resp.status_code, 400,
                f'map_name={bad!r}: {resp.get_data(as_text=True)}')
            body = resp.get_json()
            self.assertIn('map_name', body.get('error', ''))
            self.assertEqual(body.get('code'), 'invalid_request')
            self.assertEqual(node.published, [], f'map_name={bad!r} を publish した')

    def test_unknown_map_returns_404(self):
        node = _FakeNode(maps=['mapA', 'mapB'])
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': 'mapX'})
        self.assertEqual(resp.status_code, 404, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertIn('mapX', body.get('error', ''))
        # 機械可読 code フィールド (#343): 404 系は not_found で統一。
        self.assertEqual(body.get('code'), 'not_found')
        self.assertEqual(node.published, [])

    def test_valid_map_publishes_and_returns_200(self):
        node = _FakeNode(maps=['mapA'])
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': 'mapA'})
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertTrue(body.get('success'))
        self.assertEqual(body.get('map_name'), 'mapA')
        self.assertNotIn('warning', body)  # warning なし時は key を出さない
        self.assertEqual(node.published, [('mapoi/nav/switch_map', 'mapA')])

    def test_valid_map_name_is_stripped_before_publish(self):
        node = _FakeNode(maps=['mapA'])
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': '  mapA  '})
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        self.assertEqual(resp.get_json().get('map_name'), 'mapA')
        # publish される data も strip 済
        self.assertEqual(node.published, [('mapoi/nav/switch_map', 'mapA')])

    def test_empty_maps_list_skips_membership_check(self):
        """`get_maps_list()` が空 (maps_path 未設定等) なら存在チェックを skip して publish。

        実装の `if maps and map_name not in maps` は maps 空で short-circuit する。
        """
        node = _FakeNode(maps=[])
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': 'anything'})
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        self.assertEqual(node.published, [('mapoi/nav/switch_map', 'anything')])

    def test_warning_is_passed_through(self):
        node = _FakeNode(maps=['mapA'], warning='no subscribers on mapoi/nav/switch_map')
        resp = _client(node).post('/api/nav/switch-map', json={'map_name': 'mapA'})
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        self.assertEqual(
            resp.get_json().get('warning'), 'no subscribers on mapoi/nav/switch_map')


class TestApiMode(unittest.TestCase):

    def test_mode_wraps_navigation_capabilities(self):
        sentinel = {'navigation_available': True, 'command_available': False, 'topics': {}}
        node = _FakeNode(nav_caps=sentinel)
        resp = _client(node).get('/api/mode')
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        self.assertEqual(resp.get_json(), {'navigation': sentinel})


class TestApiNavStatus(unittest.TestCase):

    def test_nav_status_payload_shape(self):
        sentinel = {'navigation_available': True}
        node = _FakeNode(
            nav_caps=sentinel,
            nav_status='navigating',
            nav_target='poi_kitchen',
            robot_pose={'x': 1.0, 'y': 2.0, 'yaw': 0.5},
            robot_radius=0.35,
        )
        resp = _client(node).get('/api/nav/status')
        self.assertEqual(resp.status_code, 200, resp.get_data(as_text=True))
        body = resp.get_json()
        self.assertEqual(body.get('status'), 'navigating')
        self.assertEqual(body.get('target'), 'poi_kitchen')
        self.assertEqual(body.get('robot_pose'), {'x': 1.0, 'y': 2.0, 'yaw': 0.5})
        self.assertEqual(body.get('robot_radius'), 0.35)
        # navigation capabilities が同梱されている
        self.assertEqual(body.get('navigation'), sentinel)


if __name__ == '__main__':
    unittest.main()
