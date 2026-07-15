"""SSE / config publish 経路の callback + Flask endpoint level test (#135 (B), #173, #193 残課題)。

これまで `mapoi_webui` の Python 側 test は yaml handler と nav / save API endpoint に限られ、
config 変更を frontend に push する SSE 経路 (config_path_callback → _broadcast_sse_event →
`/api/events`) は未カバーだった。本 test はその穴を埋める:

- `config_path_callback`: `mapoi/config_path` 受信時の map 名抽出と broadcast 判定
  (外部切替 / 同一 map 内容変更 / 期待形式外で無駄な reload を避ける判定 / 異常入力耐性)
- `command_rejected_callback`: `mapoi/nav/command_rejected` 受信 → SSE broadcast 経路 (#354)
- `_broadcast_sse_event`: 接続中の全 client queue への fanout、event data 形状、Full 吸収
- `/api/events`: SSE response header、client 登録 → 配信 → 切断時 discard lifecycle (queue leak 防止)

`test_api_nav_endpoints.py` と同じく rclpy を起動せず、duck-typed fake を `MapoiWebNode` の
unbound メソッドに渡して呼ぶ (node 生成不要)。`/api/events` の generator は `q.get(timeout=30)`
で blocking するため、view を直接呼んで Response を取り出し、別スレッドで 1 frame だけ drain して
deterministically に検証する (put→get は即時なので client 登録さえ確認できれば race しない)。
"""
import hashlib
import json
import os
import queue
import tempfile
import threading
import time
import unittest

from mapoi_webui.mapoi_webui_node import MapoiWebNode


class _NullLogger:
    def info(self, *_a, **_k):
        pass

    def warn(self, *_a, **_k):
        pass

    def error(self, *_a, **_k):
        pass


class _Msg:
    """std_msgs/String 互換の最小 stub (data 属性のみ)。"""

    def __init__(self, data):
        self.data = data


class _FakeConfigNode:
    """config_path_callback が参照する最小 attr を持つ duck-typed node。

    `_broadcast_sse_event` は呼ばれた (event_type, payload) を記録するだけの stub にして、
    callback の「broadcast するか否か / map 名抽出が正しいか」だけを isolate して検証する。
    """

    def __init__(self, config_file='mapoi_config.yaml', map_name='mapA'):
        self.config_file_ = config_file
        self.map_name_ = map_name
        self.broadcasts = []

    def _broadcast_sse_event(self, event_type, payload=None):
        self.broadcasts.append((event_type, payload))

    def get_logger(self):
        return _NullLogger()


class _FakeSseNode:
    """`_broadcast_sse_event` / `/api/events` generator が参照する最小 attr を持つ node。"""

    def __init__(self):
        self._sse_clients = set()
        self._sse_lock = threading.Lock()

    def get_logger(self):
        return _NullLogger()


def _call_config_path(node, data):
    # unbound メソッドに fake self を渡す (node 生成不要)。
    MapoiWebNode.config_path_callback(node, _Msg(data))


