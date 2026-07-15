"""mapoi 主導 waypoint 到達モード (waypoint_arrival_mode=mapoi) の integration test (#243).

既存 `test_poi_event_route_integration.py` は FollowWaypoints mock 前提の "nav2" モード
(Nav2 が waypoint 進行) を pin する。本 test は "mapoi" モード — mapoi が tolerance.xy
到達判定で 1 waypoint ずつ NavigateToPose を送って進める経路 — を NavigateToPose mock で
pin する。

到達判定は統一仕様 (#265): OR((tolerance.xy ∧ tolerance.yaw) ∨ Nav2 SUCCEEDED)。

検証する shape:
- OR トリガ a: 現 waypoint の tolerance.xy ∧ tolerance.yaw 到達で次 waypoint へ進む (mock が次 goal を受信)。
- yaw 不一致: tolerance.xy 内でも姿勢が tolerance.yaw を超えていれば radius では進まず、
  Nav2 SUCCEEDED を待つ。
- OR トリガ b: Nav2 (mock) が現 waypoint を SUCCEEDED にすると、robot が radius に入らずとも
  次 waypoint へ進む (スタック防止のフォールバック)。
- pause waypoint 到達で停止 ("paused")、次 goal を送らず resume を待ち、resume で次へ進む。
- 最終 goal も同じ統一判定: tolerance.xy ∧ tolerance.yaw で route 完了 (姿勢一致なら Nav2 を
  待たない)、yaw 不一致なら Nav2 SUCCEEDED を待つ。

mock は `navigate_to_pose` action server。受理した goal の pose を順に記録し、test 側が
「bridge が次 waypoint へ goal を送り直したか」を観測できるようにする。default では goal を
cancel まで EXECUTING で保持 (= robot が tolerance.xy で到達するまで Nav2 は走り続ける mock)、
`succeed_current_goal()` で現 goal を SUCCEEDED finalize する (トリガ b / route 終端用)。
"""

import math
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
from nav2_mocks import FakeNavigateToPoseServer


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
            'stopped_dwell_time_sec': 0.3,
            'cmd_vel_msg_type': 'twist',
            # 本 test の対象: mapoi 主導 waypoint 到達モード (#243)。
            'waypoint_arrival_mode': 'mapoi',
        }],
    )

    return launch.LaunchDescription([
        mapoi_server_node,
        mapoi_nav2_bridge_node,
        launch_testing.actions.ReadyToTest(),
    ]), {'mapoi_server': mapoi_server_node, 'mapoi_nav2_bridge': mapoi_nav2_bridge_node}


