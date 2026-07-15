#!/usr/bin/env python3
"""mapoi_webui_node: ROS2 node with embedded Flask server for mapoi Web UI."""

import math
import os
import json
import logging
import queue
import threading
import time

import rclpy
from rclpy.duration import Duration
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, LivelinessPolicy, QoSProfile
try:  # Jazzy (rclpy 3.x+) and newer
    from rclpy.event_handler import SubscriptionEventCallbacks
except ImportError:  # Humble fallback
    from rclpy.qos_event import SubscriptionEventCallbacks
from std_msgs.msg import String
from std_srvs.srv import Trigger
from mapoi_interfaces.srv import (
    GetTagDefinitions, RequestInitialPose, SelectMap)
from mapoi_interfaces.msg import (
    LocalizationBackendStatus, NavigationBackendStatus)
import tf2_ros

from flask import Flask, jsonify, request, send_from_directory, Response

from mapoi_webui.yaml_handler import (
    compute_config_version, get_pois, get_routes,
    load_config, save_custom_tags, save_pois, save_routes,
)
from mapoi_webui.map_image import get_map_metadata, get_map_png


def _version_conflict_response(data, config_path):
    """楽観的競合検出 (#241、routes / custom_tags への展開は #343)。

    frontend が GET 時に受け取った version を `expected_version` として送り返す。
    current version と不一致なら (jsonify, 409) タプルを返し、呼び出し側はそれを
    そのまま return する。一致、または `expected_version` 不在 (旧クライアント /
    curl 後方互換で check skip) なら None。pois / routes / custom_tags の 3 つの
    save endpoint は同一 yaml へ書き込むため、この契約を共有する。
    空文字列など存在するが不一致な値は 409 になる (None のみが skip)。
    """
    expected_version = data.get('expected_version')
    if expected_version is None:
        return None
    current_version = compute_config_version(config_path)
    if current_version is not None and current_version != expected_version:
        return jsonify({
            'error': 'yaml file has been modified externally; '
                     'please reload to fetch the latest state before saving',
            'code': 'version_mismatch',
            'current_version': current_version,
        }), 409
    return None


def _validate_unique_names(items, label):
    """POI / route の name list が空 / 重複を含まないか検査する (#109)。

    frontend (poi-editor / route-editor) は formOk で同じ check を持つが、
    yaml 直編集や別 client からの POST に対する保険として backend でも reject。
    return: エラーメッセージ文字列 (issue あり) または None (OK)。case-sensitive 判定。
    """
    # Top-level 型 check: list でないと {"pois": null} や数値が validate を
    # 素通りして save_* で 500 になる (Codex PR #120 round 1 medium)。
    if not isinstance(items, list):
        return f'{label} list must be an array'
    seen = set()
    for i, it in enumerate(items):
        name = (it.get('name') if isinstance(it, dict) else None)
        if not name or not isinstance(name, str) or not name.strip():
            return f'{label} #{i} has empty name'
        name = name.strip()
        if name in seen:
            return f'{label} name "{name}" is duplicated'
        seen.add(name)
    return None


# tolerance 最小値 (msg spec #138): xy = 1 mm、yaw = 約 0.057°。
# 0 / 負値は「無反応 POI」を許してしまうため明示的に禁止。
_TOLERANCE_MIN = 0.001


def _validate_pois_tag_exclusivity(pois):
    """POI list の tag 排他組合せを backend でも reject する (#85, #143)。

    frontend (poi-editor.formOk → MapoiPoiFilter.validatePoiTags) で reject 済みだが、
    yaml 直編集や別 client からの POST に対する保険として backend でも検査。
    return: エラーメッセージ文字列 (issue あり) または None (OK)。
    """
    for i, poi in enumerate(pois):
        tags = []
        if isinstance(poi, dict) and isinstance(poi.get('tags'), list):
            tags = [str(t).lower() for t in poi['tags']]
        has_waypoint = 'waypoint' in tags
        has_landmark = 'landmark' in tags
        has_pause = 'pause' in tags
        name = (poi.get('name', '?') if isinstance(poi, dict) else '?')
        if has_waypoint and has_landmark:
            return (f'POI #{i} ({name}): "waypoint" and "landmark" cannot be used together. '
                    'A landmark is only a reference POI and is not sent to Nav2.')
        if has_pause and has_landmark:
            return (f'POI #{i} ({name}): "pause" and "landmark" cannot be used together. '
                    'A landmark is not a navigation target, so the robot cannot pause there.')
        # (initial_pose × landmark 排他は #144 で initial_pose system tag を廃止したため不要に。)
    return None


def _validate_pois_tolerance(pois):
    """POI list の tolerance.{xy,yaw} が finite な number で >= 0.001 を満たすかを検査する (#138)。

    frontend (poi-editor の HTML min / parseTolerance) で reject 済みだが、
    yaml 直編集や別 client からの POST に対する保険として backend でも reject。
    bool は int subclass のため明示除外、NaN / +-Inf は math.isfinite で reject
    (Codex review #139 medium 対応)。
    return: エラーメッセージ文字列 (issue あり) または None (OK)。
    """
    for i, poi in enumerate(pois):
        if not isinstance(poi, dict):
            return f'POI #{i} must be an object'
        tol = poi.get('tolerance')
        if not isinstance(tol, dict):
            return f'POI #{i} ({poi.get("name", "?")}): "tolerance" must be a mapping {{xy, yaw}}'
        for key in ('xy', 'yaw'):
            v = tol.get(key)
            if (isinstance(v, bool)
                    or not isinstance(v, (int, float))
                    or not math.isfinite(v)
                    or v < _TOLERANCE_MIN):
                return (f'POI #{i} ({poi.get("name", "?")}): '
                        f'"tolerance.{key}" must be a finite number >= {_TOLERANCE_MIN}')
    return None


