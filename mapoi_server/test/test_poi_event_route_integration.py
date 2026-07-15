"""ROUTE-mode PoiEvent 発火 integration test (#226).

`test_poi_event_integration.py` (negative evidence: route 走行外では発火しない) の
positive 版。bridge が ROUTE mode に入ってから ENTER / PAUSED / EXIT が発火することを
launch_testing で検証する。

bridge は `mapoi_route_cb` で Nav2 ``follow_waypoints`` action server に接続できないと
``reset_nav_state()`` で ``nav_mode_`` を IDLE に戻すため、ROUTE mode を維持して event
発火を観測するには Nav2 mock が必須。本テストでは下記 ``FakeFollowWaypointsServer``
が単一の goal を ACCEPTED → EXECUTING で保持し、cancel 要求が来たら CANCELED で
finalize するだけの最小実装。tolerance event 判定は TF 由来なので Nav2 feedback の
``current_waypoint`` 進行は不要 (mock は feedback も result 進行も流さない)。
"""

import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import launch
import launch_ros.actions
import launch_testing
import launch_testing.actions
import launch_testing.markers

import rclpy
from rclpy.qos import QoSDurabilityPolicy, QoSHistoryPolicy, QoSProfile, QoSReliabilityPolicy
from tf2_ros import TransformBroadcaster
from geometry_msgs.msg import TransformStamped
from geometry_msgs.msg import Twist
from mapoi_interfaces.msg import PoiEvent
from ament_index_python.packages import get_package_share_directory
from std_msgs.msg import String
from nav2_mocks import FakeFollowWaypointsServer


# pause 判定 dwell。tolerance_check_hz=10 (= 100ms) より十分長く取り、PAUSED まで
# 到達するまでの待ち時間を test 全体で短く保つ。
STOPPED_DWELL_TIME_SEC = 0.3


@launch_testing.markers.keep_alive
def generate_test_description():
    pkg_share = get_package_share_directory('mapoi_server')
    test_config_dir = os.path.join(pkg_share, 'test')

    mapoi_server_node = launch_ros.actions.Node(
        package='mapoi_server',
        executable='mapoi_server',
        name='mapoi_server',
        parameters=[{
            'maps_path': test_config_dir,
            'map_name': '.',
            'config_file': 'test_mapoi_config.yaml',
        }],
    )

    mapoi_nav2_bridge_node = launch_ros.actions.Node(
        package='mapoi_server',
        executable='mapoi_nav2_bridge',
        name='mapoi_nav2_bridge',
        parameters=[{
            'tolerance_check_hz': 10.0,
            'map_frame': 'map',
            'base_frame': 'base_link',
            'stopped_dwell_time_sec': STOPPED_DWELL_TIME_SEC,
            # 本 test は Twist で cmd_vel を publish するため、distro auto-detect (jazzy で
            # TwistStamped 化) を上書きして Twist 固定 (#249)。
            'cmd_vel_msg_type': 'twist',
        }],
    )

    return launch.LaunchDescription([
        mapoi_server_node,
        mapoi_nav2_bridge_node,
        launch_testing.actions.ReadyToTest(),
    ]), {'mapoi_server': mapoi_server_node, 'mapoi_nav2_bridge': mapoi_nav2_bridge_node}