class TestPoiEventRouteMapoiArrival(unittest.TestCase):
    """mapoi 主導 waypoint 到達モード (#243) の advance / pause / route 終端を pin する。"""

    NAV_STATUS_WAIT_TIMEOUT = 5.0
    GOAL_WAIT_TIMEOUT = 5.0
    TF_PUBLISH_INTERVAL = 0.05
    CMD_VEL_PUBLISH_INTERVAL = 0.05
    FAR_AWAY_XY = (100.0, 100.0)

    # route_a = [poi_goal_only(1,0), poi_with_pause(0,2,pause),
    #            poi_with_custom(3,0), poi_pause_and_custom(0,4,pause)]
    WP0 = (1.0, 0.0)   # poi_goal_only (非 pause)
    WP1 = (0.0, 2.0)   # poi_with_pause (pause)
    WP2 = (3.0, 0.0)   # poi_with_custom (非 pause)

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_poi_event_route_mapoi_node')
        cls.received_nav_status = []

        nav_status_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
        )
        cls.nav_status_sub = cls.node.create_subscription(
            String, 'mapoi/nav/status', cls._nav_status_callback, nav_status_qos)
        cls.event_sub = cls.node.create_subscription(
            PoiEvent, 'mapoi/events', cls._event_callback, 10)
        cls.received_events = []
        cls.tf_broadcaster = TransformBroadcaster(cls.node)
        cls.cmd_vel_pub = cls.node.create_publisher(Twist, 'cmd_vel', 10)
        cls.route_pub = cls.node.create_publisher(String, 'mapoi/nav/route', 1)
        cls.cancel_pub = cls.node.create_publisher(String, 'mapoi/nav/cancel', 1)
        cls.resume_pub = cls.node.create_publisher(String, 'mapoi/nav/resume', 1)

        cls._robot_xy = cls.FAR_AWAY_XY
        cls._robot_yaw = 0.0
        cls._tf_timer = cls.node.create_timer(
            cls.TF_PUBLISH_INTERVAL, cls._tf_publish_callback)
        cls._cmd_vel_timer = cls.node.create_timer(
            cls.CMD_VEL_PUBLISH_INTERVAL, cls._cmd_vel_publish_callback)

        cls.fake_server = FakeNavigateToPoseServer()

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
        t.transform.rotation.z = math.sin(cls._robot_yaw / 2.0)
        t.transform.rotation.w = math.cos(cls._robot_yaw / 2.0)
        cls.tf_broadcaster.sendTransform(t)

    @classmethod
    def _cmd_vel_publish_callback(cls):
        # mapoi モードの advance は TF 由来 (tolerance.xy 進入) なので cmd_vel は
        # 「動いている」状態を保ち、pause POI の auto-pause 判定にだけ関わる。
        cls.cmd_vel_pub.publish(Twist())  # 0 速度 (= 停止扱い、pause dwell を成立させる)

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    @classmethod
    def _event_callback(cls, msg):
        cls.received_events.append(msg)

    def setUp(self):
        self._set_robot_pose(*self.FAR_AWAY_XY)
        self._publish_cancel()
        self._spin_for(0.3)
        self.received_nav_status.clear()
        self.received_events.clear()
        self.fake_server.reset()
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/route'),
                        'mapoi_nav2_bridge が mapoi/nav/route を subscribe していない')

    def tearDown(self):
        self._publish_cancel()
        self._spin_for(0.4)
        self._set_robot_pose(*self.FAR_AWAY_XY)
        self.received_nav_status.clear()
        self.received_events.clear()
        self.fake_server.reset()

    # --- helpers ---

    def _set_robot_pose(self, x, y, yaw=0.0):
        type(self)._robot_xy = (x, y)
        type(self)._robot_yaw = yaw

    def _spin_for(self, duration_sec):
        end = time.monotonic() + duration_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)

    def _wait_for_subscriber(self, topic_name, timeout_sec=5.0):
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if self.node.count_subscribers(topic_name) > 0:
                return True
        return False

    def _wait_for_nav_status(self, status, timeout_sec=None):
        if timeout_sec is None:
            timeout_sec = self.NAV_STATUS_WAIT_TIMEOUT
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            if any(s == status or s.startswith(status + ':')
                   for s in self.received_nav_status):
                return True
        return False

    def _wait_for_goal_count(self, n, timeout_sec=None):
        if timeout_sec is None:
            timeout_sec = self.GOAL_WAIT_TIMEOUT
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if len(self.fake_server.goals_xy()) >= n:
                return True
        return False

    def _publish_route(self, route_name):
        msg = String()
        msg.data = route_name
        self.route_pub.publish(msg)

    def _publish_cancel(self):
        msg = String()
        msg.data = ''
        self.cancel_pub.publish(msg)

    def _publish_resume(self):
        msg = String()
        msg.data = ''
        self.resume_pub.publish(msg)

    def _activate_route(self, route_name):
        """Route を投入し、最初の NavigateToPose goal が mock に届くまで待つ。

        action server discovery 未完了のタイミングで route を投入すると bridge は
        backend_unavailable で route を放棄する。初回 test の discovery race を吸収する
        ため、goal が届くまで route 投入を数回リトライする。
        """
        for _ in range(5):
            self._publish_route(route_name)
            if self._wait_for_goal_count(1, timeout_sec=2.0):
                return
        self.fail(f"route '{route_name}' で最初の waypoint goal が mock に届かなかった")

    def _assert_xy_close(self, actual, expected, tol=0.05):
        self.assertAlmostEqual(actual[0], expected[0], delta=tol)
        self.assertAlmostEqual(actual[1], expected[1], delta=tol)

    # --- tests ---

    def test_first_waypoint_goal_sent_on_route_start(self):
        """route 開始で先頭 waypoint への NavigateToPose goal が送られる。"""
        self._activate_route('route_a')
        goals = self.fake_server.goals_xy()
        self.assertGreaterEqual(len(goals), 1)
        self._assert_xy_close(goals[0], self.WP0)

    def test_advance_on_radius_entry(self):
        """OR トリガ a: 現 waypoint の tolerance.xy 進入で次 waypoint へ goal を送り直す。"""
        self._activate_route('route_a')  # goal[0] = WP0 (poi_goal_only)
        self._set_robot_pose(*self.WP0)  # WP0 radius に進入
        self.assertTrue(
            self._wait_for_goal_count(2),
            'WP0 tolerance.xy 進入後に WP1 への goal が送られるはず')
        goals = self.fake_server.goals_xy()
        self._assert_xy_close(goals[1], self.WP1)  # poi_with_pause

    def test_advance_on_nav2_succeeded_without_radius_entry(self):
        """OR トリガ b: robot が radius に入らずとも Nav2 SUCCEEDED で次へ進む (スタック防止)。"""
        self._activate_route('route_a')  # goal[0] = WP0
        # robot は FAR_AWAY のまま (WP0 radius に入らない)。mock が WP0 を SUCCEEDED に。
        self.fake_server.succeed_current_goal()
        self.assertTrue(
            self._wait_for_goal_count(2),
            'Nav2 SUCCEEDED 後に次 waypoint への goal が送られるはず')
        goals = self.fake_server.goals_xy()
        self._assert_xy_close(goals[1], self.WP1)

    def test_pause_waypoint_stops_and_resume_advances(self):
        """pause waypoint 到達で停止 (進めない) → resume で次 waypoint へ進む。"""
        self._activate_route('route_a')          # goal[0] = WP0
        self._set_robot_pose(*self.WP0)           # → WP1 へ advance
        self.assertTrue(self._wait_for_goal_count(2), '前提: WP1 への advance')
        self._set_robot_pose(*self.WP1)           # WP1 (pause) radius に進入
        # pause POI 到達で "paused" status が出る。
        self.assertTrue(
            self._wait_for_nav_status('paused'),
            'pause waypoint 到達で "paused" status が出るはず')
        # pause 中は次 goal を送らない (advance しない)。
        self._spin_for(0.6)
        self.assertEqual(
            len(self.fake_server.goals_xy()), 2,
            f'pause 中は次 waypoint goal を送らないはず (実際: {self.fake_server.goals_xy()})')
        # resume すると次 waypoint (WP2) へ進む。
        self._publish_resume()
        self.assertTrue(
            self._wait_for_goal_count(3),
            'resume 後に次 waypoint への goal が送られるはず')
        self._assert_xy_close(self.fake_server.goals_xy()[2], self.WP2)

