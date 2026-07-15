# Navigation / Localization backend 仕様

> English version (primary): [backend-status.md](./backend-status.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

mapoi は navigation と localization の 2 つの backend を **独立した契約** として扱います。両者ごとに専用の bridge ノードを起動し、`backend_status` topic で readiness を WebUI / RViz panel に伝えます。独自スタックを mapoi の UI から扱いたい場合は、bridge ノードを自作して以下の topic 仕様を満たしてください。

## Navigation backend 仕様

`mapoi_nav2_bridge` は実質的に Nav2 用の bridge ノードです。

**Subscribe する command topics**（mapoi の UI が publish する）:

| topic | 型 |
| --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String`（POI 名） |
| `mapoi/nav/route` | `std_msgs/String`（ルート名） |
| `mapoi/nav/cancel` | `std_msgs/String` |
| `mapoi/nav/pause` | `std_msgs/String` |
| `mapoi/nav/resume` | `std_msgs/String` |
| `mapoi/nav/switch_map` | `std_msgs/String`（map 名） |

> Localization 側の `mapoi/initialpose_poi`（`mapoi_interfaces/InitialPoseRequest`）受信と `/initialpose` 配信は #209 で `mapoi_amcl_localization_bridge` に分離されており、Navigation 仕様には含まれません。詳細は下の「Localization backend 仕様」節を参照してください。

**Publish する status topics**（mapoi の UI が subscribe する）:

| topic | 型 |
| --- | --- |
| `mapoi/nav/status` | `std_msgs/String`（`status` または `status:target`） |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus`（readiness summary、`transient_local`、minimal 3 フィールド） |

`mapoi_nav2_bridge` が使う `status` 値は `navigating` / `succeeded` / `aborted` / `canceled` / `paused` / `map_switching` / `map_switch_succeeded` / `map_switch_failed` / `backend_unavailable` / `rejected`（詳細は [`mapoi_server` の README](../mapoi_server/README.ja.md) の `mapoi/nav/status` 節）。**bridge 実装上の必須ルール**: goal / route / map switch コマンドを「受理したが実行しなかった」経路（無効な入力、内部 service 未 ready 等）では、必ず何らかの終端 status を publish してください。publish しないまま return すると、WebUI / RViz panel には直前の status（`succeeded` / `navigating` 等）が居座り続け、操作者が誤操作に気づけません（#339）。新規 status 値の追加は既存 subscriber に対して後方互換です。

**optional topic**: `mapoi_nav2_bridge` は上記の必須 status に加えて `mapoi/nav/command_rejected`（`std_msgs/String`、volatile QoS、payload は target のみ）を reject の都度必ず publish する（#354）。`mapoi/nav/status` が状態 snapshot のため走行中の reject を書けない制約の埋め合わせとして WebUI toast 通知に使うイベント軸で、custom bridge の必須実装ではない（実装しなくても `backend_ready` の contract には影響しない）。

`rejected` と `backend_unavailable` の使い分け: `backend_unavailable` は **Nav2 側**（action server / `/goal_pose` fallback subscriber）が不在で navigation そのものを実行できない場合に限定する。`mapoi/get_pois_info` / `mapoi/get_route_pois` service 未 ready・POI 名 typo・landmark POI 指定・空 route など、**mapoi_server 側** の内部 service 未 ready・入力検証で Nav2 の readiness とは無関係にコマンドを reject する経路は `rejected` を使う（goal / route とも service 呼び出し前に `wait_for_service(2s)` で readiness を確認し、未 ready なら `rejected` を publish する。#339 / #355）。運用上どちらも「今回のコマンドは実行されなかった」点は同じだが、`backend_unavailable` を Nav2 readiness 専用に保つことで、WebUI の `Navigation connected` バッジ (`mapoi/nav/backend_status`) との対応関係を崩さない。

`mapoi/nav/backend_status` の `backend_ready=true` を出した時のみ WebUI が `Navigation connected` 状態になり、ナビ操作 UI が enable されます。bridge は POI / route / map 情報を `mapoi_server` の service（`mapoi/get_pois_info` / `mapoi/get_route_pois` / `mapoi/select_map`）から取得し、自前の navigation API に変換します。各 topic / service の詳細は [`mapoi_server` の README](../mapoi_server/README.ja.md) を参照してください。

**`NavigationBackendStatus` 各フィールドの埋め方**（minimal 仕様）:

- `backend_type`: 自前 bridge を識別する短い文字列（例: `nav2`, `custom_lidar_planner`）。WebUI の tooltip に表示されるだけで挙動には影響しないが、複数 bridge が混在する環境で運用者が見分けられるように一意にする
- `backend_ready`: navigation 操作（goal / route / cancel / pause / resume / map switch）を **今この瞬間に** 受け付けられるかの真偽値。実装内部で複数 capability の readiness を AND 合成しても、必要な 1 機能だけ見ても可
- `reason`: `backend_ready=false` の時の人間可読な理由文字列（例: `"navigate_to_pose action not available"`）。空文字も許容するが、トラブルシュート支援のために理由を入れることを推奨。WebUI / panel は tooltip にプレーンテキストとして表示する想定で、HTML を解釈しません

bridge 実装者の必須実装は **`backend_ready` を真にする** ことだけです。per-capability の内訳が必要なら `reason` 文字列に詰めるか、bridge 固有の追加 topic で出してください。

**Localization は別仕様**: 自己位置推定（AMCL / SLAM toolbox / 自前 localization など）の readiness は本仕様では扱いません。`mapoi/initialpose_poi` の subscribe や `/initialpose` への配信は #209 で `mapoi_amcl_localization_bridge` に分離されており、独立した `LocalizationBackendStatus` 仕様（下節）として定義されています。独自 localization に切り替える場合は AMCL bridge を停止して、自作 bridge から `mapoi/localization/backend_status` を publish + `mapoi/initialpose_poi` を subscribe してください。

**運用上の注意**: WebUI / RViz panel は `backend_ready=true` を見て操作 UI を enable するため、operator は **`Navigation connected` バッジが点いてから操作する** ことを前提にしています。bridge 起動直後の数 100ms は action server が discovery 中でまだ ready でないことがあり、その間に強引に Run を押すと一度 `backend_unavailable` ステータスが返ることがあります（再度 Run を押せば正常進行）。バッジ表示は 1Hz 更新です。

## Localization backend 仕様

`mapoi_amcl_localization_bridge` は AMCL 互換 localization (`/initialpose` を `geometry_msgs/PoseWithCovarianceStamped` で受ける構成) 用の bridge ノードです。slam_toolbox / NDT / 自前 localization 等を mapoi の UI（WebUI / RViz panel）から扱いたい場合は、bridge ノードを自作して以下の topic 仕様を満たしてください。Navigation backend と独立した仕様として運用されるため、両方の bridge は同時に動かせます。

**Subscribe する command topics**（唯一の writer である `mapoi_server` が publish する。WebUI / RViz panel / `mapoi_nav2_bridge` からは `mapoi/request_initial_pose` service 経由で依頼する、#211）:

| topic | 型 |
| --- | --- |
| `mapoi/initialpose_poi` | `mapoi_interfaces/InitialPoseRequest`（`{map_name, poi_name}`、`transient_local`） |

**Publish する status topics**（mapoi の UI が subscribe する）:

| topic | 型 |
| --- | --- |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus`（readiness summary、`transient_local`、minimal 3 フィールド） |

`mapoi/localization/backend_status` の `backend_ready=true` を出した時のみ WebUI / RViz panel の Initial Pose 操作 UI が enable されます。bridge は POI 情報を `mapoi_server` の `mapoi/get_pois_info` service から取得し、`InitialPoseRequest.poi_name` を resolve して自前 localization に流します。

**`LocalizationBackendStatus` 各フィールドの埋め方**（minimal 仕様）:

- `backend_type`: 自前 bridge を識別する短い文字列（例: `amcl`, `slam_toolbox`, `custom_lidar_amcl`）。WebUI の tooltip に表示されるだけで挙動には影響しないが、複数 bridge が混在する環境で運用者が見分けられるように一意にする
- `backend_ready`: 初期位置を **今この瞬間に** 受け付け可能かの真偽値。bridge から見た downstream localization が listening 状態にあること（`mapoi_amcl_localization_bridge` は `/initialpose` の subscriber 数 > 0 を ready 条件として使用）
- `reason`: `backend_ready=false` の時の人間可読な理由文字列（例: `"no subscriber on /initialpose (localization node not running?)"`）。空文字も許容するが、トラブルシュート支援のために理由を入れることを推奨

bridge 実装者の必須実装は **`backend_ready` を真にする** ことだけです。Navigation 軸との独立性により、Nav2 を使わない localization 単体構成（POI editor + AMCL のみ）も成立します。

**Navigation backend との関係**: WebUI / RViz panel は **2 つのバッジを別 indicator として表示** します（`Navigation connected` / `Localization connected`）。Nav2 が落ちても AMCL は ready のまま、その逆もあり得ます。Operator map switch（`mapoi/nav/switch_map`）は Nav2 LoadMap → mapoi/initialpose_poi publish → localization bridge resolve のシーケンスで進みますが、UI の **map switch ボタンは navigation backend_ready のみで gate** しています（localization bridge 不在 / 一時不通でも localization bridge 側の subscriber 後起動 retry で initial pose 配信が吸収できるため）。確実に initial pose まで通したい場合は operator が両バッジを目視確認してから map switch を実行してください。