class MapoiWebNode(Node):
    def __init__(self):
        super().__init__('mapoi_webui_node')

        # Parameters (same as mapoi_server)
        self.declare_parameter('maps_path', '')
        self.declare_parameter('map_name', 'turtlebot3_world')
        self.declare_parameter('config_file', 'mapoi_config.yaml')
        self.declare_parameter('web_port', 8765)
        self.declare_parameter('web_host', '0.0.0.0')
        self.declare_parameter('map_frame', 'map')
        self.declare_parameter('base_frame', 'base_link')
        # ロボットの実寸 (m)。frontend が connector 到達閾値 / robot marker サイズ
        # に使う (#116, #117)。Nav2 の `robot_radius` と意味は同じだが、本 node
        # は Nav2 非依存運用 (Editor mode 等) も想定するため、launch param で
        # 個別に渡す。Nav2 と必ずセットで使う場合は controller_server 側を
        # 直接 pull する案 (#117 案 B) を別途検討。
        self.declare_parameter('robot_radius', 0.15)

        self.maps_path_ = self.get_parameter('maps_path').get_parameter_value().string_value
        self.map_name_ = self.get_parameter('map_name').get_parameter_value().string_value
        self.config_file_ = self.get_parameter('config_file').get_parameter_value().string_value
        self.web_port_ = self.get_parameter('web_port').get_parameter_value().integer_value
        self.web_host_ = self.get_parameter('web_host').get_parameter_value().string_value
        self.map_frame_ = self.get_parameter('map_frame').get_parameter_value().string_value
        self.base_frame_ = self.get_parameter('base_frame').get_parameter_value().string_value
        # 値検証: 設定ミス (typo / yaml で `0` / 負値) を silent に飲み込まないよう
        # finite かつ正値を要求する。invalid なら warn して default 0.15 に fallback
        # (frontend 側 `setRobotRadius` ガードと defense in depth、
        #  Codex PR #126 round 1 low)。
        raw_radius = float(
            self.get_parameter('robot_radius').get_parameter_value().double_value)
        if not math.isfinite(raw_radius) or raw_radius <= 0.0:
            self.get_logger().warn(
                f'robot_radius={raw_radius!r} is invalid. Using the default 0.15 m. '
                'Please check the launch parameter so it matches the Nav2 robot_radius.')
            self.robot_radius_ = 0.15
        else:
            self.robot_radius_ = raw_radius

        # ROS2 service clients
        self.reload_client_ = self.create_client(Trigger, 'mapoi/reload_map_info')
        self.tag_defs_client_ = self.create_client(GetTagDefinitions, 'mapoi/get_tag_definitions')
        self.select_map_client_ = self.create_client(SelectMap, 'mapoi/select_map')
        # #211: initialpose POI は直接 publish せず mapoi_server (唯一の writer) へ
        # request_initial_pose service 経由で依頼する。
        self.request_initial_pose_client_ = self.create_client(
            RequestInitialPose, 'mapoi/request_initial_pose')

        # Navigation publishers
        self.goal_poi_pub_ = self.create_publisher(String, 'mapoi/nav/goal_pose_poi', 10)
        self.route_pub_ = self.create_publisher(String, 'mapoi/nav/route', 10)
        self.cancel_pub_ = self.create_publisher(String, 'mapoi/nav/cancel', 10)
        self.pause_pub_  = self.create_publisher(String, 'mapoi/nav/pause',  10)
        self.resume_pub_ = self.create_publisher(String, 'mapoi/nav/resume', 10)
        self.switch_map_pub_ = self.create_publisher(String, 'mapoi/nav/switch_map', 1)

        # TF for robot pose
        self.tf_buffer_ = tf2_ros.Buffer()
        self.tf_listener_ = tf2_ros.TransformListener(self.tf_buffer_, self)
        self.robot_pose_ = None
        self.create_timer(0.2, self.update_robot_pose)

        # Navigation status
        # QoS は mapoi_nav2_bridge と同じ transient_local。後起動 webui でも latched 値を受信できる。
        self.nav_status_ = 'idle'
        self.nav_status_target_ = ''
        nav_status_qos = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.nav_status_sub_ = self.create_subscription(
            String, 'mapoi/nav/status', self.nav_status_callback, nav_status_qos)

        # Command-rejected イベント通知 (#354): `mapoi/nav/status` は latched な状態
        # snapshot のため、走行中 (nav_mode_ != IDLE) の reject は publish されない (#339)。
        # このため走行中の typo goal 等は操作者に伝わらなかった。`mapoi/nav/command_rejected`
        # は状態と独立したイベント軸で、bridge 側は nav_mode_ に関わらず reject の都度必ず
        # publish する。QoS は volatile (非 latched、depth 10 = bridge 側 publisher と同じ)。
        # SSE 経由で frontend に toast として流すだけなので、後起動 subscriber へのリプレイは
        # 不要 — transient_local にしない。
        self.command_rejected_sub_ = self.create_subscription(
            String, 'mapoi/nav/command_rejected', self.command_rejected_callback, 10)

        # Navigation / Localization backend readiness (#198 / #209) の QoS は msg contract
        # (#208) に従う: transient_local + liveliness (publisher=MANUAL_BY_TOPIC,
        # subscriber=AUTOMATIC) + lease 5s (両側必須)。subscriber 側 policy を AUTOMATIC に
        # するのは pub=MANUAL_BY_TOPIC × sub=AUTOMATIC が compatible 表上 OK だから。lease は
        # `pub.lease <= sub.lease` 制約があり、両側 5s で揃えるのが運用上シンプル。
        # 結果として **msg contract に従わない publisher (例: liveliness QoS 未設定 = lease
        # infinite) は意図的に QoS incompatible で接続を拒否する**。custom bridge 実装者は
        # msg README の contract 表に従う必要がある (test_backend_status_liveliness.py で
        # incompatibility を pin)。
        backend_status_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            liveliness=LivelinessPolicy.AUTOMATIC,
            liveliness_lease_duration=Duration(seconds=5),
        )

        # Navigation backend readiness (#198): mapoi_nav2_bridge が 1Hz で publish する
        # readiness summary。WebUI / panel はこの値で navigation 操作 UI を gate する。
        # backend_status topic 不在の場合 (旧 mapoi_nav_server build (#208 以前 backend_status contract 未実装、#204 で nav2_bridge へ rename) 等) は None のまま、command topic
        # subscriber 数による旧判定にフォールバックする (`get_navigation_capabilities`)。
        self.backend_status_ = None
        # `nav_backend_alive_` は liveliness event_callback が更新する publisher 生存 flag。
        # 初期 False (subscription 作成直後 = publisher 未発見) で、discovery で True、
        # 1Hz publish が 5s lease 以上途切れると False に戻る。`backend_status_` が None でない
        # (= 一度受信した) 状態で False に戻った場合のみ staleness として UI を disable。
        self.nav_backend_alive_ = False
        self.backend_status_sub_ = self.create_subscription(
            NavigationBackendStatus, 'mapoi/nav/backend_status',
            self.backend_status_callback, backend_status_qos,
            event_callbacks=SubscriptionEventCallbacks(
                liveliness=self._nav_backend_liveliness_callback))

        # Localization backend readiness (#209): mapoi_amcl_localization_bridge (or any custom
        # localization bridge) が 1Hz で publish する readiness summary。WebUI は Set Initial
        # Pose 操作 UI と initial pose POI 選択をこの値で gate する。topic 不在 (bridge 未起動 /
        # editor 専用構成) は None のまま、frontend 側で「不明」状態として扱う。
        self.localization_status_ = None
        self.localization_backend_alive_ = False
        self.localization_status_sub_ = self.create_subscription(
            LocalizationBackendStatus, 'mapoi/localization/backend_status',
            self.localization_status_callback, backend_status_qos,
            event_callbacks=SubscriptionEventCallbacks(
                liveliness=self._localization_backend_liveliness_callback))

        # Subscribe to mapoi/config_path for external map switches.
        # transient_local QoS で publisher (mapoi_server) の latched 値を受信する (#135)。
        config_path_qos = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
        self.config_path_sub_ = self.create_subscription(
            String, 'mapoi/config_path', self.config_path_callback, config_path_qos)

        # SSE clients (#135 (B)): rviz / 外部 save 由来の config 変更を frontend に push するための
        # client queue 集合。/api/events で接続された client ごとに queue を 1 つ持ち、
        # config_path_callback で全 client queue に broadcast する。
        self._sse_clients = set()
        self._sse_lock = threading.Lock()

        # maps_path 未設定時は fail-fast せず WARN のみで起動継続する (#343)。map 一覧は
        # maps_path 配下を listdir で直読みするため (`get_maps_list`)、この場合は空リストに
        # なり editor 系 (地図一覧・POI/route 編集) は使えないが、nav 操作系 API は引き続き動く
        # (詳細は mapoi_webui/README.md の「maps_path 未設定時の縮退」を参照)。
        if not self.maps_path_:
            self.get_logger().warn('maps_path parameter not set. Set it to use the web editor.')

        self.get_logger().info(
            f'mapoi_webui_node started: maps_path={self.maps_path_}, '
            f'map_name={self.map_name_}, port={self.web_port_}')

        # Start Flask in daemon thread
        self.flask_app_ = self.create_flask_app()
        flask_thread = threading.Thread(
            target=self.run_flask, daemon=True)
        flask_thread.start()

    def nav_status_callback(self, msg):
        """Update navigation status from mapoi/nav/status topic."""
        # Expected format: "status" or "status:target"
        parts = msg.data.split(':', 1)
        self.nav_status_ = parts[0]
        # Only overwrite target if explicitly provided; keep previous target
        # so that succeeded/aborted/canceled still show the target name.
        if len(parts) > 1:
            self.nav_status_target_ = parts[1]

    def command_rejected_callback(self, msg):
        """Forward a rejected command as an SSE `command_rejected` event (#354).

        `mapoi/nav/status` を汚さない (走行中は書き換えない、#339) ためのイベント通知。
        frontend はこれを受けて一時的な toast を表示する (latched snapshot とは独立)。
        """
        self._broadcast_sse_event('command_rejected', {'target': msg.data})

    def backend_status_callback(self, msg):
        """Cache the latest navigation backend readiness payload (#198, #205)."""
        # Minimal contract (#205 review): backend_type / backend_ready / reason のみ。
        # bridge 実装者の負担を減らすため per-capability フィールドは持たない。
        # localization readiness は別 topic (#209) で扱う。
        self.backend_status_ = {
            'backend_type': msg.backend_type,
            'backend_ready': bool(msg.backend_ready),
            'reason': msg.reason,
        }

    def localization_status_callback(self, msg):
        """Cache the latest localization backend readiness payload (#209)."""
        # Minimal contract: backend_type / backend_ready / reason のみ。Navigation 側 (#205) と同じ
        # 形式で別 topic に分かれているのは、navigation と localization が独立した契約 / 軸として
        # 扱われるため (将来 AMCL 以外の localization に切替えやすくする目的、issue #209)。
        self.localization_status_ = {
            'backend_type': msg.backend_type,
            'backend_ready': bool(msg.backend_ready),
            'reason': msg.reason,
        }

    def _nav_backend_liveliness_callback(self, event):
        """Track navigation backend publisher liveness (#208).

        MANUAL_BY_TOPIC publisher + lease 経過で `alive_count` が 0 へ遷移する。
        `_resolve_backend_status_for_ui` がこの flag を見て、受信済み cache を
        explicit unready (`backend_ready=false`) にデコレートする。
        """
        self.nav_backend_alive_ = event.alive_count > 0

    def _localization_backend_liveliness_callback(self, event):
        """Track localization backend publisher liveness (#208). Same semantics as nav."""
        self.localization_backend_alive_ = event.alive_count > 0

    @staticmethod
    def _resolve_backend_status_for_ui(cached, alive):
        """Combine cached backend_status payload with liveliness flag for UI gating (#208).

        三状態を区別する:

        - cached is None (未受信)              → return None
            * `get_navigation_capabilities` 側で legacy fallback path に流れる
              (旧 publisher / bridge 不在の後方互換)
        - cached is not None and alive=True   → return cached (latched payload そのまま)
        - cached is not None and alive=False  → return {**cached, backend_ready: False}
            * publisher が一度 alive 後に lost した状態 (lease 経過 / process kill)。
              legacy fallback ではなく explicit unready として返し、UI を確実に disable
              させる (#212 codex review high)。
        """
        if cached is None:
            return None
        if alive:
            return cached
        return {
            **cached,
            'backend_ready': False,
            'reason': 'liveliness lost (publisher not active)',
        }

    def update_robot_pose(self):
        """Lookup TF map->base_link and cache robot pose."""
        try:
            t = self.tf_buffer_.lookup_transform(
                self.map_frame_, self.base_frame_, rclpy.time.Time())
            q = t.transform.rotation
            yaw = math.atan2(
                2.0 * (q.w * q.z + q.x * q.y),
                1.0 - 2.0 * (q.y * q.y + q.z * q.z))
            self.robot_pose_ = {
                'x': t.transform.translation.x,
                'y': t.transform.translation.y,
                'yaw': yaw,
            }
        except Exception:
            self.robot_pose_ = None

    def config_path_callback(self, msg):
        """Detect external map switches via mapoi/config_path topic + frontend に push (#135 (B))."""
        # Extract map_name from path: .../maps/<map_name>/mapoi_config.yaml
        path = msg.data
        try:
            parts = path.replace('\\', '/').split('/')
            # Find config_file in path, map_name is the directory before it
            map_name_parsed = None
            for i, part in enumerate(parts):
                if part == self.config_file_ and i > 0:
                    map_name_parsed = parts[i - 1]
                    if map_name_parsed != self.map_name_:
                        self.map_name_ = map_name_parsed
                        self.get_logger().info(f'Map switched externally to: {map_name_parsed}')
                    break
            if map_name_parsed is not None:
                # 内容変更 (POI / route の save 後 reload による再 publish) でも frontend に通知して
                # loadPois / loadRoutes / loadTagDefinitions を再実行させる (#135 (B))。
                # map 名が正しく解釈できた時のみ broadcast: 期待形式に部分一致するが map 特定でき
                # ないケースで無駄な reload を避ける (#173 Round 1 / 2 medium)。
                payload = {'map_name': map_name_parsed}
                # yaml 全体の内容 hash (#241 の config_version) を同梱する (#384)。保存した
                # タブは save 応答で同じ値を既に持っており、frontend はこれと照合して自タブ発
                # の変更なら reload (undo 履歴・map 視点の破棄) を skip できる。計算できない
                # 場合 (config 不在等) は省略し、frontend は従来どおり reload する (安全側)。
                try:
                    version = compute_config_version(path)
                except OSError:
                    version = None
                if version:
                    payload['config_version'] = version
                self._broadcast_sse_event('config_changed', payload)
        except Exception as e:
            self.get_logger().warn(f'Failed to parse config path: {e}')

    def _broadcast_sse_event(self, event_type, payload=None):
        """SSE で接続中の全 frontend client にイベントを broadcast する (#135 (B))。

        各 client の queue.Queue に put_nowait で event を流す。client 切断は
        /api/events generator の finally で自動 discard されるので queue 漏れなし。
        queue は bounded (maxsize=10、#173 の DoS 対策) なので、drain が滞る遅い
        client では Full で event が黙って drop される (通知系イベントの用途では許容)。
        """
        data = {'type': event_type}
        if payload is not None:
            data['payload'] = payload
        with self._sse_lock:
            for q in self._sse_clients:
                try:
                    q.put_nowait(data)
                except queue.Full:
                    pass  # 遅い client への event は drop する (maxsize=10, #173)

    def get_config_path(self, map_name=None):
        """Get the full path to mapoi_config.yaml for a given map."""
        name = map_name or self.map_name_
        return os.path.join(self.maps_path_, name, self.config_file_)

    def get_map_dir(self, map_name=None):
        """Get the map directory path."""
        name = map_name or self.map_name_
        return os.path.join(self.maps_path_, name)

    def call_reload_map_info(self):
        """Call reload_map_info service and await response.success.

        Returns True only when server reports success; False on
        unavailable / timeout / server-side failure.
        """
        response = self._call_service_sync(
            self.reload_client_, Trigger.Request(), 'mapoi/reload_map_info', timeout_sec=3.0)
        if response is None:
            return False  # service unavailable / timeout (logged in helper)
        if not response.success:
            self.get_logger().warn(f'mapoi/reload_map_info returned failure: {response.message}')
            return False
        self.get_logger().info('mapoi/reload_map_info succeeded')
        return True

    def publish_with_subscriber_check(self, pub, msg, topic_name):
        """Publish with best-effort subscriber-count check.

        ROS 2 `publisher.publish()` は subscriber がいなくても成功扱いになる。
        UI ボタン経由の publish では subscriber 不在 (相手 node 未起動等) を
        silent failure にせず warning として user に返したい場面がある。

        **best-effort**: `get_subscription_count()` と `publish()` の間で
        subscriber 状態が変わる race を排除できない (check 通過後に消える /
        check 失敗後に直前に現れる)。「明らかな未起動」検出が目的で、厳密な
        到達確認が必要なら service / action / ack 設計に切り替える。

        Args:
            pub: rclpy publisher
            msg: message to publish
            topic_name: topic name for warning message

        Returns:
            (published, warning) where warning is None or human-readable string.
            published is always True since publish itself doesn't fail locally.
        """
        sub_count = pub.get_subscription_count()
        pub.publish(msg)
        if sub_count == 0:
            warning = (f'No subscribers found for {topic_name}. '
                       'The listener, such as mapoi_nav2_bridge, may not be running.')
            self.get_logger().warn(warning)
            return True, warning
        return True, None

    def get_navigation_capabilities(self):
        """Return navigation backend availability for the WebUI / API consumers.

        Priority (#198):
        1. mapoi/nav/backend_status topic 受信済 → backend_ready をそのまま使う。
           Nav2 action / service の存在を含めた厳密 readiness。
        2. backend_status 未受信 (旧 mapoi_nav_server build (#208 以前 backend_status contract 未実装、#204 で nav2_bridge へ rename) や bridge 未実装) → command topic
           subscriber 数による旧判定にフォールバック (= bridge 起動だけは検知できる)。

        旧フィールド名 (navigation_available 等) は frontend 既存利用のため維持する。
        Minimal contract に合わせて返す情報も backend_ready / topics 程度に絞る (#205 review)。
        フォールバック path で `navigation_available` と操作ボタン enable の条件が完全一致しないのは
        意図的な後方互換 — 新 mapoi_nav2_bridge に切り替われば backend_status path で整合する。
        """
        publishers = {
            'switch_map': (self.switch_map_pub_, 'mapoi/nav/switch_map'),
            'goal': (self.goal_poi_pub_, 'mapoi/nav/goal_pose_poi'),
            'route': (self.route_pub_, 'mapoi/nav/route'),
            'cancel': (self.cancel_pub_, 'mapoi/nav/cancel'),
            'pause': (self.pause_pub_, 'mapoi/nav/pause'),
            'resume': (self.resume_pub_, 'mapoi/nav/resume'),
        }
        topics = {}
        for key, (pub, topic_name) in publishers.items():
            count = pub.get_subscription_count()
            topics[key] = {
                'topic': topic_name,
                'subscribers': count,
                'available': count > 0,
            }
        # #211: initialpose_poi の publish は mapoi_server に集約したため WebUI は publisher を
        # 持たない。subscriber 数 (localization bridge / sim が listening しているか) は node 経由で
        # 数える。なお下の通り initialpose は navigation backend 可用性の根拠には使わない。
        initialpose_subs = self.count_subscribers('mapoi/initialpose_poi')
        topics['initialpose'] = {
            'topic': 'mapoi/initialpose_poi',
            'subscribers': initialpose_subs,
            'available': initialpose_subs > 0,
        }
        # `mapoi/initialpose_poi` may still have simulator / localization bridge
        # subscribers after mapoi_nav2_bridge stops. Treat only navigation command
        # topics as evidence that a navigation backend is available.
        command_keys = ('goal', 'route', 'cancel', 'pause', 'resume')
        command_available = any(topics[key]['available'] for key in command_keys)
        switch_map_available = topics['switch_map']['available']
        legacy_available = switch_map_available or command_available

        # bridge プロセスが死亡しても transient_local の cache (self.backend_status_ /
        # self.localization_status_) は subscriber callback が呼ばれない限り古い値を持ち続ける。
        # liveliness QoS (#208) で取得した `*_backend_alive_` flag を `_resolve_backend_status_for_ui`
        # で反映する。
        # - 未受信: backend = None → legacy fallback (旧 mapoi_nav_server build (#208 以前 contract 未実装、#204 で rename) / bridge 不在の後方互換)
        # - 受信済み + alive: cache をそのまま使う
        # - 受信済み + lost: backend_ready=false に上書きした dict を返す → UI 確実 disable
        #   (#212 codex review high: legacy fallback path に落とすと command subscriber が残った
        #    状態で false-enable になる回帰)
        backend = self._resolve_backend_status_for_ui(
            self.backend_status_, self.nav_backend_alive_)
        localization = self._resolve_backend_status_for_ui(
            self.localization_status_, self.localization_backend_alive_)

        if backend is not None:
            navigation_available = bool(backend['backend_ready'])
            # backend_status path では map switch も backend_ready の AND に含まれているため
            # alias として同じ値を返す (#205 round 3 review high #2 後方互換)。
            switch_map_available_final = navigation_available
        else:
            navigation_available = legacy_available
            switch_map_available_final = switch_map_available
        return {
            'navigation_available': navigation_available,
            # `switch_map_available` は backend_status 不在時のフォールバック表示用に維持。
            # 旧 API 互換のため key を消さない (#205 round 3 review high #2)。
            'switch_map_available': switch_map_available_final,
            'command_available': command_available,
            'topics': topics,
            'backend_status': backend,
            # Localization backend readiness (#209) は navigation とは独立した仕様として frontend に
            # 渡す。topic 不在 (bridge 未起動) / publisher 死亡時は None で、frontend は
            # 「Localization status unknown」を表示し、Set Initial Pose UI は安全側に disable する。
            'localization_status': localization,
        }

    def _call_service_sync(self, client, request, service_name, timeout_sec=3.0,
                           wait_for_service_sec=2.0):
        """Call ROS 2 service from Flask thread and wait for the response.

        SingleThreadedExecutor で main thread の rclpy.spin が future を resolve するため、
        Flask thread はここで future.done() を polling する。spin_until_future_complete を
        Flask thread から呼ぶと main thread の spin と executor を競合させるので避ける。

        実効最長待ち時間 ≒ wait_for_service_sec + timeout_sec (default 5s)。
        timeout 時は future.cancel() で pending request を解放する (helper の
        繰り返し呼び出しで cancel 忘れ future が蓄積しないように)。

        Args:
            client: rclpy service client
            request: service request message
            service_name: service name for logging
            timeout_sec: 全体の wait timeout (default 3.0s)
            wait_for_service_sec: service discovery の wait timeout (default 2.0s)

        Returns:
            response object on success, None on timeout / unavailability / exception.
        """
        if not client.wait_for_service(timeout_sec=wait_for_service_sec):
            self.get_logger().warn(f'{service_name} service not available')
            return None
        future = client.call_async(request)
        deadline = time.monotonic() + timeout_sec
        while not future.done():
            if time.monotonic() > deadline:
                future.cancel()
                self.get_logger().warn(f'{service_name} call timed out after {timeout_sec}s')
                return None
            time.sleep(0.05)
        try:
            return future.result()
        except Exception as e:
            self.get_logger().error(f'{service_name} call exception: {e}')
            return None

    def get_maps_list(self):
        """Get list of available maps from maps_path directory."""
        maps = []
        if self.maps_path_ and os.path.isdir(self.maps_path_):
            for entry in sorted(os.listdir(self.maps_path_)):
                full = os.path.join(self.maps_path_, entry)
                if os.path.isdir(full):
                    maps.append(entry)
        return maps

    def run_flask(self):
        """Run Flask server (called in daemon thread)."""
        self.get_logger().info(f'Flask server starting on {self.web_host_}:{self.web_port_}')
        self.flask_app_.run(
            host=self.web_host_,
            port=self.web_port_,
            debug=False,
            use_reloader=False,
            threaded=True)  # SSE long-lived connection と他 request の並行のため明示 (#135 (B))

    def create_flask_app(self):
        """Create and configure the Flask application."""
        app = Flask(__name__)
        logging.getLogger('werkzeug').setLevel(logging.WARNING)
        node = self  # capture for closures

        # Static files directory
        web_dir = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'web')
        # For installed package, try share directory
        try:
            from ament_index_python.packages import get_package_share_directory
            share_web_dir = os.path.join(
                get_package_share_directory('mapoi_webui'), 'web')
            if os.path.isdir(share_web_dir):
                web_dir = share_web_dir
        except Exception:
            pass

        @app.route('/')
        def index():
            return send_from_directory(web_dir, 'index.html')

        @app.route('/css/<path:filename>')
        def css_static(filename):
            return send_from_directory(os.path.join(web_dir, 'css'), filename)

        @app.route('/js/<path:filename>')
        def js_static(filename):
            return send_from_directory(os.path.join(web_dir, 'js'), filename)

        # #394: 同梱 Leaflet (web/vendor/leaflet/) の配信。静的配信はディレクトリ単位の
        # ホワイトリスト方式のため、vendor/ を追加しないと index.html の参照が 404 になる。
        @app.route('/vendor/<path:filename>')
        def vendor_static(filename):
            return send_from_directory(os.path.join(web_dir, 'vendor'), filename)

        @app.route('/api/maps')
        def api_maps():
            maps = node.get_maps_list()
            return jsonify({
                'maps': maps,
                'current_map': node.map_name_,
            })

        @app.route('/api/maps/<name>/image')
        def api_map_image(name):
            map_dir = node.get_map_dir(name)
            png_bytes, content_type = get_map_png(map_dir)
            if png_bytes is None:
                return jsonify({'error': 'Map image not found', 'code': 'not_found'}), 404
            return Response(png_bytes, mimetype=content_type)

        @app.route('/api/maps/<name>/metadata')
        def api_map_metadata(name):
            map_dir = node.get_map_dir(name)
            meta = get_map_metadata(map_dir)
            if meta is None:
                return jsonify({'error': 'Map metadata not found', 'code': 'not_found'}), 404
            return jsonify({
                'resolution': meta['resolution'],
                'origin': meta['origin'],
                'width': meta['width'],
                'height': meta['height'],
            })

        @app.route('/api/editor/select-map', methods=['POST'])
        def api_select_map():
            data = request.get_json()
            if not data or 'map_name' not in data:
                return jsonify({'error': 'map_name required', 'code': 'invalid_request'}), 400
            map_name = str(data['map_name']).strip()
            if not map_name:
                return jsonify({'error': 'map_name required', 'code': 'invalid_request'}), 400
            req = SelectMap.Request()
            req.map_name = map_name
            response = node._call_service_sync(
                node.select_map_client_, req, 'mapoi/select_map', timeout_sec=3.0)
            if response is None:
                return jsonify({
                    'error': 'mapoi/select_map service unavailable or timed out',
                    'code': 'service_unavailable',
                }), 503
            if not response.success:
                return jsonify({
                    'error': response.error_message or 'mapoi/select_map failed',
                    'code': 'invalid_request',
                }), 400
            node.map_name_ = map_name
            return jsonify({
                'success': True,
                'map_name': map_name,
                'config_path': response.config_path,
                'resolved_initial_poi_name': response.resolved_initial_poi_name,
            })

        @app.route('/api/nav/switch-map', methods=['POST'])
        def api_nav_switch_map():
            data = request.get_json()
            # map_name は文字列必須。非文字列 (null / 数値 / list 等) を str() で
            # coerce すると `null` が literal 'None' になり、maps_path 未設定時の
            # membership skip 経路でそのまま publish される潜在バグになる。別 client
            # からの POST 保険として、型不正は coerce せず 400 で弾く (#199 follow-up)。
            if not isinstance(data, dict) or not isinstance(data.get('map_name'), str):
                return jsonify({'error': 'map_name required', 'code': 'invalid_request'}), 400
            map_name = data['map_name'].strip()
            if not map_name:
                return jsonify({'error': 'map_name required', 'code': 'invalid_request'}), 400
            maps = node.get_maps_list()
            if maps and map_name not in maps:
                return jsonify({'error': f'unknown map: {map_name}', 'code': 'not_found'}), 404
            msg = String()
            msg.data = map_name
            _, warning = node.publish_with_subscriber_check(
                node.switch_map_pub_, msg, 'mapoi/nav/switch_map')
            node.get_logger().info(f'Navigation map switch requested: {map_name}')
            payload = {'success': True, 'map_name': map_name}
            if warning:
                payload['warning'] = warning
            return jsonify(payload)

        @app.route('/api/mode')
        def api_mode():
            return jsonify({
                'navigation': node.get_navigation_capabilities(),
            })

        @app.route('/api/pois')
        def api_get_pois():
            config_path = node.get_config_path()
            if not os.path.exists(config_path):
                return jsonify({'error': 'Config not found', 'code': 'not_found'}), 404
            pois = get_pois(config_path)
            return jsonify({
                'pois': pois,
                'map_name': node.map_name_,
                # 楽観的競合検出のため frontend に渡す yaml 内容ハッシュ (#241)。
                'config_version': compute_config_version(config_path),
            })

        @app.route('/api/pois', methods=['POST'])
        def api_save_pois():
            data = request.get_json()
            if data is None or 'pois' not in data:
                return jsonify({'error': 'Invalid request body', 'code': 'invalid_request'}), 400
            # name の uniqueness / 空 を backend でも validate (#109)。
            # frontend 経由なら poi-editor が reject 済みだが、yaml 直編集や
            # 別 client からの POST に対する保険として 400 で reject する。
            err = _validate_unique_names(data['pois'], 'POI')
            if err:
                return jsonify({'error': err, 'code': 'invalid_request'}), 400
            err = _validate_pois_tag_exclusivity(data['pois'])
            if err:
                return jsonify({'error': err, 'code': 'invalid_request'}), 400
            err = _validate_pois_tolerance(data['pois'])
            if err:
                return jsonify({'error': err, 'code': 'invalid_request'}), 400
            config_path = node.get_config_path()
            if not os.path.exists(config_path):
                return jsonify({'error': 'Config not found', 'code': 'not_found'}), 404
            conflict = _version_conflict_response(data, config_path)
            if conflict is not None:
                return conflict
            try:
                save_pois(config_path, data['pois'])
                new_version = compute_config_version(config_path)
                reloaded = node.call_reload_map_info()
                if not reloaded:
                    return jsonify({
                        'success': True,
                        'config_version': new_version,
                        'warning': 'The YAML file was saved, but mapoi_server mapoi/reload_map_info '
                                   'did not respond or failed. Check the logs for details.'
                    })
                return jsonify({'success': True, 'config_version': new_version})
            except Exception as e:
                node.get_logger().error(f'Failed to save POIs: {e}')
                return jsonify({'error': str(e), 'code': 'internal_error'}), 500

        @app.route('/api/tag-definitions')
        def api_tag_definitions():
            response = node._call_service_sync(
                node.tag_defs_client_, GetTagDefinitions.Request(), 'mapoi/get_tag_definitions')
            if response is None:
                return jsonify({
                    'error': 'mapoi/get_tag_definitions service unavailable or timed out',
                    'code': 'service_unavailable',
                }), 503
            tags = [
                {'name': d.name, 'description': d.description, 'is_system': d.is_system}
                for d in response.definitions
            ]
            # custom_tags は pois/routes と同じ yaml に書き込むため、POST /api/custom-tags の
            # 楽観的競合検出 (#241 の展開, #343) 用に config_version を同梱する。config 不在
            # (maps_path 未設定等) では compute_config_version が None を返し、POST 側は
            # None を expected_version 一致対象から外して check skip 相当に倒す。
            return jsonify({
                'tags': tags,
                'config_version': compute_config_version(node.get_config_path()),
            })

        @app.route('/api/custom-tags', methods=['POST'])
        def api_save_custom_tags():
            data = request.get_json()
            if data is None or 'custom_tags' not in data:
                return jsonify({'error': 'Invalid request body', 'code': 'invalid_request'}), 400
            config_path = node.get_config_path()
            if not os.path.exists(config_path):
                return jsonify({'error': 'Config not found', 'code': 'not_found'}), 404
            conflict = _version_conflict_response(data, config_path)
            if conflict is not None:
                return conflict
            try:
                save_custom_tags(config_path, data['custom_tags'])
                new_version = compute_config_version(config_path)
                reloaded = node.call_reload_map_info()
                if not reloaded:
                    return jsonify({
                        'success': True,
                        'config_version': new_version,
                        'warning': 'The YAML file was saved, but mapoi_server mapoi/reload_map_info '
                                   'did not respond or failed. Check the logs for details.'
                    })
                return jsonify({'success': True, 'config_version': new_version})
            except Exception as e:
                node.get_logger().error(f'Failed to save custom tags: {e}')
                return jsonify({'error': str(e), 'code': 'internal_error'}), 500

        @app.route('/api/routes')
        def api_get_routes():
            config_path = node.get_config_path()
            if not os.path.exists(config_path):
                return jsonify({'error': 'Config not found', 'code': 'not_found'}), 404
            routes = get_routes(config_path)
            return jsonify({
                'routes': routes,
                'map_name': node.map_name_,
                # 楽観的競合検出 (#241 の展開, #343) のため frontend に渡す yaml 内容ハッシュ。
                'config_version': compute_config_version(config_path),
            })

        @app.route('/api/routes', methods=['POST'])
        def api_save_routes():
            data = request.get_json()
            if data is None or 'routes' not in data:
                return jsonify({'error': 'Invalid request body', 'code': 'invalid_request'}), 400
            err = _validate_unique_names(data['routes'], 'Route')
            if err:
                return jsonify({'error': err, 'code': 'invalid_request'}), 400
            config_path = node.get_config_path()
            if not os.path.exists(config_path):
                return jsonify({'error': 'Config not found', 'code': 'not_found'}), 404
            conflict = _version_conflict_response(data, config_path)
            if conflict is not None:
                return conflict
            try:
                save_routes(config_path, data['routes'])
                new_version = compute_config_version(config_path)
                reloaded = node.call_reload_map_info()
                if not reloaded:
                    return jsonify({
                        'success': True,
                        'config_version': new_version,
                        'warning': 'The YAML file was saved, but mapoi_server mapoi/reload_map_info '
                                   'did not respond or failed. Check the logs for details.'
                    })
                return jsonify({'success': True, 'config_version': new_version})
            except Exception as e:
                node.get_logger().error(f'Failed to save routes: {e}')
                return jsonify({'error': str(e), 'code': 'internal_error'}), 500

        @app.route('/api/nav/goal', methods=['POST'])
        def api_nav_goal():
            data = request.get_json()
            if not data or 'poi_name' not in data:
                return jsonify({'error': 'poi_name required', 'code': 'invalid_request'}), 400
            msg = String()
            msg.data = data['poi_name']
            _, warning = node.publish_with_subscriber_check(
                node.goal_poi_pub_, msg, 'mapoi/nav/goal_pose_poi')
            node.nav_status_ = 'navigating'
            node.nav_status_target_ = data['poi_name']
            node.get_logger().info(f'Nav goal: {data["poi_name"]}')
            return jsonify({'success': True, 'warning': warning} if warning else {'success': True})

        @app.route('/api/nav/route', methods=['POST'])
        def api_nav_route():
            data = request.get_json()
            if not data or 'route_name' not in data:
                return jsonify({'error': 'route_name required', 'code': 'invalid_request'}), 400
            msg = String()
            msg.data = data['route_name']
            _, warning = node.publish_with_subscriber_check(
                node.route_pub_, msg, 'mapoi/nav/route')
            node.nav_status_ = 'navigating'
            node.nav_status_target_ = data['route_name']
            node.get_logger().info(f'Nav route: {data["route_name"]}')
            return jsonify({'success': True, 'warning': warning} if warning else {'success': True})

        @app.route('/api/nav/cancel', methods=['POST'])
        def api_nav_cancel():
            msg = String()
            msg.data = 'cancel'
            _, warning = node.publish_with_subscriber_check(
                node.cancel_pub_, msg, 'mapoi/nav/cancel')
            node.nav_status_ = 'canceled'
            node.get_logger().info('Nav canceled')
            return jsonify({'success': True, 'warning': warning} if warning else {'success': True})

        @app.route('/api/nav/pause', methods=['POST'])
        def api_nav_pause():
            msg = String()
            msg.data = 'webui'
            _, warning = node.publish_with_subscriber_check(
                node.pause_pub_, msg, 'mapoi/nav/pause')
            node.get_logger().info('Nav pause requested')
            return jsonify({'success': True, 'warning': warning} if warning else {'success': True})

        @app.route('/api/nav/resume', methods=['POST'])
        def api_nav_resume():
            msg = String()
            msg.data = 'webui'
            _, warning = node.publish_with_subscriber_check(
                node.resume_pub_, msg, 'mapoi/nav/resume')
            node.get_logger().info('Nav resume requested')
            return jsonify({'success': True, 'warning': warning} if warning else {'success': True})

        @app.route('/api/nav/status')
        def api_nav_status():
            return jsonify({
                'status': node.nav_status_,
                'target': node.nav_status_target_,
                'robot_pose': node.robot_pose_,
                # robot_radius は launch 起動時固定だが、frontend が boot 時に
                # 別 endpoint を叩かなくて済むよう poll 結果に相乗りさせる。
                # 値は float (m)、payload size 影響は無視できる (#117)。
                'robot_radius': node.robot_radius_,
                'navigation': node.get_navigation_capabilities(),
            })

        @app.route('/api/nav/initial-pose', methods=['POST'])
        def api_nav_initialpose():
            data = request.get_json()
            if not data or 'poi_name' not in data:
                return jsonify({'error': 'poi_name required', 'code': 'invalid_request'}), 400
            # #211: initialpose_poi を直接 publish せず、唯一の writer である mapoi_server に
            # request_initial_pose service 経由で publish を依頼する。
            req = RequestInitialPose.Request()
            req.map_name = node.map_name_
            req.poi_name = data['poi_name']
            response = node._call_service_sync(
                node.request_initial_pose_client_, req, 'mapoi/request_initial_pose', timeout_sec=3.0)
            if response is None:
                return jsonify({
                    'error': 'mapoi/request_initial_pose service unavailable or timed out',
                    'code': 'service_unavailable',
                }), 503
            if not response.success:
                return jsonify({
                    'error': response.error_message or 'mapoi/request_initial_pose failed',
                    'code': 'invalid_request',
                }), 400
            node.get_logger().info(f'Initial pose: {data["poi_name"]}')
            # #211 review fix: 旧 publish_with_subscriber_check 相当の「無人 publish」警告を復元。
            # service は mapoi_server に届くが、その先の mapoi/initialpose_poi に subscriber
            # (localization bridge) が居なければ initial pose はどこにも配信されない。operator が
            # localization 未接続のまま set した場合に検知できるよう warning を返す。
            if node.count_subscribers('mapoi/initialpose_poi') == 0:
                return jsonify({
                    'success': True,
                    'warning': 'No subscribers found for mapoi/initialpose_poi. '
                               'The localization bridge may not be running, so the initial pose was not sent.',
                })
            return jsonify({'success': True})

        @app.route('/api/events')
        def api_events():
            """SSE endpoint: rviz / 外部 save 由来の config 変更を frontend に push (#135 (B))。

            EventSource (frontend) で接続して、{"type": "config_changed"} 等のイベントを受信する。
            client 切断時は heartbeat (30s 周期) の write error で generator が例外を受け、
            finally で client queue を discard する (#173 Round 1 high)。
            """
            def gen():
                # bounded queue: 遅いクライアントで event が積み上がる場合の DoS 対策 (#173 Round 1 high)。
                # 通常は frontend が即座に drain するので 10 件以内に収まる、overflow は drop。
                q = queue.Queue(maxsize=10)
                with node._sse_lock:
                    node._sse_clients.add(q)
                try:
                    while True:
                        try:
                            # 30s timeout + heartbeat: client 切断検知 + プロキシバッファ flush。
                            # timeout 時は SSE comment (`:\n\n`) を yield することで socket write を発火し、
                            # client 切断時は ConnectionResetError → finally で discard される。
                            data = q.get(timeout=30)
                            yield f'data: {json.dumps(data)}\n\n'
                        except queue.Empty:
                            yield ': heartbeat\n\n'
                finally:
                    with node._sse_lock:
                        node._sse_clients.discard(q)
            # SSE response headers: プロキシ (nginx 等) のバッファリング無効化 + cache 無効化
            # (#173 Round 1 medium)。
            return Response(gen(), mimetype='text/event-stream',
                            headers={
                                'Cache-Control': 'no-cache',
                                'X-Accel-Buffering': 'no',
                            })

        return app


def main(args=None):
    rclpy.init(args=args)
    node = MapoiWebNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        # launch 経由 SIGINT では rclpy context が既に shutdown 済みの場合があり、
        # rclpy.shutdown() を直接呼ぶと RCLError になる。try_shutdown() は
        # 状態確認と shutdown を 1 つの API に閉じ込めてある。
        rclpy.try_shutdown()


if __name__ == '__main__':
    main()
