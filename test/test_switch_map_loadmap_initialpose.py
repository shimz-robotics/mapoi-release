"""operator map switch の LoadMap → request_initial_pose E2E launch_test (#299 テスト残課題).

「initial pose POI 要求は LoadMap 成功後のみ」(#149 / #184 の不変条件、PR #298 で
timing gate は nav2_bridge 所有のまま wire-publish だけ mapoi_server に移譲) を、
mapoi_server + mapoi_nav2_bridge + FakeMapServer (load_map service mock) の実配線で pin する。
#211 時点の 3 系統レビューが一致して挙げた最高価値の未カバー経路。

検証する shape:
- 成功経路: `mapoi/nav/switch_map` → select_map → LoadMap 成功 → `mapoi/initialpose_poi` に
  {切替先 map, その POI list 先頭} が publish される。順序は
  (1) select_map 由来の clear が先行し、(2) LoadMap **応答完了** が initialpose publish
  より前 (FakeMapServer の応答遅延注入で、LoadMap pending 中に先行 publish する
  regression も検出できる)。
- 失敗経路: LoadMap が失敗すると `map_switch_failed` になり、非空 poi_name の
  initialpose publish は発生しない (latched 値は select_map の clear のまま)。
  切替開始以降の sample だけを index 基準で assert するため、直前の latched 状態
  (= 他 test の実行有無) に依存しない。
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
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from ament_index_python.packages import get_package_share_directory
from std_msgs.msg import String
from mapoi_interfaces.msg import InitialPoseRequest
from nav2_mocks import FakeMapServer


@launch_testing.markers.keep_alive
def generate_test_description():
    pkg_share = get_package_share_directory('mapoi_server')
    maps_dir = os.path.join(pkg_share, 'test', 'maps')

    mapoi_server_node = launch_ros.actions.Node(
        package='mapoi_server',
        executable='mapoi_server',
        name='mapoi_server',
        parameters=[{
            'maps_path': maps_dir,
            'map_name': 'test_map_a',
            'config_file': 'mapoi_config.yaml',
        }],
    )
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


def _transient_local_qos(depth):
    # publisher 側 (rclcpp::QoS(1).transient_local()) と互換。subscriber 側 depth は
    # 履歴の取りこぼし防止のため広めに取る (深さ非対称は QoS 互換)。
    return QoSProfile(
        depth=depth,
        reliability=ReliabilityPolicy.RELIABLE,
        durability=DurabilityPolicy.TRANSIENT_LOCAL,
        history=HistoryPolicy.KEEP_LAST,
    )


class TestSwitchMapLoadMapInitialPose(unittest.TestCase):
    """switch_map → LoadMap → initialpose_poi の E2E 順序契約を pin する。"""

    SPIN_TIMEOUT = 15.0

    @classmethod
    def setUpClass(cls):
        rclpy.init()
        cls.node = rclpy.create_node('test_switch_map_loadmap_node')
        # fixture の map: セクション (node_name: /map_server) に合わせた load_map mock。
        cls.map_server = FakeMapServer(service_name='/map_server/load_map')
        cls.switch_pub = cls.node.create_publisher(String, 'mapoi/nav/switch_map', 1)

    @classmethod
    def tearDownClass(cls):
        cls.map_server.shutdown()
        cls.node.destroy_node()
        rclpy.shutdown()

    def setUp(self):
        # test ごとに新しい subscription を張る。join 時に transient_local の latched 値を
        # 1 個受け、それ以降は live sample が (受信時刻付きで) 追記される。
        self.initialpose_samples = []
        self.initialpose_sub = self.node.create_subscription(
            InitialPoseRequest, 'mapoi/initialpose_poi',
            lambda msg: self.initialpose_samples.append(
                (msg.map_name, msg.poi_name, time.monotonic())),
            _transient_local_qos(10))
        self.nav_statuses = []
        self.status_sub = self.node.create_subscription(
            String, 'mapoi/nav/status',
            lambda msg: self.nav_statuses.append(msg.data),
            _transient_local_qos(10))
        self.map_server.reset()

    def tearDown(self):
        self.node.destroy_subscription(self.initialpose_sub)
        self.node.destroy_subscription(self.status_sub)

    def _spin_until(self, predicate, timeout=None):
        timeout = timeout if timeout is not None else self.SPIN_TIMEOUT
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            rclpy.spin_once(self.node, timeout_sec=0.05)
            if predicate():
                return True
        return False

    def _switch_map(self, map_name):
        # bridge の subscription (depth=1) が discovery 済みであることを確認してから publish。
        self.assertTrue(
            self._spin_until(
                lambda: self.node.count_subscribers('mapoi/nav/switch_map') >= 1),
            'mapoi_nav2_bridge の switch_map subscription が discovery されない')
        msg = String()
        msg.data = map_name
        self.switch_pub.publish(msg)

    def test_a_switch_success_publishes_initialpose_after_loadmap(self):
        """成功経路: LoadMap 応答完了 → {切替先 map, 先頭 POI} publish の順序を pin する。"""
        # 起動直後の latched 値 (#144 の起動時 publish) を受けてから切替を始める。
        # (本 test は起動後最初に実行される前提。method 名の a/b が実行順を規定する。)
        self.assertTrue(
            self._spin_until(lambda: len(self.initialpose_samples) >= 1),
            '起動時 latched initialpose が受信できない')
        self.assertEqual(self.initialpose_samples[0][:2], ('test_map_a', 'map_a_start'))

        # LoadMap 応答を 0.2s 遅延させる。「応答完了前に initialpose を publish する」
        # regression が入った場合、t_poi が応答完了時刻より ~0.2s 早くなり下の assertLess で
        # 確実に検出できる (scheduling noise より 2 桁大きい)。bridge 側
        # spin_until_future_complete の hard timeout 1s に対し 0.8s の余白を残し、
        # 高負荷 CI での LoadMap timeout flake を避ける (Codex review round 2 low)。
        self.map_server.set_response_delay(0.2)
        self._switch_map('test_map_b')

        # 切替完了: 非空 poi_name の test_map_b sample が届くまで spin。
        def switched():
            return any(m == 'test_map_b' and p != ''
                       for m, p, _ in self.initialpose_samples)
        self.assertTrue(self._spin_until(switched), 'test_map_b の initialpose が届かない')
        self.assertTrue(
            self._spin_until(
                lambda: 'map_switch_succeeded:test_map_b' in self.nav_statuses),
            'map_switch_succeeded が publish されない')

        # LoadMap は fixture の map_file URL で 1 回受理されている。
        load_reqs = self.map_server.requests()
        self.assertEqual(len(load_reqs), 1,
                         f'LoadMap 呼び出し回数が 1 でない: {load_reqs}')
        self.assertTrue(load_reqs[0][0].endswith('test_map_b/map_b.yaml'),
                        f'LoadMap の map_url が想定外: {load_reqs[0][0]}')

        # 順序契約 (1): select_map 由来の clear ({test_map_b, ""}) が先頭 POI publish より先。
        seq_b = [(m, p) for m, p, _ in self.initialpose_samples if m == 'test_map_b']
        self.assertEqual(
            seq_b, [('test_map_b', ''), ('test_map_b', 'map_b_start')],
            'clear → 先頭 POI の順序契約が崩れている: ' + repr(seq_b))

        # 順序契約 (2): LoadMap 応答完了時刻 < 先頭 POI 受信時刻 (「POI 要求は LoadMap 成功後
        # のみ」#149/#184 不変条件)。受信時刻でなく応答完了時刻と比較することで、応答 pending
        # 中に非同期で先行 publish する regression も検出する (0.2s 遅延注入とセット)。
        # 全て同一ホストの monotonic clock なので直接比較できる。
        t_load_done = load_reqs[0][2]
        t_poi = next(t for m, p, t in self.initialpose_samples
                     if m == 'test_map_b' and p != '')
        self.assertLess(
            t_load_done, t_poi,
            'initialpose publish が LoadMap 応答完了より先に観測された (#149/#184 違反)')

    def test_b_switch_loadmap_failure_publishes_no_initialpose(self):
        """失敗経路: LoadMap 失敗時は map_switch_failed になり、非空 POI は publish されない。"""
        # join 時の latched 値 (単一 writer + depth=1 なのでちょうど 1 sample) を消費し、
        # 以降は index 基準で「切替開始後に publish された sample」だけを assert する。
        # これにより直前の latched 状態 (test_a の成否・単体実行) に依存しない。
        self.assertTrue(
            self._spin_until(lambda: len(self.initialpose_samples) >= 1),
            'latched initialpose が受信できない')
        n0 = len(self.initialpose_samples)

        self.map_server.set_fail(True)
        try:
            self._switch_map('test_map_a')
            self.assertTrue(
                self._spin_until(
                    lambda: 'map_switch_failed:test_map_a' in self.nav_statuses),
                'map_switch_failed が publish されない')
            # 失敗確定後の dwell で「遅れて POI が publish される」経路が無いことも確認する。
            self._spin_until(lambda: False, timeout=2.0)
            after_switch = [(m, p) for m, p, _ in self.initialpose_samples[n0:]]
            self.assertEqual(
                after_switch, [('test_map_a', '')],
                'LoadMap 失敗時は select_map の clear のみが publish されるはず: '
                + repr(after_switch))
        finally:
            self.map_server.set_fail(False)
