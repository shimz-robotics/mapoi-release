"""走行中の reject コマンドが status を上書きしないことの test (#339, Cursor review PR #352 round 2 high).

`publish_rejected_status` は進行中の navigation (`nav_mode_ != IDLE`) がある間は
"rejected" を publish しない。無効な新規コマンド (typo 等) は進行中の navigation の
state / action には一切影響しないため、"rejected" で "navigating" を上書きすると
ロボットは走行を継続しているのに UI が停止したかのように誤表示されるのを防ぐ。

`test_nav_status_rejected_paths.py` は nav_mode_ == IDLE 前提 (reject → "rejected"
が publish される側) を pin しており、本 test はその逆 (走行中は publish されない側)
を NavigateToPose mock で pin する。

#354: 上記の "status を上書きしない" 設計の副作用として、走行中の reject が操作者に
一切通知されない課題が残っていた。`mapoi/nav/command_rejected` は status と独立した
イベント通知で、nav_mode_ に関わらず reject の都度必ず publish される。本 test は
「status は上書きされないが command_rejected は publish される」という #354 の核心
挙動を pin する。
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
            'cmd_vel_msg_type': 'twist',
        }],
    )

    return launch.LaunchDescription([
        mapoi_server_node,
        mapoi_nav2_bridge_node,
        launch_testing.actions.ReadyToTest(),
    ]), {'mapoi_server': mapoi_server_node, 'mapoi_nav2_bridge': mapoi_nav2_bridge_node}


class TestNavStatusRejectedDuringNavigation(unittest.TestCase):
    """走行中は reject で status を上書きしないこと (#339) を pin する。"""

    NAV_STATUS_WAIT_TIMEOUT = 5.0
    GOAL_WAIT_TIMEOUT = 5.0
    # 走行中の goal_pose_poi (test_mapoi_config.yaml 定義)。
    GOAL_POI = 'poi_goal_only'

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_nav_status_rejected_during_nav_node')
        cls.received_nav_status = []

        nav_status_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
        )
        cls.nav_status_sub = cls.node.create_subscription(
            String, 'mapoi/nav/status', cls._nav_status_callback, nav_status_qos)

        # #354: command_rejected は volatile (非 transient_local) QoS。bridge 側
        # publisher (rclcpp::QoS(10), default reliable + volatile) と一致させる。
        cls.received_command_rejected = []
        command_rejected_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.VOLATILE,
        )
        cls.command_rejected_sub = cls.node.create_subscription(
            String, 'mapoi/nav/command_rejected', cls._command_rejected_callback,
            command_rejected_qos)

        cls.goal_pub = cls.node.create_publisher(String, 'mapoi/nav/goal_pose_poi', 1)
        cls.cancel_pub = cls.node.create_publisher(String, 'mapoi/nav/cancel', 1)

        cls.fake_server = FakeNavigateToPoseServer(
            node_name='fake_navigate_to_pose_server_rejected_during_nav')

    @classmethod
    def tearDownClass(cls):
        try:
            cls.fake_server.shutdown()
        finally:
            cls.node.destroy_node()
            rclpy.shutdown()

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    @classmethod
    def _command_rejected_callback(cls, msg):
        cls.received_command_rejected.append(msg.data)

    def setUp(self):
        self._publish_cancel()
        self._spin_for(0.3)
        self.received_nav_status.clear()
        self.received_command_rejected.clear()
        self.fake_server.reset()
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/goal_pose_poi'),
                        'mapoi_nav2_bridge が mapoi/nav/goal_pose_poi を subscribe していない')
        # command_rejected は volatile QoS でリプレイされないため、matching 完了前の
        # reject 取りこぼしを防ぐべく publisher の存在も gate する (#354 review medium)。
        self.assertTrue(self._wait_for_publisher('mapoi/nav/command_rejected'),
                        'mapoi_nav2_bridge が mapoi/nav/command_rejected を publish していない')

    def tearDown(self):
        self._publish_cancel()
        self._spin_for(0.4)
        self.received_nav_status.clear()
        self.received_command_rejected.clear()
        self.fake_server.reset()

    # --- helpers ---

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

    def _wait_for_publisher(self, topic_name, timeout_sec=5.0):
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if self.node.count_publishers(topic_name) > 0:
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

    def _wait_for_command_rejected(self, target, timeout_sec=None):
        if timeout_sec is None:
            timeout_sec = self.NAV_STATUS_WAIT_TIMEOUT
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            if target in self.received_command_rejected:
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

    def _publish_goal(self, poi_name):
        msg = String()
        msg.data = poi_name
        self.goal_pub.publish(msg)

    def _publish_cancel(self):
        msg = String()
        msg.data = ''
        self.cancel_pub.publish(msg)

    def _activate_goal(self, poi_name):
        for _ in range(5):
            self._publish_goal(poi_name)
            if self._wait_for_goal_count(1, timeout_sec=2.0):
                return
        self.fail(f"Go '{poi_name}' で NavigateToPose goal が mock に届かなかった")

    # --- tests ---

    def test_invalid_goal_during_navigation_does_not_overwrite_status(self):
        """走行中に typo goal を送っても "rejected" で status が上書きされない。"""
        self._activate_goal(self.GOAL_POI)
        self.assertTrue(
            self._wait_for_nav_status('navigating:' + self.GOAL_POI),
            '前提となる navigating status が来ていない')
        self.received_nav_status.clear()

        # 走行中に typo goal を送る。goal 自体は mock に届かない (POI 解決前に reject) はず。
        self._publish_goal('poi_typo_does_not_exist')
        self._spin_for(1.5)

        rejected_msgs = [s for s in self.received_nav_status if s.startswith('rejected')]
        self.assertEqual(
            rejected_msgs, [],
            f'走行中の reject で status が上書きされた (#339 regression): {rejected_msgs}')
        # 走行中の goal はキャンセルされず継続しているはず (mock に新規 goal が届いていない)。
        self.assertEqual(
            len(self.fake_server.goals_xy()), 1,
            '走行中の goal が意図せず変化した')
        # #354: status は上書きされない一方、command_rejected イベントは publish される
        # (= 操作者が走行中の typo goal に気づける核心挙動)。
        self.assertTrue(
            self._wait_for_command_rejected('poi_typo_does_not_exist', timeout_sec=2.0),
            '走行中の reject で command_rejected が publish されなかった (#354 regression)')
