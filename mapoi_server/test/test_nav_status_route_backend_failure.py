"""``get_route_pois`` backend 異常応答の reject 経路 (#353) を pin する.

issue #353 起票時は ``GetRoutePois.srv`` に ``success`` フィールドが無く、
「service 自体が異常応答を返す」経路 (``on_route_received`` の ``!result->success``)
は決定論的に再現できないとされていた。その後 #342 (merge 済) で response に
``success`` / ``error_message`` が追加され、mock service が ``success=false`` を
返すことで決定論的に再現できるようになった。

``on_route_received`` (mapoi_nav2_bridge.cpp) には reject 経路が 3 つある:
  (a) ``!result`` — service が未応答 (future が null)。実運用ではほぼ到達不能な
      防御分岐で、future の callback 自体が発火しないため launch_test では
      決定論的に再現できない。**本 test のスコープ外** (#353 issue コメント参照)。
  (b) ``!result->success`` — service が明示的に異常応答 (success=false) を返す。
      #342 で追加された経路。本 test のケース 1 が pin する。
  (c) ``success=true`` だが ``pois_list`` が空 ("Route is empty")。
      本 test のケース 2 が pin する。

既存の ``test_nav_status_rejected_paths.py`` は実物の ``mapoi_server`` を使うため、
(b) は「存在しない route 名 → route not found」という mapoi_server 固有の応答経由で
しか踏めず、(c) は fixture に waypoints 空の route を足さないと作れない。bridge は
custom backend とも組む前提 (docs/backend-status.md) なので、server の実装詳細に
依存しない**任意の** success=false 応答 (error_message 内容も server 非依存) に対する
bridge 側の防御を本 test で pin する。mock 差し替えという新しい fixture 構成が
必要なため新規ファイルに分離 (docs/testing-policy.md 3節)。

route コマンドが ``get_route_pois`` 呼び出しに到達する前提条件は
``route_client_->wait_for_service(2s)`` (#355/#356) のみ (goal 側と異なり
``get_pois_info`` の readiness は route 経路では参照されない)。そのため
mock ノードが提供する service は ``mapoi/get_route_pois`` だけで足りる。
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
from nav2_mocks import FakeRoutePoisServer


@launch_testing.markers.keep_alive
def generate_test_description():
    # mapoi_server は起動しない: get_route_pois は FakeRoutePoisServer (mock) が提供する。
    # 実物の mapoi_server では「存在しない route 名」→ success=true & 空リストの経路しか
    # 作れず、success=false の任意異常応答 (経路 (b)) を決定論的に再現できないため。
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


class TestNavStatusRouteBackendFailure(unittest.TestCase):
    """get_route_pois の異常応答経路 (b)(c) で "rejected:<route>" が publish されることを pin する。"""

    NAV_STATUS_WAIT_TIMEOUT = 8.0
    ROUTE_NAME = 'route_backend_failure_test'

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_nav_status_route_backend_failure_node')
        cls.received_nav_status = []

        nav_status_qos = QoSProfile(
            depth=10,
            history=QoSHistoryPolicy.KEEP_LAST,
            reliability=QoSReliabilityPolicy.RELIABLE,
            durability=QoSDurabilityPolicy.TRANSIENT_LOCAL,
        )
        cls.nav_status_sub = cls.node.create_subscription(
            String, 'mapoi/nav/status', cls._nav_status_callback, nav_status_qos)
        cls.route_pub = cls.node.create_publisher(String, 'mapoi/nav/route', 1)

        cls.fake_route_pois_server = FakeRoutePoisServer(
            node_name='fake_route_pois_server_backend_failure')

    @classmethod
    def tearDownClass(cls):
        try:
            cls.fake_route_pois_server.shutdown()
        finally:
            cls.node.destroy_node()
            rclpy.shutdown()

    @classmethod
    def _nav_status_callback(cls, msg):
        cls.received_nav_status.append(msg.data)

    def setUp(self):
        self.fake_route_pois_server.reset()
        self._spin_for(0.3)
        self.received_nav_status.clear()
        self.assertTrue(self._wait_for_subscriber('mapoi/nav/route'),
                        'mapoi_nav2_bridge が mapoi/nav/route を subscribe していない')

    def tearDown(self):
        self.fake_route_pois_server.reset()
        self.received_nav_status.clear()

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

    def _publish_route(self, route_name):
        msg = String()
        msg.data = route_name
        self.route_pub.publish(msg)

    # --- tests ---

    def test_route_backend_error_response_publishes_rejected(self):
        """get_route_pois が success=false + error_message を返す (経路 (b)) 場合、
        "rejected:<route_name>" を publish する。"""
        self.fake_route_pois_server.set_response(
            success=False, error_message='simulated backend failure')
        self._publish_route(self.ROUTE_NAME)
        self.assertTrue(
            self._wait_for_nav_status(f'rejected:{self.ROUTE_NAME}'),
            'get_route_pois の success=false 応答で status が publish されない (#353 regression)')
        self.assertIn(
            self.ROUTE_NAME, self.fake_route_pois_server.requests(),
            'get_route_pois request が mock backend に届いていない')

    def test_route_backend_success_but_empty_publishes_rejected(self):
        """get_route_pois が success=true だが pois_list 空 (経路 (c)) の場合、
        "rejected:<route_name>" を publish する。"""
        self.fake_route_pois_server.set_response(
            success=True, pois_list=[], landmark_pois=[])
        self._publish_route(self.ROUTE_NAME)
        self.assertTrue(
            self._wait_for_nav_status(f'rejected:{self.ROUTE_NAME}'),
            'get_route_pois の success=true & 空 pois_list 応答で status が publish されない'
            ' (#353 regression)')
        self.assertIn(
            self.ROUTE_NAME, self.fake_route_pois_server.requests(),
            'get_route_pois request が mock backend に届いていない')
