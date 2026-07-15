# #211: mapoi/initialpose_poi の publisher を mapoi_server 1 つに集約したこと
# (= mapoi_nav2_bridge / WebUI / RViz panel の直接 publish 廃止) を pin する launch_test。
#
# 旧 #154 test (test_reload_clears_initialpose.py, #287 で削除) は mapoi_server 単一 writer
# 前提で、複数 writer の per-writer transient_local cache が生む stale 競合 (#211 の本丸) を
# 捕捉できなかった。本 test は別アプローチで「writer が構造的に 1 本である」ことを直接 pin する:
#   (1) mapoi_server + mapoi_nav2_bridge を起動し、mapoi/initialpose_poi の publisher 数が
#       ちょうど 1 (= mapoi_server のみ) であることを確認する。修正前は nav2_bridge も
#       publisher を持つため 2 になり RED、提案A 適用後は nav2_bridge が service client に
#       なり 1 で GREEN。直接 writer の再追加 (= #211 race の再混入) を恒久的に検知する。
#   (2) 新 service request_initial_pose 経由で POI / clear が単一 writer から publish され、
#       clear が latched 値を last-write-wins で上書きする (late subscriber に stale が残らない)
#       ことを behavioral に確認する。
import os
import unittest

import launch
import launch_ros.actions
import launch_testing
import launch_testing.actions
import launch_testing.markers

import rclpy
from rclpy.qos import QoSProfile, DurabilityPolicy, HistoryPolicy, ReliabilityPolicy
from mapoi_interfaces.msg import InitialPoseRequest
from mapoi_interfaces.srv import RequestInitialPose
from ament_index_python.packages import get_package_share_directory


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
    # nav2_bridge を一緒に起動し、「nav2_bridge が initialpose_poi の writer を持たない」こと
    # (#211 提案A の単一 writer 不変条件) を確認する。Nav2 不在でも起動・spin する。
    mapoi_nav2_bridge_node = launch_ros.actions.Node(
        package='mapoi_server',
        executable='mapoi_nav2_bridge',
        name='mapoi_nav2_bridge',
    )

    return launch.LaunchDescription([
        mapoi_server_node,
        mapoi_nav2_bridge_node,
        launch_testing.actions.ReadyToTest(),
    ]), {
        'mapoi_server': mapoi_server_node,
        'mapoi_nav2_bridge': mapoi_nav2_bridge_node,
    }


def _transient_local_qos():
    # publisher 側 (rclcpp::QoS(1).transient_local()) と互換: reliable + keep_last(1) + transient_local。
    return QoSProfile(
        depth=1,
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        history=HistoryPolicy.KEEP_LAST,
    )


