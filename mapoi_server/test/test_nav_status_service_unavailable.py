"""service 未 ready 時の goal / route 経路が "rejected" を publish することの再現 test (#339 / #355).

`mapoi_server` を起動せず `mapoi_nav2_bridge` 単独で goal_pose_poi を送ると、
`pois_info_client_->wait_for_service(2s)` が timeout し、POI 名の解決すらできない。
この経路も他の「受理前 reject」経路と同様に status を publish する必要がある。
route コマンドも同型: `get_route_pois` が unreachable なら
`route_client_->wait_for_service(2s)` timeout で reject する (#355)。

`test_nav_status_rejected_paths.py` は mapoi_server 込みの構成のため、この経路
(service 自体が unreachable) は別 launch fixture (mapoi_server を起動しない) で
分離して pin する。
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
from std_msgs.msg import String


@launch_testing.markers.keep_alive
def generate_test_description():
    # mapoi_server は起動しない: get_pois_info service が unreachable な状態を作る。
    mapoi_nav2_bridge_node = launch_ros.actions.Node(
        package='mapoi_server',
        executable='mapoi_nav2_bridge',
        name='mapoi_nav2_bridge',
        parameters=[{
            'cmd_vel_msg_type': 'twist',
        }],
    )

    return launch.LaunchDescription([
        mapoi_nav2_bridge_node,
        launch_testing.actions.ReadyToTest(),
    ]), {'mapoi_nav2_bridge': mapoi_nav2_bridge_node}


class TestNavStatusServiceUnavailable(unittest.TestCase):
    """get_pois_info / get_route_pois service 未 ready での reject (#339 / #355) を pin する。"""

    # wait_for_service(2s) の timeout + spin overhead を見込む。
    NAV_STATUS_WAIT_TIMEOUT = 8.0

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_nav_status_service_unavailable_node')
        cls.received_nav_status = []

        nav_status_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
        )
        cls.nav_status_sub = cls.node.create_subscription(
            String, 'mapoi/nav/status', cls._nav_status_callback, nav_status_qos)
        cls.goal_pub = cls.node.create_publisher(String, 'mapoi/nav/goal_pose_poi', 1)
        cls.route_pub = cls.node.create_publisher(String, 'mapoi/nav/route', 1)

    @classmethod
    def tearDownClass(cls):
        cls.node.destroy_node()
        rclpy.shutdown()

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    def setUp(self):
        self._spin_for(0.3)
        self.received_nav_status.clear()
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/goal_pose_poi'),
                        'mapoi_nav2_bridge が mapoi/nav/goal_pose_poi を subscribe していない')

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

    def test_goal_with_pois_info_service_unavailable_publishes_rejected(self):
        """mapoi_server 不在 (get_pois_info unreachable) でも "rejected:<name>" を publish する。"""
        msg = String()
        msg.data = 'poi_goal_only'
        self.goal_pub.publish(msg)
        self.assertTrue(
            self._wait_for_nav_status('rejected:poi_goal_only'),
            'get_pois_info service 未 ready で status が publish されない (#339 regression)')

    def test_route_with_route_pois_service_unavailable_publishes_rejected(self):
        """mapoi_server 不在 (get_route_pois unreachable) でも "rejected:<name>" を publish する (#355)。"""
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/route'),
                        'mapoi_nav2_bridge が mapoi/nav/route を subscribe していない')
        msg = String()
        msg.data = 'route_a'
        self.route_pub.publish(msg)
        self.assertTrue(
            self._wait_for_nav_status('rejected:route_a'),
            'get_route_pois service 未 ready で status が publish されない (#355 regression)')
