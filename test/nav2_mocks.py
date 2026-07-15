"""Nav2 action server mocks を launch_test 間で共有するヘルパーモジュール。

各 launch_test ファイルにコピペされていた FakeNavigateToPoseServer /
FakeFollowWaypointsServer をここに集約し、元ファイルからは import する。
"""

import threading
import time

import rclpy
from rclpy.action import ActionServer, CancelResponse
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor, SingleThreadedExecutor
from nav2_msgs.action import NavigateToPose
from nav2_msgs.action import FollowWaypoints
from nav2_msgs.srv import LoadMap
from mapoi_interfaces.srv import GetRoutePois


class FakeNavigateToPoseServer:
    """`navigate_to_pose` の最小 mock (mapoi 主導モード / 単発 Go 共通)。

    受理 goal の (x, y) を順に記録し、cancel まで EXECUTING で保持する
    (= mapoi が tolerance.xy 到達で cancel するまで Nav2 が走り続ける挙動の mock)。
    `succeed_current_goal()` を呼ぶと、実行中の goal を SUCCEEDED で finalize する
    (OR トリガ b / 最終 goal の route 完了を起こす用途)。

    FollowWaypoints mock と同様、acceptance / cancel / execute の並列処理に
    ReentrantCallbackGroup + MultiThreadedExecutor を使う。
    """

    def __init__(self, node_name='fake_navigate_to_pose_server'):
        self._node = rclpy.create_node(node_name)
        callback_group = ReentrantCallbackGroup()
        self._action_server = ActionServer(
            self._node, NavigateToPose, 'navigate_to_pose',
            execute_callback=self._execute_callback,
            cancel_callback=lambda gh: CancelResponse.ACCEPT,
            handle_accepted_callback=lambda gh: gh.execute(),
            callback_group=callback_group,
        )
        self._executor = MultiThreadedExecutor(num_threads=4)
        self._executor.add_node(self._node)
        self._stop_event = threading.Event()
        self._succeed_event = threading.Event()
        self._lock = threading.Lock()
        self._goals_xy = []
        self._thread = threading.Thread(target=self._spin, daemon=True)
        self._thread.start()

    def _spin(self):
        while rclpy.ok() and not self._stop_event.is_set():
            self._executor.spin_once(timeout_sec=0.05)

    def _execute_callback(self, goal_handle):
        p = goal_handle.request.pose.pose.position
        with self._lock:
            self._goals_xy.append((p.x, p.y))
        while (rclpy.ok() and not self._stop_event.is_set()
               and not goal_handle.is_cancel_requested
               and not self._succeed_event.is_set()):
            time.sleep(0.05)
        if goal_handle.is_cancel_requested:
            goal_handle.canceled()
        elif self._succeed_event.is_set():
            # 1 goal 分の SUCCEEDED 要求を消費する (次 goal は再び保持に戻る)。
            self._succeed_event.clear()
            goal_handle.succeed()
        else:
            goal_handle.abort()
        return NavigateToPose.Result()

    def succeed_current_goal(self):
        self._succeed_event.set()

    def goals_xy(self):
        with self._lock:
            return list(self._goals_xy)

    def reset(self):
        """test 間の状態リセット: 記録 goal と pending succeed 要求を消す。"""
        self._succeed_event.clear()
        with self._lock:
            self._goals_xy.clear()

    def shutdown(self):
        self._stop_event.set()
        self._thread.join(timeout=1.0)
        try:
            self._executor.shutdown()
        finally:
            try:
                self._action_server.destroy()
            finally:
                self._node.destroy_node()