class TestMultiwriterInitialposeNoStale(unittest.TestCase):
    """mapoi/initialpose_poi の writer が mapoi_server 1 本であること、および新 service 経由の
    publish / clear が last-write-wins で効くことを確認する (#211)。"""

    SPIN_TIMEOUT = 10.0
    TOPIC = 'mapoi/initialpose_poi'

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_multiwriter_initialpose_node')
        cls.request_client = cls.node.create_client(RequestInitialPose, 'mapoi/request_initial_pose')

    @classmethod
    def tearDownClass(cls):
        cls.node.destroy_node()
        rclpy.shutdown()

    def _spin_until(self, predicate, timeout=None):
        timeout = timeout if timeout is not None else self.SPIN_TIMEOUT
        end = self.node.get_clock().now().nanoseconds + int(timeout * 1e9)
        while self.node.get_clock().now().nanoseconds < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if predicate():
                return True
        return False

    def _take_one_latched(self, timeout=None):
        """新規 subscription を作って transient_local の latched 値を 1 個受信する。

        毎回新しい subscription を作って destroy することで、後起動の subscriber が
        latched 値をどう受信するかをシナリオとして直接検証できる (deleted #154 test と同手法)。
        """
        received = []
        sub = self.node.create_subscription(
            InitialPoseRequest, self.TOPIC,
            lambda msg: received.append(msg),
            _transient_local_qos())
        try:
            got = self._spin_until(lambda: len(received) >= 1, timeout=timeout)
            self.assertTrue(got, "Latched message not received within timeout")
            return received[0]
        finally:
            self.node.destroy_subscription(sub)

    def test_a_single_writer_on_initialpose_topic(self):
        """mapoi_server + mapoi_nav2_bridge 起動時、mapoi/initialpose_poi の publisher は
        mapoi_server の 1 本のみ。修正前 (nav2_bridge も直接 publish) では 2 で RED、
        提案A 適用後は 1 で GREEN。直接 writer の再追加を恒久検知する。"""
        # 両ノードが discovery 済みであることを確認してから publisher 数を見る:
        #  - mapoi_server      : mapoi/config_path を publish
        #  - mapoi_nav2_bridge : mapoi/nav/backend_status を 1Hz publish
        both_up = self._spin_until(
            lambda: self.node.count_publishers('mapoi/config_path') >= 1
            and self.node.count_publishers('mapoi/nav/backend_status') >= 1)
        self.assertTrue(
            both_up,
            "mapoi_server / mapoi_nav2_bridge did not both become discoverable in time")
        # nav2_bridge の全 endpoint が伝播するよう少し dwell してから assert する (同一 participant
        # の endpoint はまとめて announce されるため、backend_status が見えた時点で initialpose_poi
        # publisher があれば一緒に見えるはず。念のため追加 spin)。
        self._spin_until(lambda: False, timeout=2.0)
        count = self.node.count_publishers(self.TOPIC)
        self.assertEqual(
            count, 1,
            f"Expected exactly 1 publisher on {self.TOPIC} (mapoi_server only), got {count}. "
            "複数なら直接 writer が再混入し #211 の per-writer stale 競合が復活している。")

    def test_b_service_publish_and_clear_last_write_wins(self):
        """新 service request_initial_pose 経由で POI を publish → late subscriber が受信し、
        続く clear (poi_name='') が latched 値を上書きして後起動 subscriber が stale POI を
        受け取らない (= 単一 writer の last-write-wins、#211 本旨) ことを確認する。"""
        self.assertTrue(
            self.request_client.wait_for_service(timeout_sec=self.SPIN_TIMEOUT),
            "request_initial_pose service did not become available")

        # 1) POI を service 経由で publish。map_name は fixture の現在 map ('.') と一致させる
        #    (#299: 非空 map_name は現在 map と一致必須になった。一致する非空 map_name の
        #    成功経路もここで担保する)。
        req = RequestInitialPose.Request()
        req.map_name = '.'
        req.poi_name = 'poi_with_pause'
        future = self.request_client.call_async(req)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=self.SPIN_TIMEOUT)
        result = future.result()
        self.assertIsNotNone(result, "request_initial_pose (POI) did not complete")
        self.assertTrue(result.success, "request_initial_pose (POI) returned success=False")

        # late subscriber は latched POI を受信する。
        msg = self._take_one_latched()
        self.assertEqual(
            msg.poi_name, 'poi_with_pause',
            f"Expected latched poi_name 'poi_with_pause', got '{msg.poi_name}'")

        # 2) clear (poi_name='') を同じ単一 writer 経由で publish。
        req_clear = RequestInitialPose.Request()
        req_clear.map_name = '.'
        req_clear.poi_name = ''
        future = self.request_client.call_async(req_clear)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=self.SPIN_TIMEOUT)
        result = future.result()
        self.assertIsNotNone(result, "request_initial_pose (clear) did not complete")
        self.assertTrue(result.success, "request_initial_pose (clear) returned success=False")

        # clear 後に subscribe する late subscriber は stale POI ではなく skip message を受信する。
        # 単一 writer なので clear が latched 値を確実に上書きする (#211: 提案A の核心)。
        msg = self._take_one_latched()
        self.assertEqual(
            msg.poi_name, '',
            "Expected empty poi_name after clear (single-writer last-write-wins), "
            f"got '{msg.poi_name}'")

    def test_c_service_rejects_mismatched_map(self):
        """#299: 現在 map と不一致な非空 map_name の要求は success=False で reject され、
        latched 値も更新されない (map switch 過渡窓の stale request 遮断)。"""
        self.assertTrue(
            self.request_client.wait_for_service(timeout_sec=self.SPIN_TIMEOUT),
            "request_initial_pose service did not become available")

        # 既知 sentinel を先に latch させ、reject 後の latched 値を完全一致で assert できる
        # ようにする (直前 test の状態に依存せず、reject 経路が clear や別値を publish する
        # 誤実装も検出できる、Codex review round 1 low 対応)。
        sentinel = RequestInitialPose.Request()
        sentinel.map_name = '.'
        sentinel.poi_name = 'poi_with_pause'
        future = self.request_client.call_async(sentinel)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=self.SPIN_TIMEOUT)
        result = future.result()
        self.assertIsNotNone(result, "request_initial_pose (sentinel) did not complete")
        self.assertTrue(result.success, "sentinel の publish に失敗")
        msg = self._take_one_latched()
        self.assertEqual((msg.map_name, msg.poi_name), ('.', 'poi_with_pause'))

        req = RequestInitialPose.Request()
        req.map_name = 'other_map'  # fixture の現在 map は '.'
        req.poi_name = 'should_not_latch'
        future = self.request_client.call_async(req)
        rclpy.spin_until_future_complete(self.node, future, timeout_sec=self.SPIN_TIMEOUT)
        result = future.result()
        self.assertIsNotNone(result, "request_initial_pose (mismatch) did not complete")
        self.assertFalse(
            result.success,
            "現在 map と不一致な map_name が reject されない (#299 regression)")
        self.assertNotEqual(result.error_message, '',
                            "reject 時の error_message が空")

        # reject は publish を伴わない: latched 値は sentinel が完全一致で残る。
        msg = self._take_one_latched()
        self.assertEqual(
            (msg.map_name, msg.poi_name), ('.', 'poi_with_pause'),
            "reject 経路で latched 値が変化している (#299 regression): "
            f"({msg.map_name!r}, {msg.poi_name!r})")
