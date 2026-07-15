# mapoi_server

> English version (primary): [README.md](./README.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

mapoi のメインパッケージです。
地図と POI の情報を管理し、他のパッケージにサービスとして提供します。また、POI 名を指定した自律走行やPOI半径イベントの検知を行います。

## 自分のロボットへの組み込み方

4 つの core node (`mapoi_server` / `mapoi_nav2_bridge` / `mapoi_amcl_localization_bridge` / `mapoi_rviz2_publisher`) と、必要に応じてシミュレータ連動 bridge をまとめて起動する bringup launch を提供しています。自前の launch から以下のように include してください:

```yaml
- include:
    file: "$(find-pkg-share mapoi_server)/launch/mapoi_bringup.launch.yaml"
    arg:
      - {name: maps_path, value: "/path/to/your/maps"}    # REQUIRED
      - {name: map_name, value: "initial_map_name"}        # REQUIRED
      - {name: config_file, value: "mapoi_config.yaml"}    # optional
      - {name: state_path, value: "/var/lib/mapoi"}        # optional (実機運用では推奨、#297)
      - {name: simulator, value: "none"}                   # gazebo|gz|none (default: none)
      - {name: robot_entity_name, value: "burger"}         # simulator=gazebo|gz のとき必要
      - {name: robot_sdf_path, value: "/path/.../model.sdf"} # simulator=gazebo のとき必要
      - {name: init_world_name, value: "default"}          # simulator=gz のとき必要 (gz_sim 起動時の world 名)
```

`maps_path` と `map_name` は **必須** です。`maps_path` 未指定 / 存在しないパス / ディレクトリでない場合は `RCLCPP_FATAL` ログ + 終了コード 1 で起動失敗します (#163)。`mapoi_turtlebot3_example` の sample を流用するなら `$(find-pkg-share mapoi_turtlebot3_example)/maps` を指定してください。

### CLI から直接起動する例

include せず `ros2 launch` を直接叩く場合の最小例:

```bash
# mapoi_turtlebot3_example の sample maps を流用 (apt / 配布 install 後にも動く)
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=$(ros2 pkg prefix --share mapoi_turtlebot3_example)/maps \
  map_name:=turtlebot3_world

# ソースツリーから直接 (ros2_ws 内で source 後)
# 注: 本 repo を `<ws>/src/mapoi/` に clone している前提 (ws 直下で `git clone .../mapoi.git src/mapoi`)。
# パッケージを `<ws>/src/mapoi_turtlebot3_example/` のようにフラット展開している場合は
# パスを調整するか、上の `pkg prefix --share` 形式を使う方が確実。
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=$(pwd)/src/mapoi/mapoi_turtlebot3_example/maps \
  map_name:=turtlebot3_dqn_stage1
```

#### 引数の取り違え注意

各 launch arg のセマンティクスは以下のとおり (`{maps_path}/{map_name}/{config_file}` でアクセスする 3 レベルの分割):

| arg | 渡すもの | 例 |
|---|---|---|
| `maps_path` | **地図群を束ねる親ディレクトリ** (各地図は配下のサブディレクトリ) | `/path/to/maps` |
| `map_name` | `maps_path` 直下の地図サブディレクトリ名 | `turtlebot3_world` |
| `config_file` | 地図サブディレクトリ内の設定ファイル名 (default: `mapoi_config.yaml`) | `mapoi_config.yaml` |

実際にロードされるパスは `{maps_path}/{map_name}/{config_file}` で、上記例なら `/path/to/maps/turtlebot3_world/mapoi_config.yaml` となります。

`maps_path` / `map_name` 必須 (上節参照) の検証は **2 段階**: `map_name` 自体未指定なら `ros2 launch` が `Required launch argument 'map_name' was not provided` で fail (node 起動前 / launch system 側)、`maps_path` がディレクトリでない等のセマンティクス違反は `mapoi_server` ノード起動時に `RCLCPP_FATAL` で fail します。

**❌ よくある間違い**: `maps_path` に **config.yaml ファイルパスを直接指定** してしまう:

```bash
# WRONG: maps_path はファイルではなくディレクトリ
ros2 launch mapoi_server mapoi_bringup.launch.yaml \
  maps_path:=./maps/turtlebot3_dqn_stage1/mapoi_config.yaml \
  map_name:=turtlebot3_dqn_stage1
```

→ 起動時に `mapoi_server` ノードが以下を 1 行で出力して FATAL 終了します (`mapoi_server.cpp` 内の `RCLCPP_FATAL` の実ログ文字列そのまま・英日混在):

```
[FATAL] maps_path '...mapoi_config.yaml' does not exist or is not a directory. 正しい maps ディレクトリ path を指定してください (例: $(find-pkg-share mapoi_turtlebot3_example)/maps)。
```

`maps_path` は **地図群を束ねる親ディレクトリ** で、特定の地図ディレクトリやファイルではありません。ディレクトリ構成は本 README 末尾の [ディレクトリ構成](#ディレクトリ構成) 節を参照してください。

`simulator` arg はシミュレータ連動 bridge の起動を制御します:
- `gazebo` (Gazebo Classic / Humble): `mapoi_gazebo_bridge` を起動。operator map switch 時に Gazebo 内の `world_model` entity を入れ替え + ロボットを delete + spawn で **POI list 先頭** の座標に再生成 (#144 で旧 `initial_pose` system tag を廃止し、yaml 順序で表現する semantics に変更)
- `gz` (gz-sim / Jazzy): `mapoi_gz_bridge` + `parameter_bridge` (`ros_gz_bridge`) を起動。operator map switch 時に gz-sim 内の `world_model` entity を入れ替え + ロボットを `SetEntityPose` で **POI list 先頭** の座標に teleport (gz-sim では Classic と異なり set_pose service が使えるため delete + spawn は不要)
- `none` (default): bridge 起動なし (実機運用)

Web UI も使う場合は `mapoi_webui.launch.yaml` も併せて include:

```yaml
- include:
    file: "$(find-pkg-share mapoi_webui)/launch/mapoi_webui.launch.yaml"
    arg:
      - {name: maps_path, value: "/path/to/your/maps"}
      - {name: map_name, value: "initial_map_name"}
```

> **NOTE**: `mapoi_webui` はシステムタグを `mapoi_server` の `mapoi/get_tag_definitions` service 経由で取得します。`mapoi_webui` 単体を起動しても system tags の表示は service 通信になるため、`mapoi_server` の package install 自体は不要 (ただし service responder として `mapoi_server` ノードが起動している必要あり)。

TurtleBot3 を使った動作例は `mapoi_turtlebot3_example` パッケージを参照してください。

## ノード

### mapoi_server

地図・POI の設定ファイルを読み込み、情報提供サービスを公開するノードです。

#### パラメータ

| パラメータ名 | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `maps_path` | `string` | **REQUIRED** | 地図ディレクトリのパス (#163 で sample maps 廃止により必須化) |
| `map_name` | `string` | `turtlebot3_world` | 起動時に読み込む地図名 |
| `config_file` | `string` | `mapoi_config.yaml` | 設定ファイル名 |
| `state_path` | `string` | `""` (無効) | last-selected map を永続化する**書き込み可能**ディレクトリ (#297)。空で永続化無効 (従来挙動)。詳細は下記「map 選択の永続化と再起動時の復元」節 |

#### map 選択の永続化と再起動時の復元 (`state_path`、#297)

`mapoi_server` の再起動 (crash / supervisor 再起動) は新しい DDS publisher の出現なので、起動時の initial pose publish (#144) は後起動 subscriber だけでなく**稼働中の localization bridge にも新規サンプルとして届きます** (active push)。`state_path` 未設定 (default) では、operator が別 map へ切替済みでも再起動後は起動パラメータ `map_name` の先頭 POI が publish され、**走行中ロボットの自己位置が起動 map の先頭 POI へテレポート**します (map 切替なしの same-map 再起動でも同様)。加えて map context 自体が起動パラメータへ巻き戻るため、goal / route の POI 名解決も旧 map 基準になります。

`state_path` に書き込み可能なディレクトリを指定すると `<state_path>/last_selected_map` に現在の map 名が記録され (起動時 + `mapoi/select_map` 成功時、tmp + rename の atomic write)、この state file の**有無**で挙動が分かれます:

- **state file なし (真の初回起動)**: 従来どおり先頭 POI を publish (#144 の自動 initial pose を維持)
- **state file あり (運用中の再起動)**: last-selected map へ map context を復元した上で、initial pose は **clear (`poi_name` 空、subscriber は無視) のみ** publish。稼働中の自己位置には影響しない

運用上の注意:

- 再起動時の復元は起動パラメータ `map_name` より**優先**されます。明示的に別 map で上げ直したい場合は `mapoi/select_map` を呼ぶか、state file を削除してから起動してください (削除すると次回起動は初回起動扱いに戻る)
- `maps_path` を複数ロボット・複数構成で共有していても、`state_path` は**ロボットごとのローカルディレクトリ**を指定してください (例: `/var/lib/mapoi`)。パラメータ値の `~` は mapoi_server 側では展開しないため、**絶対パスで指定**するか、shell 展開が効く経路 (`state_path:=$HOME/.ros/mapoi` 等) で渡してください
- state file の map が `maps_path` 配下に存在しない場合 (地図削除・パス付け替え) は WARN を出して起動パラメータの map を使います (この場合も publish は clear のみ)
- `state_path` が書き込み不能な場合は起動時に `RCLCPP_FATAL` + 終了コード 1 で fail fast します (#163 の `maps_path` 検証と同じ方針)。運用中の書き込み失敗 (disk full 等) は WARN のみで稼働継続します

#### サービス

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/get_maps_info` | `GetMapsInfo` | 利用可能な地図一覧と現在の地図名を取得 |
| `mapoi/get_pois_info` | `GetPoisInfo` | 現在の地図の全 POI を取得 |
| `mapoi/get_route_pois` | `GetRoutePois` | ルート上の POI を取得。`route_name` に一致する route が無い場合は `success=false` + `error_message` (#342) |
| `mapoi/get_routes_info` | `GetRoutesInfo` | 利用可能なルート一覧を取得 |
| `mapoi/get_tag_definitions` | `GetTagDefinitions` | タグ定義（システム/ユーザー）を取得 |
| `mapoi/select_map` | `SelectMap` | 現在 map context を切り替え（Nav2 は呼ばない）。`map_name` は `maps_path` 直下の単一 path segment のみ受け付け、separator 入り / `.` / `..` は reject (#328) |
| `mapoi/reload_map_info` | `std_srvs/Trigger` | 設定ファイルを再読み込み |
| `mapoi/request_initial_pose` | `RequestInitialPose` | `{map_name, poi_name}` を受け取り `mapoi/initialpose_poi` を publish する唯一の経路 (#211)。空 `poi_name` は clear (subscriber は無視)。非空 `map_name` が現在 map と不一致の場合は publish せず `success=false` (#299) |

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/config_path` | `std_msgs/String` | 現在の設定ファイルのパス（定期配信、transient_local QoS） |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest` | initial pose 候補の通知（transient_local QoS、depth=1）。`mapoi/request_initial_pose` service 経由でのみ発火する唯一の publisher (#211) |

### mapoi_nav2_bridge

POI 名を指定した自律走行と、POI 半径イベント検知を行うノードです。

`MapoiNav2Bridge` クラスの実装は、保守性のため 5 つの translation unit に分割されています
(#345、2 段階の分割が完了): `src/mapoi_nav2_bridge.cpp` (ノード初期化、pause/resume/cancel
の dispatch、POI 半径/pause 判定エンジンなど共有の核)、`src/mapoi_nav2_bridge_route.cpp`
(route 走行: `FollowWaypoints` と mapoi 主導 waypoint 到達モード)、
`src/mapoi_nav2_bridge_goal.cpp` (単発 goal 走行: `NavigateToPose`)、
`src/mapoi_nav2_bridge_map_switch.cpp` (map switch: `switch_map` subscribe、
`select_map` 同期、Nav2 `LoadMap`、initial pose request)、
`src/mapoi_nav2_bridge_backend_status.cpp` (1Hz backend status publisher。独自の
callback group で動くので触る前にファイル冒頭のコメントを確認すること) です。
5 ファイルとも単一の `mapoi_nav2_bridge` 実行ファイルにコンパイルされ、この分割は内部
実装のみの変更であり、ノードの外部挙動 (topic / service / parameter / ログ) は変わりません。

#### パラメータ

| パラメータ名 | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `radius_check_hz` | `double` | `5.0` | POI 半径チェックの頻度 (Hz) |
| `hysteresis_exit_multiplier` | `double` | `1.15` | EXIT 判定の閾値倍率（チャタリング防止） |
| `map_frame` | `string` | `map` | TF の親フレーム |
| `base_frame` | `string` | `base_link` | TF の子フレーム |
| `auto_resume_timeout_sec` | `double` | `0.0` | `EVENT_PAUSED` 発火後の auto-resume timeout (秒、#231)。`0.0` で disabled (現行仕様、外部 `/mapoi/nav/resume` を無限待ち)。正値で「PAUSED から N 秒後に内部 resume を呼ぶ」demo / 自動運転シナリオ向け opt-in 動作を有効化。負値は起動時に `0.0` へ clamp。動的 reconfigure 非対応 |
| `cmd_vel_topic` | `string` | `cmd_vel` | `EVENT_PAUSED` 判定用 `cmd_vel` の subscribe 先 topic 名。Nav2 と同じ topic を見る前提 (停止判定 source) |
| `cmd_vel_msg_type` | `string` | `auto` | `cmd_vel` の message 型 (#249 / #251)。`twist` / `twist_stamped` / `auto` (default = `ROS_DISTRO` から自動選択)。詳細と override が必要なケースは下記サブセクション参照 |
| `waypoint_arrival_mode` | `string` | `nav2` | route の waypoint 到達判定の主導権 (#243)。`mapoi` では単発 Go も POI 個別 `tolerance.xy`+`tolerance.yaw` で到達判定する (#261)。`nav2` / `mapoi`。詳細は下記「waypoint 到達モード」節参照。動的 reconfigure 非対応 (起動時に解決、未知値は `nav2` にフォールバック) |

##### `cmd_vel_msg_type` の値と override が必要な構成 (#249 / #251)

`cmd_vel` の publisher 型は ROS 2 distro / 採用 controller によって `geometry_msgs/Twist` と `geometry_msgs/TwistStamped` に分かれる:

- `twist` — `geometry_msgs/Twist` で subscribe (humble Nav2 互換)
- `twist_stamped` — `geometry_msgs/TwistStamped` で subscribe (jazzy 以降 Nav2: `collision_monitor` / `docking_server` 等が TwistStamped 化済)
- `auto` (default) — `ROS_DISTRO` 環境変数で自動選択 (`humble` → `twist`、それ以外 / 未設定 → `twist_stamped`)

**型不一致は単に subscribe が空になるのではなく process が落ちる**: 同じ topic に違う型の subscriber を 2 つ作ると rcl が `invalid allocator` で crash する (#249 で実機再発)。`auto` の選ぶ型と実際に流れる publisher 型を揃えるのが大前提。

`auto` を override する必要があるのは下記の混在構成:

| ケース | override |
| --- | --- |
| jazzy 上で自前 controller が Twist のまま publish | `cmd_vel_msg_type: "twist"` を明示 |
| humble 上で自前 controller が TwistStamped を先取りしている | `cmd_vel_msg_type: "twist_stamped"` を明示 |
| 最小 CI / 自作 container で `ROS_DISTRO` が unset、かつ humble を使う | `cmd_vel_msg_type: "twist"` を明示 (default fallback は `twist_stamped` のため) |

未知値 (typo) は WARN を出した上で `ROS_DISTRO` ベースで安全側にフォールバックする。

##### waypoint 到達モード (`waypoint_arrival_mode`、#243)

route 走行中に「ある waypoint に到達したので次へ進む」判定を **どちらが主導するか** を切り替える。

| 値 | 到達判定の主導 | tolerance.xy と xy_goal_tolerance の関係 |
| --- | --- | --- |
| `nav2` (default) | Nav2 `FollowWaypoints` が `xy_goal_tolerance` で waypoint を進行。mapoi は radius observer (`EVENT_ENTER` 等を出すだけ) | `tolerance.xy` は `xy_goal_tolerance` **より大きく** 取る必要がある (Nav2 が radius に入る前に goal 判定で止まると ENTER が出ないため) |
| `mapoi` | mapoi が 1 waypoint ずつ `NavigateToPose` を送り、**到達 = OR((`tolerance.xy` ∧ `tolerance.yaw`) ∨ Nav2 SUCCEEDED)** で次へ進める (#265) | `tolerance.xy < xy_goal_tolerance` が可能 (POI を小さくできる) |

**`mapoi` モードの設計ポイント:**

- **統一到達 (#265)**: route 中間 waypoint / 最終 goal / 単発 Go (`mapoi/nav/goal_pose_poi`) すべて **OR((`tolerance.xy` ∧ `tolerance.yaw`) ∨ Nav2 SUCCEEDED)** で判定する。robot が半径内かつ姿勢が `tolerance.yaw` 以内に自然に収まっていれば Nav2 完走を待たず即到達/前進 (snappy)、ずれていれば Nav2 の姿勢合わせ (SUCCEEDED) を待つ。radius 進入後にその場旋回して yaw が合うケースも毎 tick 再評価で拾う。`nav2` モードでは route も Go も従来通り Nav2 任せ。
- **per-POI `tolerance.yaw` が「yaw を見るか」のつまみ**: 到達判定の角度差は `[0, π]` (最大 π ≈ 180°)。ただの通過点は `tolerance.yaw` を π 以上 (`3.1416` 等、デモ config の `basic_waypoint` と同値) にすれば完全に yaw 不問で fly-through。π 直下 (`3.14`) でも実質同等だが、真後ろ ±0.09° のごく狭い帯だけ radius 未到達になる (Nav2 SUCCEEDED が保険)。向きが要る点 (撮影・最終 goal 等) は小さくして姿勢を効かせる。`tolerance.yaw` を実効的に効かせるには (xy と同様) Nav2 の `yaw_goal_tolerance` を `tolerance.yaw` 以下まで下げる (OR の実効許容は緩い方になるため)。**注**: yaw 不問 (`tolerance.yaw >= π`) の POI は RViz/WebUI で扇形の代わりに塗りつぶし円が重ね描きされる (#267)。
- **OR 到達 (スタック防止)**: `tolerance.xy`/`tolerance.yaw` を Nav2 の goal_checker より小さくしても、Nav2 SUCCEEDED を保険にすることで route が止まらない (AND だけだとデッドロックしうる)。
- **実効到達半径を真に小さくするには Nav2 も寄せる**: OR の実効到達半径は「緩い方」になる。`tolerance.xy=0.15` でも Nav2 が `xy_goal_tolerance=0.25` で止まれば実効 0.25 のまま (ENTER も出ない)。POI を実際に小さくするには `param/<distro>/burger.yaml` の `goal_checker.xy_goal_tolerance` も `tolerance.xy` 以下まで下げる。
- **landmark は対象外**: `landmark` は waypoint ではなく ENTER (音声等) トリガ半径なので本モードの進行判定に関与せず、半径は経路に掛かる程度に広いままでよい。
- **pause waypoint**: 統一到達 (`tolerance.xy` ∧ `tolerance.yaw`) で auto-pause し、`mapoi/nav/resume` で次 waypoint へ進む。撮影で向きを問うなら `tolerance.yaw` を小さく、問わないなら大きく。

**demo (turtlebot3_example) で試す:**

```sh
# mapoi モードで起動 (POI を小さくする実験には burger.yaml の goal_checker.xy_goal_tolerance も下げる)
ros2 launch mapoi_turtlebot3_example turtlebot3_navigation.launch.yaml waypoint_arrival_mode:=mapoi
```

demo は `mapoi` モード既定 (#263)。sample config (`turtlebot3_world/mapoi_config.yaml`) は POI ごとに `tolerance.yaw` を設定済み (通過点は広め、最終 `goal` は `0.1` で厳密)。`mapoi` モードで POI を縮小する場合は yaml の `tolerance` と Nav2 param をセットで sim 確認しながら調整する。

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/nav/switch_map` | `std_msgs/String` | operator map switch 指示。受信後に `mapoi/select_map` → Nav2 `LoadMap` → `mapoi/request_initial_pose` service 経由で `mapoi_server` が `mapoi/initialpose_poi` を publish (= localization bridge へ初期位置 trigger) を実行 |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | 指定した POI 名の位置に自律走行 |
| `mapoi/nav/route` | `std_msgs/String` | 指定したルート名のウェイポイントを順に走行 |
| `mapoi/nav/pause` | `std_msgs/String` | ナビゲーションの一時停止 |
| `mapoi/nav/resume` | `std_msgs/String` | ナビゲーションの再開 |
| `mapoi/nav/cancel` | `std_msgs/String` | ナビゲーションのキャンセル |
| `mapoi/config_path` | `std_msgs/String` | マップ切替検知（transient_local QoS） |

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `goal_pose` | `geometry_msgs/PoseStamped` | ゴール位置の配信 |
| `mapoi/nav/status` | `std_msgs/String` | ナビゲーション状態を `"status"` または `"status:target"` 形式で配信（例: `"navigating:kitchen"`、`"succeeded:patrol_route"`、`"paused:patrol_route"`、`"map_switching:turtlebot3_world"`、`"rejected:kitchen"`）。`status` は `navigating` / `succeeded` / `aborted` / `canceled` / `paused` / `map_switching` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected`。`backend_unavailable` は Nav2 action / service 不在で goal / route / resume を実行できなかった場合（#198）。`rejected` は goal / route コマンドを **受理する前** に無効と判定し実行しなかった場合（存在しない POI 名、landmark タグ POI を goal 指定、`mapoi/get_pois_info` / `mapoi/get_route_pois` service 未 ready、route の waypoints が空、等）（#339, #355）。`target` は POI 名（goal mode）、route 名（route mode）、または map 名で、subscriber 側は最初の `:` で split して復元する（target 内に `:` が含まれても残り全体を target として扱える）。`transient_local` QoS の **現在状態 snapshot**（depth=1）で、後起動 subscriber が最後の状態を受信できるが状態遷移履歴は復元できない。現在走行中かは `navigating` / `paused` / `map_switching` で判定し、終端状態（`succeeded` / `aborted` / `canceled` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected`）は直近結果として扱う |
| `mapoi/nav/command_rejected` | `std_msgs/String` | reject コマンドの**イベント通知**（#354）。payload は reject 対象の target 文字列のみ。`mapoi/nav/status` は latched な状態 snapshot のため、走行中（`nav_mode_ != IDLE`）の reject は "rejected" を書けない（#339、上記参照）。本 topic はそれとは独立したイベント軸で、`publish_rejected_status` の先頭で **nav_mode_ に関わらず reject の都度必ず publish** する（走行中に typo goal 等を送っても操作者が気づけるようにするため）。QoS は volatile（非 `transient_local`、depth=10）— イベントは「今」しか意味を持たないため後起動 subscriber へのリプレイは行わない。WebUI はこれを購読して SSE 経由の一時的な toast 表示に使う（`mapoi_webui/README.ja.md` 参照） |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | navigation bridge の readiness summary を 1Hz で配信（#198）。`transient_local` QoS。Minimal 3 フィールド（`backend_type` / `backend_ready` / `reason`）。WebUI / panel は `backend_ready` で navigation 操作 UI を一括 gate する。詳細は [docs/backend-status.ja.md](../docs/backend-status.ja.md) を参照 |
| `mapoi/events` | `mapoi_interfaces/PoiEvent` | route 走行中の POI 侵入 (`EVENT_ENTER`) / pause POI で navigation 停止 (`EVENT_PAUSED`) / 退出 (`EVENT_EXIT`) イベント (#220) |

#### アクションクライアント

| アクション名 | 型 | 説明 |
| --- | --- | --- |
| `follow_waypoints` | `nav2_msgs/FollowWaypoints` | ウェイポイント追従 |
| `navigate_to_pose` | `nav2_msgs/NavigateToPose` | 単一ゴールナビゲーション |

#### サービスクライアント

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `request_initial_pose` | `RequestInitialPose` | Nav2 LoadMap 完了後、`mapoi_server` に initial pose publish を依頼 (#211)。mapoi_nav2_bridge は直接 `mapoi/initialpose_poi` を publish しない |

#### POI 半径イベント検知 (`PoiEvent`)

`mapoi/events` topic で 3 種別の event を publish します (#220 で 4 種別 → 3 種別に簡素化)。検知対象は **route 走行中 (`nav_mode == ROUTE`)** + **route 登録 POI** のみで、route 走行外 (`IDLE` / `GOAL` mode) では event は発火しません。`waypoint_arrival_mode` には依存せず、`FollowWaypoints` 経路 (`nav2` モード) でも waypoint 毎 `NavigateToPose` 経路 (`mapoi` モード、demo 既定) でも発火します。

| event_type | 発火条件 |
| --- | --- |
| `EVENT_ENTER` | route POI の `tolerance.xy` 半径内へ侵入 |
| `EVENT_PAUSED` | `pause` タグ付き POI の `tolerance.xy` 内で **navigation 停止** (cmd_vel dwell で検知)、1 visit につき 1 回のみ |
| `EVENT_EXIT` | route POI から `tolerance.xy * hysteresis_exit_multiplier` を超えて退出 |

検知の前提:

- TF lookup (`map` -> `base_link`) でロボット位置を取得（デフォルト 5Hz）
- `pause` タグ付き POI では侵入時に走行を自動一時停止 (併せて `EVENT_PAUSED` を nav 停止後に publish)
- `EVENT_PAUSED` は採用 controller が **navigation 停止中も cmd_vel = 0 を継続 publish** する前提 (Nav2 default の挙動)。controller が静止時に cmd_vel publish を止める実装の場合、`EVENT_PAUSED` は発火しません
- マップ切替時は内部状態をリセットし、新しい POI リストで監視を再開
- `RESUMED` 相当の event はありません (resume は client 側 request + `mapoi/nav/status` で観測可能)
- `auto_resume_timeout_sec > 0.0` を設定すると、`EVENT_PAUSED` 発火後 N 秒で内部的に resume を呼ぶ opt-in 動作になります (#231)。外部 `/mapoi/nav/resume` が timer より先に届けば pending timer はキャンセル、route cancel / 別 route 投入 / 新たな PAUSED 発火でも上書きキャンセルされます。default `0.0` (= disabled) では現行仕様 (無限待ち) を維持します

### mapoi_amcl_localization_bridge

AMCL 互換 localization (`/initialpose` を `geometry_msgs/PoseWithCovarianceStamped` で受ける構成) 向けの localization bridge ノードです (#209)。`mapoi_nav2_bridge` から AMCL adapter を分離して、Navigation backend と Localization backend を独立した仕様で扱えるようにしています。slam_toolbox / NDT / 自前 localization に切替える場合は、本 bridge の代替として同じ topic 仕様を満たす自作 bridge を用意してください（[docs/backend-status.ja.md](../docs/backend-status.ja.md) の「Localization backend 仕様」節参照）。

#### パラメータ

| パラメータ名 | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `initial_pose_topic` | `string` | `/initialpose` | initial_pose の配信先 topic 名 (非 AMCL の localization に対応する場合に変更) |
| `initialpose_retry_interval_sec` | `double` | `0.1` | subscriber 後起動 / 検知後 republish の timer 間隔 (秒)。`0.01` 未満は default に clamp |
| `initialpose_retry_max_attempts` | `int` | `50` | subscriber 検知前の最大 wait 試行回数 (= `interval_sec × max_attempts` 秒で諦め)。default = 約 5 秒 |
| `initialpose_post_subscribe_republish_count` | `int` | `3` | subscriber 検知後に追加 republish する回数。AMCL の「visible だが処理 ready 直前」取りこぼしを防ぐ |
| `map_frame` | `string` | `map` | `PoseWithCovarianceStamped.header.frame_id` |

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest` | 初期位置に採用する POI 名 (`{map_name, poi_name}`)。`mapoi/get_pois_info` で resolve した pose を `/initialpose` に流す。空 `poi_name` は無視 |

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `initialpose` | `geometry_msgs/PoseWithCovarianceStamped` | 初期位置の配信 (topic 名は `initial_pose_topic` parameter で変更可)。`mapoi/initialpose_poi` で受信した POI の pose を流す。subscriber 後起動 / 検知後 ready 直前ケースに備えて wall_timer ベースで非同期 retry + post-subscribe republish する (`initialpose_retry_*` parameter 群、#152) |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | localization bridge の readiness summary を 1Hz で配信 (#209)。`transient_local` QoS。Minimal 3 フィールド (`backend_type` / `backend_ready` / `reason`)。`backend_ready` は `/initialpose` の subscriber 数 > 0 を ready proxy として採用。WebUI / panel は `backend_ready` で Initial Pose UI を gate する |

#### サービスクライアント

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/get_pois_info` | `GetPoisInfo` | POI 名から pose を resolve するために `mapoi_server` を呼び出す |

#### 対応 localization パッケージの要件

`mapoi_amcl_localization_bridge` は以下を満たす localization パッケージで動作します:

- `geometry_msgs/PoseWithCovarianceStamped` を topic で受信できる (default `/initialpose`)
- 動的 (起動後の operator map switch) でも上記 topic 経由で初期位置を受け付ける

**動作確認済み**: Nav2 AMCL (Humble / Jazzy)。

**別 topic を使うパッケージ** (例: 自社実装) は、`initial_pose_topic` parameter で配信先を変更できます。受信 message 型が `PoseWithCovarianceStamped` でない場合は、本 bridge を停止して同じ仕様を満たす自作 bridge を用意してください（[docs/backend-status.ja.md](../docs/backend-status.ja.md) の「Localization backend 仕様」節を参照）。

**注意**: `/mapoi/nav/switch_map` 経由の map 入替は Nav2 `LoadMap` service 経由のため、Nav2 lifecycle に乗らない localization では map 入替動作の整合は別途検証・対応が必要です。

### mapoi_rviz2_publisher

RViz2 上に POI のマーカーを表示するためのノードです。

#### パラメータ

| パラメータ名 | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `show_tolerance_sector` | `bool` | `true` | POI tolerance visualization (#136 / #179) を表示するか。対象 layer = xy 判定円 outline + yaw 制約扇形 (`0 < tolerance.yaw < π` の時のみ重ね描き) + pause overlay (xy 円沿いの dot pattern)。`tolerance.yaw >= π` (yaw 不問) の場合は扇形の代わりに塗りつぶし円を重ね描きする (#267)。`false` で全 POI のこれら 3 layer を抑制 (Editor 中心の使い方や RViz が情報過多な時の用途)。WebUI 側にも同等の描画仕様 (`mapoi_webui/web/js/map-viewer.js`) があるため、仕様変更時はペアで更新する |
| `poi_label_format` | `string` | `"index"` | POI label の表示形式: `"index"` = POI Editor 行番号 (1-based 通し、tag フィルタ非依存) / `"name"` = POI 名 / `"both"` = `"<index>: <name>"` / `"none"` = 非表示 |
| `route_display_mode` | `string` | `"selected"` | Route marker の表示形式: `"all"` = 全 route 表示 (active route は太線 + 不透明で強調) / `"selected"` = active route のみ表示 / `"none"` = 表示しない |

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/highlight/goal` | `std_msgs/String` | ハイライトするゴール POI 名 |
| `mapoi/highlight/route` | `std_msgs/String` | ハイライトするルート POI 名（カンマ区切り） |

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/markers/pois` | `visualization_msgs/MarkerArray` | POI マーカー。`waypoint` は緑、`landmark` は灰、custom tag のみの POI は青で描画 |
| `mapoi/markers/routes` | `visualization_msgs/MarkerArray` | Route マーカー。route line と選択中 route の方向矢印を描画 |

`mapoi/markers/pois` は Marker namespace で layer を分けています。RViz の `Namespaces` toggle で layer 単位に表示を切り替えられます。

| Marker namespace | 説明 |
| --- | --- |
| `arrow/<poi_name>` | POI の向き矢印と label |
| `tolerance_xy/<poi_name>` | `tolerance.xy` の判定円 |
| `tolerance_yaw/<poi_name>` | `tolerance.yaw` の扇形 |
| `status_paused/<poi_name>` | `pause` tag POI の dot overlay |

## タグシステム

POI にはタグを付与して用途を分類できます。タグは **システムタグ** と **ユーザータグ** の2種類があります。

### システムタグ

`include/mapoi_server/system_tags.hpp` 内に compile-time 定数として定義されるグローバルなタグです (`mapoi::kSystemTags`)。`mapoi/get_tag_definitions` service で配信されます。system tag はコア機能と一体のため変更非推奨で、増減はコア改修扱いです。

| タグ名 | 説明 |
| --- | --- |
| `waypoint` | Nav2 navigation の到達対象（単発 navigation goal、route の中間点いずれも対応） |
| `landmark` | Nav2 navigation 対象にしない参照専用 POI（可視化のみ、waypoint 候補と initial pose 候補から除外） |
| `pause` | ロボットが POI 半径内に入ったとき、**ROUTE 走行中かつ active route の `waypoints` / `landmarks` に含まれる POI** であれば自動的に一時停止（#143）。GOAL 走行中・IDLE では発火しない。`landmark` との併用は不可 |

### ユーザータグ

各地図の `mapoi_config.yaml` 内の `custom_tags` セクションで定義します。

```yaml
custom_tags:
  - name: audio_info
    description: "音声ガイドのトリガー"
```

## 設定ファイル (mapoi_config.yaml)

各地図ディレクトリに `mapoi_config.yaml` を配置して、地図と POI を設定します。

```yaml
custom_tags:
  - name: audio_info
    description: "音声ガイドのトリガー"

map:
  path_planning: map_file_name
  localization: map_file_name

poi:
  # POI 名は構造を示す汎用例 (turtlebot3 demo の実 POI 名とは独立)
  - name: entrance
    pose: {x: -2.0, y: -0.5, yaw: 0.0}
    tolerance: {xy: 0.5, yaw: 0.785}
    tags: [waypoint]                # POI list 先頭が default initial pose に採用される (#144)
    description: エントランス
  - name: checkpoint_a
    pose: {x: 0.5, y: 0.5, yaw: 0.0}
    tolerance: {xy: 0.5, yaw: 0.785}
    tags: [waypoint, pause]   # tolerance.xy 半径内に入ると自動一時停止
    description: 中間チェックポイント
  - name: info_point_a
    pose: {x: 1.0, y: 2.0, yaw: 1.57}
    tolerance: {xy: 1.0, yaw: 0.785}
    tags: [audio_info]
    description: 音声案内ポイントA

route:
  - name: route_1
    waypoints: [entrance, info_point_a]
```

### フィールド説明

- **custom_tags**: ユーザー定義タグ
  - `name`: タグ名
  - `description`: タグの説明
- **map**: 使用する地図ファイルの設定
  - `path_planning`: 経路計画用の地図ファイル名
  - `localization`: 自己位置推定用の地図ファイル名
- **poi**: POI の定義（**順序が semantics を持ちます**: 各 map で `poi:` 配下の **先頭 POI** が地図ロード/切替時の default 初期位置として採用される。先頭は `landmark` タグなし & `pose.x/y/yaw` が完備された POI である必要あり。明示指定したい場合は `SelectMap.srv` の `initial_poi_name` で POI 名を渡す、#144）
  - `name`: POI の名前（トピックで指定する際に使用）
  - `pose`: 位置（`x`, `y`, `yaw` すべて必須。default 初期位置候補として使われる場合は欠落不可）
  - `tolerance`: Nav2 align tolerance struct
    - `xy`: Euclidean tolerance (m)。POI 進入判定半径としても使用される
    - `yaw`: Angular tolerance (rad)。`0` = 未指定として Nav2 default にフォールバック
  - `tags`: タグのリスト
  - `description`: 説明文
- **route**: ルートの定義
  - `name`: ルート名
  - `waypoints`: 巡回する POI 名のリスト（`waypoint` タグ付き POI を Nav2 `FollowWaypoints` に送る）
  - `landmarks` (任意): この route 走行中に意識する補助 POI 名のリスト（#143）。Nav2 へは送らず radius event 監視と pause 発火スコープにのみ使う。`landmark` タグ付き POI を指定する想定
- **gazebo** (任意、`mapoi_gazebo_bridge` のみが参照): operator map switch 時に入れ替える Gazebo Classic 側のモデル定義
  - `world_model.uri`: モデル URI (例: `model://turtlebot3_world`)
  - `world_model.name`: Gazebo 内での entity 名 (delete/spawn のキー)

## ディレクトリ構成

`maps_path` 配下は地図ごとのサブディレクトリで構成されます (システムタグ定義は `include/mapoi_server/system_tags.hpp` 内に const として保持されるため、この dir には含まれません)。

```
maps/
└── <地図名>/
    ├── mapoi_config.yaml    # POI・ルート・ユーザータグ設定
    ├── <地図名>.yaml        # 地図メタデータ
    └── <地図名>.pgm         # 地図画像
```