class FakeMapServer:
    """Nav2 map_server の ``<node_name>/load_map`` service の最小 mock (#299 テスト残課題)。

    mapoi_nav2_bridge の ``send_load_map_request`` が投げる LoadMap request を
    (map_url, monotonic 受信時刻, monotonic 応答完了時刻) で記録し、``set_fail(True)``
    中は RESULT_MAP_DOES_NOT_EXIST を返して LoadMap 失敗経路を再現する。
    map ファイルの実在は見ない (URL の突き合わせだけが目的)。

    ``set_response_delay(sec)`` で応答を遅延させられる。「LoadMap **応答完了** より前に
    initialpose request が出ない」timing gate (#149/#184) を pin する用途で、遅延中に
    非同期で先行 publish する regression を応答完了時刻との比較で検出可能にする。
    遅延は bridge 側 ``spin_until_future_complete`` の timeout (1s) 未満に抑えること。

    service callback は遅延中 sleep する以外 blocking しないので SingleThreadedExecutor
    で足りる。action mock 群と同じく専用 node + daemon spin thread + shutdown() の構成。
    """

    def __init__(self, service_name='/map_server/load_map',
                 node_name='fake_map_server'):
        self._node = rclpy.create_node(node_name)
        self._lock = threading.Lock()
        self._requests = []
        self._fail = False
        self._response_delay_sec = 0.0
        self._service = self._node.create_service(
            LoadMap, service_name, self._handle_load_map)
        self._executor = SingleThreadedExecutor()
        self._executor.add_node(self._node)
        self._stop_event = threading.Event()
        self._thread = threading.Thread(target=self._spin, daemon=True)
        self._thread.start()

    def _spin(self):
        while rclpy.ok() and not self._stop_event.is_set():
            self._executor.spin_once(timeout_sec=0.05)

    def _handle_load_map(self, request, response):
        t_received = time.monotonic()
        with self._lock:
            fail = self._fail
            delay = self._response_delay_sec
        if delay > 0.0:
            time.sleep(delay)
        if fail:
            response.result = LoadMap.Response.RESULT_MAP_DOES_NOT_EXIST
        else:
            response.result = LoadMap.Response.RESULT_SUCCESS
        with self._lock:
            self._requests.append((request.map_url, t_received, time.monotonic()))
        return response

    def set_fail(self, fail):
        with self._lock:
            self._fail = fail

    def set_response_delay(self, seconds):
        with self._lock:
            self._response_delay_sec = seconds

    def requests(self):
        """(map_url, monotonic 受信時刻, monotonic 応答完了時刻) のリストを返す。"""
        with self._lock:
            return list(self._requests)

    def reset(self):
        with self._lock:
            self._requests.clear()
            self._fail = False
            self._response_delay_sec = 0.0

    def shutdown(self):
        self._stop_event.set()
        self._thread.join(timeout=1.0)
        try:
            self._executor.shutdown()
        finally:
            try:
                self._node.destroy_service(self._service)
            finally:
                self._node.destroy_node()


class FakeFollowWaypointsServer:
    """Nav2 ``follow_waypoints`` の最小 mock。

    bridge が ROUTE mode を維持できるよう、accept した goal を cancel まで
    EXECUTING で保持する。``execute_callback`` は cancel 待ちの polling loop を
    回す都合で blocking するため、test 本体の rclpy spin とは分離が必要。
    また goal acceptance / cancel request / execute_callback を 1 thread に
    押し込むと acceptance response が client に届く前に execute_callback で
    詰まり、bridge 側 ``goal_response_callback`` が永遠に来ない事象が起きる
    (humble / jazzy 両方で観測)。回避のため ReentrantCallbackGroup +
    ``MultiThreadedExecutor`` を daemon thread で回し、acceptance / cancel /
    execute を並列に処理させる。

    feedback (current_waypoint 進行) は publish しない: bridge の event 発火は TF
    由来であり、``feedback_callback`` の更新は cancel 時の paused_waypoints slice
    にのみ影響するが、本 test は cancel 後の resume を扱わないので無視できる。
    """

    def __init__(self, node_name='fake_follow_waypoints_server'):
        self._node = rclpy.create_node(node_name)
        # ReentrantCallbackGroup で acceptance / cancel / execute_callback を並列化。
        # MutuallyExclusive だと execute_callback の polling loop が他 callback を blocking する。
        callback_group = ReentrantCallbackGroup()
        self._action_server = ActionServer(
            self._node, FollowWaypoints, 'follow_waypoints',
            execute_callback=self._execute_callback,
            cancel_callback=lambda gh: CancelResponse.ACCEPT,
            handle_accepted_callback=lambda gh: gh.execute(),
            callback_group=callback_group,
        )
        # execute_callback が long-running polling のため、acceptance / cancel と並列で
        # 動かせる thread 数 (>=2) が必要。余裕を見て 4。
        self._executor = MultiThreadedExecutor(num_threads=4)
        self._executor.add_node(self._node)
        self._stop_event = threading.Event()
        self._thread = threading.Thread(target=self._spin, daemon=True)
        self._thread.start()

    def _spin(self):
        while rclpy.ok() and not self._stop_event.is_set():
            self._executor.spin_once(timeout_sec=0.05)

    def _execute_callback(self, goal_handle):
        # cancel 要求が来るまで EXECUTING 状態を維持。tearDownClass で stop_event を
        # set すると executor が止まるので、生存中の goal は ROS2 が abort する。
        while (rclpy.ok() and not self._stop_event.is_set()
               and not goal_handle.is_cancel_requested):
            time.sleep(0.05)
        if goal_handle.is_cancel_requested:
            goal_handle.canceled()
        else:
            # shutdown 経路: rclpy が落ちる前に明示的に abort する。
            goal_handle.abort()
        return FollowWaypoints.Result()

    def shutdown(self):
        self._stop_event.set()
        # daemon thread を join しても rclpy 側は context shutdown でまとめて掃除される。
        # spin_once が timeout=0.05s なので最大 50ms で抜ける。
        self._thread.join(timeout=1.0)
        try:
            self._executor.shutdown()
        finally:
            # ActionServer は Node.destroy_node() で waitable が確実に掃除されないため
            # (humble の rclpy examples も destroy() を明示)、先に明示破棄する。
            try:
                self._action_server.destroy()
            finally:
                self._node.destroy_node()