class TestConfigPathCallback(unittest.TestCase):
    """mapoi/config_path 受信 → map 名抽出 → broadcast 判定 (#135 (B), #173)。"""

    def test_external_switch_updates_map_and_broadcasts(self):
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, '/data/maps/mapB/mapoi_config.yaml')
        self.assertEqual(node.map_name_, 'mapB')  # 外部切替で map_name_ 更新
        self.assertEqual(
            node.broadcasts, [('config_changed', {'map_name': 'mapB'})])

    def test_same_map_content_change_still_broadcasts(self):
        # 同一 map の内容変更 (save 後 reload による再 publish) でも frontend に通知し、
        # loadPois / loadRoutes を再実行させる (#135 (B))。map_name_ は変わらない。
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, '/data/maps/mapA/mapoi_config.yaml')
        self.assertEqual(node.map_name_, 'mapA')
        self.assertEqual(
            node.broadcasts, [('config_changed', {'map_name': 'mapA'})])

    def test_broadcast_includes_config_version_when_file_readable(self):
        # yaml が実在する場合は payload に config_version (yaml 全体の sha256, #241) を
        # 同梱する (#384)。frontend はこれを save 応答の version と照合し、自タブ発の
        # 変更なら reload (undo 履歴・map 視点の破棄) を skip する。
        # 既存の他 test は実在しない path を使うため version は計算不能 → 省略 (後方互換)。
        with tempfile.TemporaryDirectory() as tmp:
            config_dir = os.path.join(tmp, 'maps', 'mapB')
            os.makedirs(config_dir)
            config_path = os.path.join(config_dir, 'mapoi_config.yaml')
            content = b'poi: []\n'
            with open(config_path, 'wb') as f:
                f.write(content)
            expected_version = hashlib.sha256(content).hexdigest()
            node = _FakeConfigNode(map_name='mapA')
            _call_config_path(node, config_path)
            self.assertEqual(node.broadcasts, [('config_changed', {
                'map_name': 'mapB',
                'config_version': expected_version,
            })])

    def test_path_without_config_file_does_not_broadcast(self):
        # config_file が path に現れない → map 特定不可 → 無駄な reload を避けて broadcast しない
        # (#173 Round 1 / 2 medium)。
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, '/data/maps/mapB/other_file.yaml')
        self.assertEqual(node.map_name_, 'mapA')  # 不変
        self.assertEqual(node.broadcasts, [])

    def test_config_file_at_path_root_does_not_broadcast(self):
        # config_file が先頭 (i == 0) で前ディレクトリが無い → map 特定不可 → broadcast しない
        # (実装の `i > 0` ガード)。
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, 'mapoi_config.yaml')
        self.assertEqual(node.map_name_, 'mapA')
        self.assertEqual(node.broadcasts, [])

    def test_backslash_path_is_normalized(self):
        # Windows 形式の backslash 区切りでも map 名を抽出できる (replace('\\', '/'))。
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, r'C:\data\maps\mapC\mapoi_config.yaml')
        self.assertEqual(node.map_name_, 'mapC')
        self.assertEqual(
            node.broadcasts, [('config_changed', {'map_name': 'mapC'})])

    def test_parse_failure_is_swallowed(self):
        # msg.data が str でない等の異常入力でも例外を投げず broadcast もしない (resilience)。
        node = _FakeConfigNode(map_name='mapA')
        _call_config_path(node, 123)  # int.replace は無い → except 経路
        self.assertEqual(node.map_name_, 'mapA')
        self.assertEqual(node.broadcasts, [])


class _FakeCommandRejectedNode:
    """command_rejected_callback が参照する最小 attr を持つ duck-typed node (#354)。

    `_broadcast_sse_event` は呼ばれた (event_type, payload) を記録するだけの stub にして、
    callback が `mapoi/nav/command_rejected` の payload をどう SSE event に変換するかだけを
    isolate して検証する。
    """

    def __init__(self):
        self.broadcasts = []

    def _broadcast_sse_event(self, event_type, payload=None):
        self.broadcasts.append((event_type, payload))


def _call_command_rejected(node, data):
    # unbound メソッドに fake self を渡す (node 生成不要)。
    MapoiWebNode.command_rejected_callback(node, _Msg(data))


class TestCommandRejectedCallback(unittest.TestCase):
    """mapoi/nav/command_rejected 受信 → SSE broadcast 経路 (#354)。

    走行中の reject でも `mapoi/nav/status` (latched snapshot) を汚さず、frontend には
    一時的な toast として流すためのイベント経路。bridge 側 payload は target 文字列のみ
    (`std_msgs/String`) なので、callback はそれを `{'target': <文字列>}` に包んで
    broadcast するだけの薄い変換であることを pin する。
    """

    def test_broadcasts_target_payload(self):
        node = _FakeCommandRejectedNode()
        _call_command_rejected(node, 'poi_typo_does_not_exist')
        self.assertEqual(
            node.broadcasts,
            [('command_rejected', {'target': 'poi_typo_does_not_exist'})])

    def test_empty_target_still_broadcasts(self):
        # bridge 側で target が空文字のケース (仕様上想定していないが防御的に確認)。
        node = _FakeCommandRejectedNode()
        _call_command_rejected(node, '')
        self.assertEqual(node.broadcasts, [('command_rejected', {'target': ''})])