class TestPoiEventRouteIntegration(unittest.TestCase):
    """ROUTE mode で ENTER / PAUSED / EXIT が新仕様 (#220) どおり発火する positive 経路。

    mock action server を立てて bridge を ROUTE mode に押し込み、TF / cmd_vel を
    test 側から動かして event 発火を観測する。各 test 後は cancel + 遠ざけで
    bridge 内 state を IDLE / inside=false に戻す。
    """

    EVENT_WAIT_TIMEOUT = 5.0
    NAV_STATUS_WAIT_TIMEOUT = 5.0
    TF_PUBLISH_INTERVAL = 0.05
    CMD_VEL_PUBLISH_INTERVAL = 0.05
    FAR_AWAY_XY = (100.0, 100.0)
    DEFAULT_ROUTE_NAME = 'route_a'

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_poi_event_route_node')
        cls.received_events = []
        cls.received_nav_status = []
        cls.inside_state = {}

        cls.event_sub = cls.node.create_subscription(
            PoiEvent, 'mapoi/events', cls._event_callback, 10)
        # mapoi/nav/status は bridge 側 publisher が transient_local (mapoi_nav2_bridge.cpp:51)。
        # default (volatile) subscriber では QoS 不整合で全メッセージが drop される。
        nav_status_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
        )
        cls.nav_status_sub = cls.node.create_subscription(
            String, 'mapoi/nav/status', cls._nav_status_callback, nav_status_qos)
        cls.tf_broadcaster = TransformBroadcaster(cls.node)
        cls.cmd_vel_pub = cls.node.create_publisher(Twist, 'cmd_vel', 10)
        cls.route_pub = cls.node.create_publisher(String, 'mapoi/nav/route', 1)
        cls.cancel_pub = cls.node.create_publisher(String, 'mapoi/nav/cancel', 1)

        cls._robot_xy = cls.FAR_AWAY_XY
        cls._cmd_vel_linear_x = 0.2
        cls._cmd_vel_angular_z = 0.0
        cls._tf_timer = cls.node.create_timer(
            cls.TF_PUBLISH_INTERVAL, cls._tf_publish_callback)
        cls._cmd_vel_timer = cls.node.create_timer(
            cls.CMD_VEL_PUBLISH_INTERVAL, cls._cmd_vel_publish_callback)

        # Nav2 mock を最後に立てる: 上記 publisher / subscription の discovery と並行で
        # action server discovery を流して試験開始までの wait を短縮する。
        cls.fake_server = FakeFollowWaypointsServer()

    @classmethod
    def tearDownClass(cls):
        try:
            cls.fake_server.shutdown()
        finally:
            try:
                cls.node.destroy_timer(cls._cmd_vel_timer)
                cls.node.destroy_timer(cls._tf_timer)
                cls.node.destroy_node()
            finally:
                rclpy.shutdown()

    @classmethod
    def _tf_publish_callback(cls):
        x, y = cls._robot_xy
        t = TransformStamped()
        t.header.stamp = cls.node.get_clock().now().to_msg()
        t.header.frame_id = 'map'
        t.child_frame_id = 'base_link'
        t.transform.translation.x = x
        t.transform.translation.y = y
        t.transform.translation.z = 0.0
        t.transform.rotation.w = 1.0
        cls.tf_broadcaster.sendTransform(t)

    @classmethod
    def _cmd_vel_publish_callback(cls):
        msg = Twist()
        msg.linear.x = cls._cmd_vel_linear_x
        msg.angular.z = cls._cmd_vel_angular_z
        cls.cmd_vel_pub.publish(msg)

    @classmethod
    def _event_callback(cls, msg):
        cls.received_events.append(msg)
        if msg.event_type == PoiEvent.EVENT_ENTER:
            cls.inside_state[msg.poi.name] = True
        elif msg.event_type == PoiEvent.EVENT_EXIT:
            cls.inside_state[msg.poi.name] = False

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    def setUp(self):
        # 全 POI から十分遠い位置で開始し、ROUTE 起動前の inside ENTER を防ぐ。
        self._set_robot_pose(*self.FAR_AWAY_XY)
        self._set_cmd_vel(0.2, 0.0)
        # tearDown の cancel が遅延配送されて新 route を kill する race を防ぐ
        # 防御 cancel + spin。idempotent (bridge 側 ``mapoi_cancel_cb`` は active
        # goal 不在でも reset_nav_state() のみ呼ぶ)。
        self._publish_cancel()
        self._spin_for(0.2)
        self.received_events.clear()
        self.received_nav_status.clear()
        # POI list の初期 fetch + tag definitions の取得まで bridge が落ち着くのを待つ。
        # 1 度のみ実行: 2 回目以降の test では subscriber は既に揃っている。
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/route'),
                        'mapoi_nav2_bridge subscribed mapoi/nav/route が見えない')

    def tearDown(self):
        """次 test の干渉を消して bridge 内 state を IDLE / inside=false に戻す。

        bridge の ``mapoi_cancel_cb`` は cancel 不要時 (handle が pause 経路で既に
        reset 済等) でも ``reset_nav_state()`` を呼ぶため、cancel publish だけで
        ``nav_mode_`` / ``poi_inside_state_`` / ``is_paused_`` は IDLE にリセット
        される。``"canceled"`` status は経路によっては publish されない (pause 後の
        2 度目 cancel 等) ので待たない (浪費)。代わりに 0.5s spin して cancel
        message が bridge へ届き ``reset_nav_state`` が実行されるのを待つ
        (local DDS + reliable QoS で sub-ms latency、bridge は default executor
        で tolerance_check を 100ms 周期で回しているので、tick 間に cancel が
        処理される。3-5x マージン)。後段 setUp 側でも防御 cancel を打つ。

        test 側の ``inside_state`` (subscriber 視点) は EVENT_EXIT 受信で更新する
        が、cancel 後は bridge が ``current_route_poi_names_`` を空にしているため
        ``is_route_poi=false`` で EXIT も発火しない (#220 lifecycle 保護)。よって
        test 側 tracker を bridge の reset と整合させるため明示クリアする。
        """
        self._publish_cancel()
        self._spin_for(0.5)
        type(self).inside_state.clear()
        self.received_events.clear()
        self.received_nav_status.clear()
        self._set_cmd_vel(0.2, 0.0)
        self._set_robot_pose(*self.FAR_AWAY_XY)

    # --- helpers ---

    def _set_robot_pose(self, x, y):
        type(self)._robot_xy = (x, y)

    def _set_cmd_vel(self, linear_x, angular_z):
        type(self)._cmd_vel_linear_x = linear_x
        type(self)._cmd_vel_angular_z = angular_z

    def _spin_for(self, duration_sec):
        end_time = time.monotonic() + duration_sec
        while time.monotonic() < end_time:
            rclpy.spin_once(self.node, timeout_sec=0.05)

    def _wait_for_subscriber(self, topic_name, timeout_sec=5.0):
        end_time = time.monotonic() + timeout_sec
        while time.monotonic() < end_time:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if self.node.count_subscribers(topic_name) > 0:
                return True
        return False

    def _wait_for_event(self, predicate, timeout_sec=None):
        if timeout_sec is None:
            timeout_sec = self.EVENT_WAIT_TIMEOUT
        end_time = time.monotonic() + timeout_sec
        while time.monotonic() < end_time:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            if any(predicate(e) for e in self.received_events):
                return True
        return False

    def _wait_for_nav_status(self, status, timeout_sec=None):
        """``status`` の publish を観測したら True。

        bridge の publish_nav_status は ``status:target`` 形式 (target 有り) または
        ``status`` 単独 (target 空) で出る (mapoi_nav2_bridge.cpp:728)。``status``
        prefix で startswith マッチして target 文字列の差を吸収する。
        """
        if timeout_sec is None:
            timeout_sec = self.NAV_STATUS_WAIT_TIMEOUT
        end_time = time.monotonic() + timeout_sec
        while time.monotonic() < end_time:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            if any(s == status or s.startswith(status + ':')
                   for s in self.received_nav_status):
                return True
        return False

    def _count_events(self, predicate):
        return sum(1 for e in self.received_events if predicate(e))

    def _publish_route(self, route_name=None):
        msg = String()
        msg.data = route_name if route_name is not None else self.DEFAULT_ROUTE_NAME
        self.route_pub.publish(msg)

    def _publish_cancel(self):
        msg = String()
        msg.data = ''
        self.cancel_pub.publish(msg)

    def _activate_route(self, route_name=None):
        """Route を投入し、bridge が ROUTE mode に入る (= "navigating" status) まで wait。

        bridge は ``mapoi_route_cb`` → ``get_route_pois`` 待ち → action server readiness
        判定 → ``async_send_goal`` の流れで ROUTE mode に入る。test 側は wait_for_subscriber
        では action server discovery を見られないので、nav_status の "navigating" を
        ROUTE mode 入りの signal として使う。
        """
        self._publish_route(route_name)
        self.assertTrue(
            self._wait_for_nav_status('navigating'),
            f"route '{route_name or self.DEFAULT_ROUTE_NAME}' で 'navigating' status を観測できなかった "
            f"(received: {self.received_nav_status})")

    # --- tests ---
    # route_a = [poi_goal_only(1,0), poi_with_pause(0,2,pause), poi_with_custom(3,0,audio_info),
    #            poi_pause_and_custom(0,4,pause+audio_info)]

    def test_enter_event_in_route_mode(self):
        """ROUTE 走行中に route 登録 POI へ侵入すると EVENT_ENTER が発火する。"""
        self._activate_route()
        self._set_robot_pose(1.0, 0.0)  # poi_goal_only
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_goal_only')),
            'route 登録 POI への侵入で EVENT_ENTER が発火するはず')

    def test_exit_event_after_enter(self):
        """ENTER 後に hysteresis * tolerance を超えて退出すると EVENT_EXIT が発火する。"""
        self._activate_route()
        self._set_robot_pose(1.0, 0.0)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_goal_only')),
            '前提: ENTER の発火')
        self._set_robot_pose(*self.FAR_AWAY_XY)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_EXIT
                           and e.poi.name == 'poi_goal_only')),
            'POI から十分離れたら EVENT_EXIT が発火するはず')

    def test_paused_event_in_pause_poi(self):
        """pause タグ付き route POI 内で cmd_vel が dwell すると EVENT_PAUSED が 1 回発火する。

        EVENT_PAUSED 単独の assertion だけだと、auto-pause / FollowWaypoints cancel
        の経路が壊れていても (TF + cmd_vel 由来の判定で) PAUSED は発火しうる。
        bridge は pause タグ POI に ENTER した瞬間 ``mapoi_pause_cb`` 経由で
        ``"paused"`` nav status を発行する (mapoi_nav2_bridge.cpp:903) ので、その
        observation を併せて確認することで auto-pause regression も検出する。
        """
        self._activate_route()
        self._set_robot_pose(0.0, 2.0)  # poi_with_pause
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_with_pause')),
            '前提: pause POI への ENTER')
        # ENTER 直後に bridge が auto-pause を発火し "paused" nav status を出すはず。
        # ここを assert することで auto-pause / cancel 経路の regression も検出する。
        self.assertTrue(
            self._wait_for_nav_status('paused', timeout_sec=2.0),
            'pause POI への ENTER 直後に bridge が auto-pause で "paused" status を出すはず')
        # cmd_vel を 0 にして dwell 経過させる
        self._set_cmd_vel(0.0, 0.0)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_PAUSED
                           and e.poi.name == 'poi_with_pause')),
            'pause POI + cmd_vel dwell で EVENT_PAUSED が発火するはず')
        # 同 visit 中 (inside のまま) 余分な dwell が続いても 2 回目は出ない
        self._spin_for(0.5)
        paused_count = self._count_events(
            lambda e: (e.event_type == PoiEvent.EVENT_PAUSED
                       and e.poi.name == 'poi_with_pause'))
        self.assertEqual(
            paused_count, 1,
            f'同 visit 内で EVENT_PAUSED は 1 回のみ (実際: {paused_count} 回)')

    def test_re_enter_after_cancel_and_re_route(self):
        """cancel + 再 route で同じ POI から再度 EVENT_ENTER が発火する (lifecycle invariant)。

        bridge は cancel 時 (``mapoi_cancel_cb`` → ``reset_nav_state``) で
        ``current_route_poi_names_`` / ``poi_inside_state_`` を clear する。これは
        再 route 開始時に「既に inside=true で ENTER が発火しない」状態を防ぐための
        保護 (#220)。本 test は cancel 後の 2 度目 ENTER がきちんと発火することで
        この clear が効いていることを示す。

        Note: cancel 直後に robot を遠ざけても EVENT_EXIT は発火しない (cancel 後は
        ``nav_mode_=IDLE`` / route POI set 空で ``is_route_poi=false``、bridge の
        lifecycle 保護で state を書き換えない)。EXIT を経由せずとも内部 state が
        clear されているため、再 route 後の ENTER で十分検証できる。
        """
        self._activate_route()
        self._set_robot_pose(1.0, 0.0)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_goal_only')),
            '前提: 1 回目の ENTER')
        # cancel して route を畳む。cancel result が反映されるまで余裕を持って待つ。
        self._publish_cancel()
        self._wait_for_nav_status('canceled', timeout_sec=2.0)
        self._spin_for(0.3)
        self.received_events.clear()
        self.received_nav_status.clear()
        # 再 route 投入 + 同じ POI への再侵入で ENTER が再発火することを確認。
        # 既に robot は (1.0, 0.0) のまま (= inside POI 上) だが、bridge 内
        # poi_inside_state_ は cancel で clear 済なので新規 ENTER として観測されるはず。
        self._activate_route()
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_goal_only')),
            '再 route 後に同じ POI から再 ENTER が発火するはず')

    def test_consecutive_pause_pois_each_fire_paused(self):
        """同一 route 内で複数の pause POI を順次踏むと各 POI で EVENT_PAUSED が発火する (#236 medium #4)。

        旧 ``corridor_a`` / ``corridor_b`` の暗黙 demo (PR #235 で削除) が担保していた
        「route 内に pause POI が複数並んでも各々で停止する」shape を test に移管する。
        ``route_a`` は ``poi_with_pause`` (0,2) と ``poi_pause_and_custom`` (0,4) の 2 つの
        pause POI を持つ。1 個目の ENTER で auto-pause が FollowWaypoints を cancel するが、
        ``is_paused_`` 中の CANCELED は ``reset_nav_state`` を呼ばない
        (mapoi_nav2_bridge.cpp:672) ため ``current_route_poi_names_`` は維持され、2 個目でも
        ENTER / PAUSED が発火する。2 個目の auto-pause は ``is_paused_`` 既 true で抑止されるが
        (mapoi_nav2_bridge.cpp:1222)、EVENT_PAUSED 自体は per-POI ``poi_paused_published_``
        判定なので独立に発火する。
        """
        self._activate_route()  # route_a
        # --- 1 個目の pause POI ---
        self._set_robot_pose(0.0, 2.0)  # poi_with_pause
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_with_pause')),
            '前提: 1 個目 pause POI への ENTER')
        # 1 個目 ENTER 直後に auto-pause で "paused" status が出るはず (regression guard)。
        self.assertTrue(
            self._wait_for_nav_status('paused', timeout_sec=2.0),
            '1 個目 pause POI の ENTER 直後に auto-pause "paused" status が出るはず')
        self._set_cmd_vel(0.0, 0.0)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_PAUSED
                           and e.poi.name == 'poi_with_pause')),
            '1 個目 pause POI で EVENT_PAUSED が発火するはず')
        # --- 2 個目の pause POI ---
        # cmd_vel を一旦動かして dwell を解除 → 次 POI 到達後に再び停止させる。
        self._set_cmd_vel(0.2, 0.0)
        self._set_robot_pose(0.0, 4.0)  # poi_pause_and_custom
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_ENTER
                           and e.poi.name == 'poi_pause_and_custom')),
            '前提: 2 個目 pause POI への ENTER')
        self._set_cmd_vel(0.0, 0.0)
        self.assertTrue(
            self._wait_for_event(
                lambda e: (e.event_type == PoiEvent.EVENT_PAUSED
                           and e.poi.name == 'poi_pause_and_custom')),
            '2 個目 pause POI でも EVENT_PAUSED が発火するはず (連続 pause)')
        # 各 POI とも PAUSED は 1 回ずつ。
        for poi_name in ('poi_with_pause', 'poi_pause_and_custom'):
            count = self._count_events(
                lambda e, n=poi_name: (e.event_type == PoiEvent.EVENT_PAUSED
                                       and e.poi.name == n))
            self.assertEqual(
                count, 1,
                f"'{poi_name}' の EVENT_PAUSED は 1 回のみ (実際: {count} 回)")

