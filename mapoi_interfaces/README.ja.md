# mapoi_interfaces

> English version (primary): [README.md](./README.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

mapoi パッケージ群で使用するメッセージとサービスの定義パッケージです。

## メッセージ (msg)

### Tolerance.msg

POI の到達判定 tolerance（Nav2 `SimpleGoalChecker` の `xy_goal_tolerance` / `yaw_goal_tolerance` と align）。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `xy` | `float64` | Euclidean tolerance (m)。POI 進入判定半径としても使われる |
| `yaw` | `float64` | Angular tolerance (rad)。`0` = 未指定として Nav2 default にフォールバック |

### PointOfInterest.msg

POI（Point of Interest）を表すメッセージです。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `name` | `string` | POI の名前。実質的な一意キー |
| `pose` | `geometry_msgs/Pose` | POI の位置・姿勢 |
| `tolerance` | `mapoi_interfaces/Tolerance` | xy / yaw tolerance（v0.3.0 で旧 `radius` から置換） |
| `tags` | `string[]` | POI に紐づくタグ（例: system tag `waypoint` / `landmark` / `pause`、user 定義 tag なら `audio_info` 等） |
| `description` | `string` | POI の説明 |

### PoiEvent.msg

route 走行中に route 登録 POI への侵入 / 退出、および `pause` タグ付き POI で navigation 停止が発生した時に publish されるイベントメッセージです (#220 で 4 種別 → 3 種別に簡素化)。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `EVENT_ENTER` | `uint8` (定数=1) | route 走行中に route 登録 POI の `tolerance.xy` 半径へ侵入 (`nav_mode == ROUTE` かつ `current_route_poi_names_` 該当 POI のみ、route 走行外 (IDLE / GOAL mode) では publish しない) |
| `EVENT_PAUSED` | `uint8` (定数=2) | `pause` タグ付き POI の `tolerance.xy` 内で navigation 停止 (cmd_vel dwell で検知)。pause 自動 trigger 後に Nav2 が停止状態に入った瞬間 |
| `EVENT_EXIT` | `uint8` (定数=3) | route 登録 POI から `tolerance.xy * hysteresis_exit_multiplier` を超えて退出 |
| `event_type` | `uint8` | イベント種別 (上記 3 種のいずれか) |
| `poi` | `mapoi_interfaces/PointOfInterest` | 対象 POI の情報 |
| `stamp` | `builtin_interfaces/Time` | イベント発生時刻 |

ライフサイクル:
```
EVENT_ENTER -> [EVENT_PAUSED (only if pause-tagged + nav stops)] -> EVENT_EXIT
```

`EVENT_PAUSED` の前提:
- 採用 controller が **navigation 停止中も cmd_vel = 0 を継続 publish する** こと (Nav2 default の挙動)。controller が静止時に cmd_vel publish を止める実装の場合、`EVENT_PAUSED` は発火しません。
- pause 自動 trigger (と `EVENT_PAUSED` の publish) は `mapoi_nav2_bridge` 側で実施し、resume は client 側 `mapoi/nav/resume` request で発動するため、`RESUMED` 相当の event は本仕様に含めません (resume は status topic で観測可能)。

### TagDefinition.msg

POI 分類タグ (system tag / user tag) の単一定義を表すメッセージです。`GetTagDefinitions.srv` のレスポンス成分としても使えるよう PR #193 で導入されました。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `name` | `string` | タグ名 (例: system tag `waypoint` / `landmark` / `pause`、user 定義 tag なら `audio_info` 等) |
| `description` | `string` | タグ用途の human-readable 説明 |
| `is_system` | `bool` | `true` = システムタグ、`false` = ユーザー定義タグ |

### InitialPoseRequest.msg

operator の map switch / reload に伴う「初期姿勢候補 POI」通知メッセージ (#149 round 8 で文字列ペアから型化)。唯一の writer である `mapoi_server` が `mapoi/request_initial_pose` service 経由の依頼を受けて publish するほか (#211。例: `mapoi_nav2_bridge` が Nav2 `LoadMap` 成功後に依頼)、起動時の default POI 採用・reload 時の stale clear でも自発 publish し、localization bridge 群が subscribe します。詳細・stale 排除戦略は `msg/InitialPoseRequest.msg` の冒頭コメント参照。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `map_name` | `string` | 通知対象の map 名 (世代識別用)。非空なら publish 時点の現在 map と一致 (#299)。map 不明の requester が service に空を渡した場合のみ空文字がありうる |
| `poi_name` | `string` | 採用する POI 名 (空文字 = 「候補なし」、subscriber は無視) |

QoS は `transient_local` (depth=1) で後起動 subscriber も latched 値を受信できます。

### NavigationBackendStatus.msg

Navigation bridge (Nav2 bridge / 自前 bridge) が 1 Hz で `mapoi/nav/backend_status` に publish する readiness メッセージです。UI 側 (`mapoi_webui`, `mapoi_rviz_plugins`) は `backend_ready` を見て navigation 操作を gate します。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `backend_type` | `string` | bridge 識別子 (例: `nav2`, `custom_lidar_planner`)。tooltip 表示用 |
| `backend_ready` | `bool` | bridge が navigation コマンドを受け付け実行できる状態か |
| `reason` | `string` | `backend_ready=false` 時の human-readable 理由 (任意、機微情報は含めない — 下記参照) |

#### QoS contract (issue #208)

publisher / subscriber 双方で以下を必ず指定:

- `durability`: `TRANSIENT_LOCAL` (late-joiner UI が最新 status を受信できる)
- `reliability`: `RELIABLE`
- `liveliness` (publisher): `MANUAL_BY_TOPIC` (publish() ごとに liveliness assert)
- `liveliness` (subscriber): `AUTOMATIC` (pub `MANUAL_BY_TOPIC` × sub `AUTOMATIC` のみ互換)
- `liveliness_lease_duration`: 5 s (両側、`pub.lease <= sub.lease` を満たす)

詳細・違反パターンは `msg/NavigationBackendStatus.msg` の冒頭コメント参照。

#### Custom bridge 実装者向けガイダンス (issue #207)

`backend_ready` の算出は bridge ごとに「実際に expose する capability の AND」とすること。`mapoi_nav2_bridge` (Nav2 bridge) の `goal_ready && route_ready && switch_map_ready` は **3 capability 全部を expose する bridge にのみ正しい**。片機能 bridge (例: NavigateToPose のみ) でこれを真似ると、expose していない capability が常に false → `backend_ready` も常に false → UI が常に "Navigation unavailable" になる。

具体例とフィールド populate 方針 (`reason` の慣習表記含む) は `msg/NavigationBackendStatus.msg` の冒頭コメントに記載。

`reason` は operator UI に表示され bag や remote dashboard に記録され得るため、credentials / token / 絶対パス / 内部 hostname / IP / user identifier / stack trace 等の機微情報を含めないこと。capability 名と短い状態動詞 (例: `not ready: navigate_to_pose action`) に留める。

> **CI lint の責務範囲** (`scripts/check_docs_consistency.py`、PR #217 / Close #216): static check は `publish_backend_status` 系関数中の **明示的な string literal** のみを走査する。パラメータ連結 (`reason = "ip=" + this->get_parameter(...).as_string()`) など動的に組み立てられる文字列、および raw string literal (`R"(...)"`) は責務外。lint 通過 ≠ 完全な redaction 保証。bridge 実装者は dynamic 部分にも本節の禁止事項を適用すること。

### LocalizationBackendStatus.msg

Localization bridge (`mapoi_amcl_localization_bridge` / 自前 bridge) が 1 Hz で `mapoi/localization/backend_status` に publish する readiness メッセージ (#209)。UI 側は `backend_ready` を見て Initial Pose 操作を gate します。Navigation backend (上記 `NavigationBackendStatus`) とは独立した軸で、それぞれ別 indicator として扱います。

| フィールド | 型 | 説明 |
| --- | --- | --- |
| `backend_type` | `string` | bridge 識別子 (例: `amcl`, `slam_toolbox`, `custom_lidar_amcl`)。tooltip 表示用 |
| `backend_ready` | `bool` | bridge が新しい initial pose を受け付け転送できる状態か |
| `reason` | `string` | `backend_ready=false` 時の human-readable 理由 (任意、機微情報禁止 — `NavigationBackendStatus` の節と同方針) |

QoS contract (TRANSIENT_LOCAL / RELIABLE / MANUAL_BY_TOPIC publisher / AUTOMATIC subscriber / 5s lease) は `NavigationBackendStatus` と同一です。詳細は `msg/LocalizationBackendStatus.msg` の冒頭コメント参照。

## サービス (srv)

### SelectMap.srv

現在の地図 context を切り替える Nav2-free サービスです。Operator mode の地図切替は `/mapoi/nav/switch_map` topic で指示し、`mapoi_nav2_bridge` がこの service を呼んだ後に Nav2 `LoadMap` を実行します。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Request | `map_name` | `string` | 選択する地図名 |
| Request | `initial_poi_name` | `string` | 初期姿勢に使う POI 名。空なら先頭の有効 POI |
| Response | `success` | `bool` | context 選択成功の有無 |
| Response | `error_message` | `string` | 失敗時の理由 |
| Response | `config_path` | `string` | 選択後の設定ファイル path |
| Response | `resolved_initial_poi_name` | `string` | 解決済み初期姿勢 POI 名 |
| Response | `nav2_node_names` | `string[]` | Nav2 map server node 名 |
| Response | `nav2_map_urls` | `string[]` | 対応する map YAML path |

### RequestInitialPose.srv

`mapoi/initialpose_poi` (唯一の writer = `mapoi_server`) への publish を依頼するサービスです (#211)。従来は `mapoi_server` / `mapoi_nav2_bridge` / WebUI / RViz panel の 4 つが直接 publish していましたが、`transient_local` の latched cache は writer ごとに保持されるためクロス writer の stale 競合がありました。全 publish を本 service 経由で `mapoi_server` 1 つに集約することで latched cache を単一化し、clear が真の last-write-wins で効くようにします。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Request | `map_name` | `string` | 対象の地図名 (`InitialPoseRequest.msg` の `map_name` をミラー)。非空なら `mapoi_server` の現在 map と一致必須で、不一致は publish されず `success=false` (#299)。空 = map 不明の requester として検証なしで透過 |
| Request | `poi_name` | `string` | 採用する POI 名。空文字は clear (skip sample を publish、subscriber は無視) |
| Response | `success` | `bool` | publish に成功したら true (`map_name` 不一致 reject 時は false、#299) |
| Response | `error_message` | `string` | 失敗時の理由 (非空) |

### GetMapsInfo.srv

利用可能な地図の一覧と現在の地図名を取得するサービスです。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Response | `maps_list` | `string[]` | 利用可能な地図名のリスト |
| Response | `map_name` | `string` | 現在の地図名 |

### GetPoisInfo.srv

現在の地図に登録されている全 POI を取得するサービスです。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Response | `pois_list` | `PointOfInterest[]` | POI のリスト |

### GetRoutePois.srv

指定されたルートに含まれる POI を、走行対象の waypoint と参照専用の landmark に分けて取得するサービスです (#143)。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Request | `route_name` | `string` | ルート名 |
| Response | `success` | `bool` | route が見つかった場合 `true`（POI 0 件でも route が存在すれば `true`）(#342) |
| Response | `error_message` | `string` | `success=false` 時のみ非空。route 不存在の説明 (#342) |
| Response | `pois_list` | `PointOfInterest[]` | ルート上の waypoint POI（順序付き。Nav2 へは `FollowWaypoints`、または `waypoint_arrival_mode=mapoi` 時は waypoint 毎の `NavigateToPose` で送られる） |
| Response | `landmark_pois` | `PointOfInterest[]` | ルートに紐づく landmark POI。Nav2 の走行対象外だが、route 走行中は radius 監視される。順序は参考情報 |

### GetRoutesInfo.srv

利用可能なルートの一覧を取得するサービスです。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Response | `routes_list` | `string[]` | ルート名のリスト |

### GetTagDefinitions.srv

タグ定義（システムタグ・ユーザータグ）を取得するサービスです。

| 方向 | フィールド | 型 | 説明 |
| --- | --- | --- | --- |
| Response | `tag_names` | `string[]` | タグ名のリスト |
| Response | `tag_descriptions` | `string[]` | タグ説明のリスト |
| Response | `is_system` | `bool[]` | システムタグかどうか（`true` = システムタグ） |