class TestBroadcastSseEvent(unittest.TestCase):
    """接続中の全 client queue への fanout と event data 形状 (#135 (B), #173 Round 1 high)。"""

    @staticmethod
    def _broadcast(node, event_type, payload=None):
        MapoiWebNode._broadcast_sse_event(node, event_type, payload)

    def test_payload_event_shape(self):
        node = _FakeSseNode()
        q = queue.Queue()
        node._sse_clients.add(q)
        self._broadcast(node, 'config_changed', {'map_name': 'mapB'})
        self.assertEqual(
            q.get_nowait(), {'type': 'config_changed', 'payload': {'map_name': 'mapB'}})

    def test_no_payload_omits_payload_key(self):
        node = _FakeSseNode()
        q = queue.Queue()
        node._sse_clients.add(q)
        self._broadcast(node, 'ping', None)
        data = q.get_nowait()
        self.assertEqual(data, {'type': 'ping'})
        self.assertNotIn('payload', data)

    def test_fanout_to_all_clients(self):
        node = _FakeSseNode()
        qs = [queue.Queue() for _ in range(3)]
        for q in qs:
            node._sse_clients.add(q)
        self._broadcast(node, 'config_changed', {'map_name': 'm'})
        for q in qs:
            self.assertEqual(
                q.get_nowait(), {'type': 'config_changed', 'payload': {'map_name': 'm'}})

    def test_full_queue_is_swallowed_and_others_delivered(self):
        # 遅い client の bounded queue が Full でも例外を投げず、他 client へは配信される
        # (#173 Round 1 high の DoS 対策の安全弁)。
        node = _FakeSseNode()
        full = queue.Queue(maxsize=1)
        full.put_nowait({'pre': 'existing'})  # 既に満杯
        ok = queue.Queue()
        node._sse_clients.add(full)
        node._sse_clients.add(ok)
        self._broadcast(node, 'config_changed', {'map_name': 'm'})  # 例外を投げない
        # full queue には新 event が入らず drop される
        self.assertEqual(full.get_nowait(), {'pre': 'existing'})
        self.assertTrue(full.empty())
        # ok queue へは配信されている
        self.assertEqual(
            ok.get_nowait(), {'type': 'config_changed', 'payload': {'map_name': 'm'}})


class TestApiEventsEndpoint(unittest.TestCase):
    """/api/events SSE endpoint の header / 登録・配信・切断 discard lifecycle (#173)。"""

    @staticmethod
    def _make_response(node):
        app = MapoiWebNode.create_flask_app(node)
        with app.test_request_context('/api/events'):
            return app.view_functions['api_events']()

    def test_sse_response_headers(self):
        # プロキシ (nginx 等) の buffering 無効化 + cache 無効化 (#173 Round 1 medium)。
        node = _FakeSseNode()
        resp = self._make_response(node)
        self.assertEqual(resp.mimetype, 'text/event-stream')
        self.assertEqual(resp.headers.get('Cache-Control'), 'no-cache')
        self.assertEqual(resp.headers.get('X-Accel-Buffering'), 'no')
        # 未 iterate の generator を閉じる (client 登録前なので副作用なし)。
        resp.response.close()
        self.assertEqual(len(node._sse_clients), 0)

    def test_register_deliver_and_discard_lifecycle(self):
        node = _FakeSseNode()
        resp = self._make_response(node)
        gen = resp.response  # lazy generator (Response に渡された gen() オブジェクト)
        received = []

        def reader():
            # 最初の next() で client queue を登録し、broadcast されるまで q.get で待つ。
            received.append(next(gen))

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        # client 登録待ち (put→get は即時なので登録さえ確認できれば deterministic)。
        deadline = time.time() + 5.0
        while not node._sse_clients and time.time() < deadline:
            time.sleep(0.005)
        self.assertEqual(len(node._sse_clients), 1, 'client queue が登録されていない')

        MapoiWebNode._broadcast_sse_event(node, 'config_changed', {'map_name': 'mapB'})
        t.join(timeout=5.0)
        self.assertFalse(t.is_alive(), 'broadcast 後も generator が yield しない')
        self.assertEqual(len(received), 1)
        frame = received[0]
        self.assertTrue(frame.startswith('data: '), frame)
        self.assertTrue(frame.endswith('\n\n'), frame)
        payload = json.loads(frame[len('data: '):].strip())
        self.assertEqual(
            payload, {'type': 'config_changed', 'payload': {'map_name': 'mapB'}})

        # client 切断 (generator close) で finally が queue を discard する
        # (#173 Round 1 high, queue leak 防止)。
        gen.close()
        self.assertEqual(len(node._sse_clients), 0, '切断時に client queue が discard されない')


if __name__ == '__main__':
    unittest.main()
