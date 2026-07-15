"""「受理したが未実行」の失敗経路で mapoi/nav/status が更新されない bug の再現 test (#339).

修正前は下記の経路が ERROR/WARN ログのみで mapoi/nav/status を publish せず、
WebUI / RViz panel には直前の status (`succeeded` / `navigating` 等) が居座って
操作者が誤操作 (typo 等) に気づけなかった。本 test はこのうち mapoi_server + mapoi_nav2_bridge
の起動だけ (Nav2 action mock 不要) で再現できる 3 経路を pin する:

- goal POI 名が見つからない (typo)
- landmark タグ付き POI を goal 指定 (#85 の reject)
- route の waypoints が空 (存在しない route 名を含む)

いずれも修正後は `"rejected:<target>"` を publish する。

#354: `test_goal_poi_not_found_publishes_rejected` には、IDLE 時 (nav_mode_ ==
IDLE) の reject でも `mapoi/nav/command_rejected` イベントが publish されることの
assert を追加している。command_rejected は nav_mode_ に関わらず常に publish される
(status 側の suppress ロジックとは独立) ため、IDLE / 走行中いずれでも発火する
挙動の IDLE 側を本ファイルで、走行中側は `test_nav_status_rejected_during_navigation.py`
で pin している。
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


class TestNavStatusRejectedPaths(unittest.TestCase):
    """受理前 reject 経路の status publish (#339) を pin する。"""

    NAV_STATUS_WAIT_TIMEOUT = 5.0

    # test_mapoi_config.yaml 定義 (landmark tag、waypoint/pause とは排他)。
    LANDMARK_POI = 'poi_landmark_listed'

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_nav_status_rejected_node')
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
        cls.route_pub = cls.node.create_publisher(String, 'mapoi/nav/route', 1)

    @classmethod
    def tearDownClass(cls):
        cls.node.destroy_node()
        rclpy.shutdown()

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    @classmethod
    def _command_rejected_callback(cls, msg):
        cls.received_command_rejected.append(msg.data)

    def setUp(self):
        self._spin_for(0.3)
        self.received_nav_status.clear()
        self.received_command_rejected.clear()
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/goal_pose_poi'),
                        'mapoi_nav2_bridge が mapoi/nav/goal_pose_poi を subscribe していない')
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/route'),
                        'mapoi_nav2_bridge が mapoi/nav/route を subscribe していない')
        # command_rejected は volatile QoS (イベント通知、リプレイ無し) なので、publisher と
        # テスト側 subscriber の matching 完了前に reject が発火すると黙って失われる。
        # subscriber 側の発見だけでなく publisher の存在も gate する (#354 review medium)。
        self.assertTrue(self._wait_for_publisher('mapoi/nav/command_rejected'),
                        'mapoi_nav2_bridge が mapoi/nav/command_rejected を publish していない')

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

    def _publish_goal(self, poi_name):
        msg = String()
        msg.data = poi_name
        self.goal_pub.publish(msg)

    def _publish_route(self, route_name):
        msg = String()
        msg.data = route_name
        self.route_pub.publish(msg)

    def _wait_for_command_rejected(self, target, timeout_sec=None):
        if timeout_sec is None:
            timeout_sec = self.NAV_STATUS_WAIT_TIMEOUT
        end = time.monotonic() + timeout_sec
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.1)
            if target in self.received_command_rejected:
                return True
        return False

    # --- tests ---

    def test_goal_poi_not_found_publishes_rejected(self):
        """typo 等で存在しない POI 名を goal 指定した場合、"rejected:<name>" を publish する。"""
        self._publish_goal('poi_typo_does_not_exist')
        self.assertTrue(
            self._wait_for_nav_status('rejected:poi_typo_does_not_exist'),
            '存在しない goal POI 名で status が publish されない (#339 regression)')
        # #354: IDLE 時の reject でも command_rejected イベントが publish される
        # (nav_mode_ に関わらず常に publish する仕様の IDLE 側を pin)。
        self.assertTrue(
            self._wait_for_command_rejected('poi_typo_does_not_exist', timeout_sec=2.0),
            'IDLE 時の reject で command_rejected が publish されなかった (#354 regression)')

    def test_landmark_goal_publishes_rejected(self):
        """landmark タグ付き POI を goal 指定した場合 (#85 reject)、"rejected:<name>" を publish する。"""
        self._publish_goal(self.LANDMARK_POI)
        self.assertTrue(
            self._wait_for_nav_status(f'rejected:{self.LANDMARK_POI}'),
            'landmark POI を goal 指定した reject で status が publish されない (#339 regression)')

    def test_empty_route_publishes_rejected(self):
        """存在しない route 名 (= waypoints 空) の場合、"rejected:<name>" を publish する。"""
        self._publish_route('route_does_not_exist')
        self.assertTrue(
            self._wait_for_nav_status('rejected:route_does_not_exist'),
            '空 route で status が publish されない (#339 regression)')