class FakeRoutePoisServer:
    """``mapoi/get_route_pois`` service の最小 mock (#353)。

    #342 で ``GetRoutePois.srv`` の response に ``success`` / ``error_message`` が
    追加されたことで、「service 自体が異常応答を返す」経路 (``on_route_received`` の
    (b) ``success=false`` 経路、および (c) ``success=true`` だが ``pois_list`` 空の
    "Route is empty" 経路) を決定論的に mock できるようになった。実物の mapoi_server が
    返す success=false は「route not found」の 1 パターンに限られるため、server 実装に
    依存しない任意の異常応答に対する bridge 側防御の pin にはこの mock が必要
    (docs/testing-policy.md 3節)。

    ``set_response()`` でテストケースごとに応答を切り替える。service callback は
    lock 下で保持値を返すだけで blocking しないため、``FakeMapServer`` と同じく
    SingleThreadedExecutor で足りる。
    """

    def __init__(self, service_name='mapoi/get_route_pois', node_name='fake_route_pois_server'):
        self._node = rclpy.create_node(node_name)
        self._lock = threading.Lock()
        self._success = True
        self._error_message = ''
        self._pois_list = []
        self._landmark_pois = []
        self._requests = []
        self._service = self._node.create_service(
            GetRoutePois, service_name, self._handle_request)
        self._executor = SingleThreadedExecutor()
        self._executor.add_node(self._node)
        self._stop_event = threading.Event()
        self._thread = threading.Thread(target=self._spin, daemon=True)
        self._thread.start()

    def _spin(self):
        while rclpy.ok() and not self._stop_event.is_set():
            self._executor.spin_once(timeout_sec=0.05)

    def _handle_request(self, request, response):
        with self._lock:
            self._requests.append(request.route_name)
            response.success = self._success
            response.error_message = self._error_message
            response.pois_list = list(self._pois_list)
            response.landmark_pois = list(self._landmark_pois)
        return response

    def set_response(self, success=True, error_message='', pois_list=None, landmark_pois=None):
        """次回以降の request への応答を設定する (次に set_response / reset するまで固定)。"""
        with self._lock:
            self._success = success
            self._error_message = error_message
            self._pois_list = list(pois_list) if pois_list else []
            self._landmark_pois = list(landmark_pois) if landmark_pois else []

    def requests(self):
        """受信した route_name のリストを返す (呼び出し確認用)。"""
        with self._lock:
            return list(self._requests)

    def reset(self):
        """test 間の状態リセット: 応答を既定 (success=True, 空リスト) に戻し記録も消す。"""
        with self._lock:
            self._success = True
            self._error_message = ''
            self._pois_list = []
            self._landmark_pois = []
            self._requests.clear()

    def shutdown(self):
        self._stop_event.set()
        self._thread.join(timeout=1.0)
        try:
            self._executor.shutdown()
        finally:
            try:
                self._node.destroy_service(self._service)
            finally:
                self._node.destroy_node()
