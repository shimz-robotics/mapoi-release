# mapoi_webui

> English version (primary): [README.md](./README.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

mapoi の Web UI パッケージです。
ブラウザから地図の表示、POI の編集、ルートの確認、ナビゲーション操作、ロボット位置の監視ができます。
スマートフォンやタブレットからも利用可能です。

## 機能

- **地図表示**: 占有格子地図をブラウザ上に表示、地図の切り替え、ズームはホイール操作・ドラッグ・ピンチのみ対応（+/− ボタンは無し）
- **POI 編集**: POI の追加・編集・削除・保存（mapoi_server に自動リロード通知）。地図上の POI はシングルクリックで選択、ダブルクリックで編集パネルを開く、位置ロック解除トグル（既定はロック）を切り替えると選択中の POI をドラッグで移動、扇形（yaw 制約）の先端ハンドルをドラッグで向き（yaw）を回転（いずれも Save で確定）。編集操作は Undo/Redo（Ctrl+Z / Ctrl+Shift+Z）に対応、モバイルではパネルを開かなくても地図上のフローティング Undo ボタンで直前の操作を取り消せる
- **ルート表示**: ルートのポリライン表示、矢印マーカー、表示/非表示の切り替え
- **ナビゲーション操作**: POI へのゴール走行、ルート走行、一時停止・再開、停止
- **スマホ表示**: Navigation を優先して開き、地図は画面の半分程度を確保
- **ナビゲーション backend 検出**: `mapoi/nav/backend_status` readiness topic (#198) 駆動で Navigation connected / unavailable バッジを UI に表示。command topic の subscriber 数による検出は、readiness topic 未受信時の capabilities payload (`/api/mode` および `/api/nav/status` の `navigation` フィールド) の best-effort フォールバックとしてのみ残る
- **自己位置推定 backend 検出**: `mapoi/localization/backend_status` readiness topic (#209) 駆動で Localization connected バッジを表示し、Set Initial Pose UI を gate する
- **自己位置推定リセット**: POI 選択による Initial Pose の設定
- **ロボット位置表示**: TF (`map` → `base_link`) によるリアルタイムのロボット位置マーカー表示
- **タグシステム**: システムタグ・ユーザータグの表示、タグによる POI 色分け
- **UI 表示設定**: UI 全体（ヘッダー・POI パネル）の表示/非表示トグル、POI パネルを地図上に重ねる半透明オーバーレイ表示（不透明度調整可、設定は保持）

## ノード

### mapoi_webui_node

Flask ベースの HTTP サーバーを内蔵した ROS2 ノードです。

#### パラメータ

| パラメータ名 | 型 | デフォルト | 説明 |
| --- | --- | --- | --- |
| `maps_path` | `string` | `""` | 地図ディレクトリのパス |
| `map_name` | `string` | `turtlebot3_world` | 起動時に読み込む地図名 |
| `config_file` | `string` | `mapoi_config.yaml` | 設定ファイル名 |
| `web_port` | `int` | `8765` | HTTP サーバーのポート |
| `web_host` | `string` | `0.0.0.0` | HTTP サーバーのバインドアドレス |
| `map_frame` | `string` | `map` | TF の親フレーム |
| `base_frame` | `string` | `base_link` | TF の子フレーム |
| `robot_radius` | `double` | `0.15` | ロボットの実寸 (m)。frontend の robot marker サイズ・active route connector の到達閾値に使う。Nav2 の `robot_radius` と意味は同じだが本 node は Nav2 非依存運用も想定するため独立 param とした。Nav2 を併用する場合は両者の値を一致させる (#117) |

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | ゴール POI 名の配信 |
| `mapoi/nav/route` | `std_msgs/String` | ルート走行の開始 |
| `mapoi/nav/pause` | `std_msgs/String` | ナビゲーションの一時停止 |
| `mapoi/nav/resume` | `std_msgs/String` | ナビゲーションの再開 |
| `mapoi/nav/cancel` | `std_msgs/String` | ナビゲーションのキャンセル |
| `mapoi/nav/switch_map` | `std_msgs/String` | Navigation map switch 指示。`mapoi_nav2_bridge` 等の navigation backend が受信して地図切り替えを実行 |

#### サービスクライアント

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/request_initial_pose` | `RequestInitialPose` | `POST /api/nav/initial-pose` 受信時に `mapoi_server` へ publish を依頼 (#211)。WebUI は直接 `mapoi/initialpose_poi` を publish しない |

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/nav/status` | `std_msgs/String` | ナビゲーション状態の受信（`"status"` / `"status:target"` 形式、transient_local QoS）。`:` split で target を抽出し REST `/api/nav/status` の `target` field として公開 |
| `mapoi/nav/command_rejected` | `std_msgs/String` | reject コマンドのイベント通知の受信（volatile QoS、payload = target 文字列、#354）。`mapoi/nav/status` の latched snapshot を汚さず、SSE `command_rejected` event として frontend に転送するだけの薄い変換 |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | `mapoi_nav2_bridge` が 1Hz で publish する navigation backend readiness summary の受信（#198）。Navigation connected バッジの表示と navigation 操作 UI の gate に使う。QoS は transient_local + liveliness（lease 5s、#208）で、publisher 死亡時は UI を disable する |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | `mapoi_amcl_localization_bridge`（または custom localization bridge）が 1Hz で publish する localization backend readiness summary の受信（#209）。Localization connected バッジの表示と Set Initial Pose UI の gate に使う。QoS は `mapoi/nav/backend_status` と同一 |
| `mapoi/config_path` | `std_msgs/String` | 外部からの地図切り替え検知 |

#### TF

| 変換 | 説明 |
| --- | --- |
| `map` → `base_link` | ロボット位置の取得（5Hz） |

## REST API

v1.0.0 に向けて URL 階層を editor 系 (`/api/editor/*`)・nav 系 (`/api/nav/*`) に分離しています (#340)。**「作用対象」列**は各 endpoint が触れる範囲を示します:

- **編集 context のみ**: `mapoi_server` 内部の編集対象 (map context / YAML) を変更するだけで、稼働中の実ロボット・Nav2 には一切作用しません
- **実ロボットに作用**: Nav2 / navigation backend (`mapoi_nav2_bridge` 等) に指示を送り、稼働中のロボットの挙動 (走行・地図切替・自己位置) を変えます
- **読み取りのみ**: 状態取得のみで副作用はありません

| メソッド | エンドポイント | 作用対象 | 説明 |
| --- | --- | --- | --- |
| GET | `/api/maps` | 読み取りのみ | 地図一覧と現在の地図名 |
| GET | `/api/maps/<name>/image` | 読み取りのみ | 地図画像（PNG） |
| GET | `/api/maps/<name>/metadata` | 読み取りのみ | 地図メタデータ（解像度・原点・サイズ） |
| POST | `/api/editor/select-map` | 編集 context のみ (実ロボット非接触) | `mapoi_server` の編集対象地図を切替 (`select_map` service)。Nav2 / 実ロボットの走行地図には作用しない。単独で叩くと server の編集 context と Nav2 の実 map が乖離しうる (乖離を解消するのは `/api/nav/switch-map`) |
| GET | `/api/pois` | 読み取りのみ | POI 一覧 |
| POST | `/api/pois` | 編集 context のみ | POI の保存 |
| GET | `/api/routes` | 読み取りのみ | ルート一覧 |
| POST | `/api/routes` | 編集 context のみ | ルートの保存 |
| GET | `/api/tag-definitions` | 読み取りのみ | タグ定義一覧 |
| POST | `/api/custom-tags` | 編集 context のみ | カスタムタグの保存 |
| GET | `/api/nav/status` | 読み取りのみ | ナビゲーション状態・ロボット位置・`robot_radius` (m) |
| POST | `/api/nav/goal` | 実ロボットに作用 | POI へのゴール走行 |
| POST | `/api/nav/route` | 実ロボットに作用 | ルート走行の開始 |
| POST | `/api/nav/pause` | 実ロボットに作用 | ナビゲーションの一時停止 |
| POST | `/api/nav/resume` | 実ロボットに作用 | ナビゲーションの再開 |
| POST | `/api/nav/cancel` | 実ロボットに作用 | ナビゲーションの停止 |
| POST | `/api/nav/initial-pose` | 実ロボットに作用 | 自己位置推定のリセット |
| POST | `/api/nav/switch-map` | 実ロボットに作用 | 稼働中ロボットの Nav2 map を実際に切替える。`mapoi/nav/switch_map` に map 名を publish |
| GET | `/api/mode` | 読み取りのみ | navigation 機能の検出結果 (`navigation_available`, topic subscriber 数) |

> **`/api/editor/select-map` と `/api/nav/switch-map` の違い (必読)**: 名前が紛らわしいですが挙動は全く異なります。`/api/editor/select-map` は WebUI の編集対象 (どの地図の POI/Route/CustomTags を編集するか) を切替えるだけで、Nav2 や実ロボットの走行地図には触れません。`/api/nav/switch-map` は稼働中ロボットの Nav2 map を実際に切替えます。`/api/editor/select-map` だけを叩いた場合、server の編集 context と Nav2 の実際の走行地図が乖離した状態になり得ます — 両方を一致させたい場合は呼び出し側が両方を叩く必要があります (#340)。

> **注意 (`POST /api/nav/*` 呼び出し側は必読)**: `mapoi/nav/*` 系 topic への publish はローカルで成功したかどうかしか分からず、navigation backend (`mapoi_nav2_bridge` 等) が実際にコマンドを受理・実行したかまでは保証できません。そのため `POST /api/nav/goal` `/route` `/pause` `/resume` `/cancel` `/initial-pose` `/switch-map` は subscriber 不在等の失敗時でも HTTP `200 OK` を返し、代わりに response body の `warning` フィールドに理由を入れます (詳細は下記「REST API と server 依存」参照)。**HTTP ステータスコードだけでは失敗を検知できません** — 外部 client は必ず response body に `warning` フィールドが含まれていないか確認してください。

### エラーレスポンス

4xx/5xx を返す endpoint は、人間可読な `error` フィールドに加えて機械可読な `code` フィールドを返します (#343)。`error` の文言は変更されることがあるため、client 側の分岐は `code` を使ってください（200 + `warning` はエラーではないため `code` を持ちません、上記の注意を参照）。

| `code` | HTTP status | 意味 |
| --- | --- | --- |
| `invalid_request` | 400 | request body 不正・必須フィールド欠如・値不正 |
| `not_found` | 404 | 指定した map / config (yaml) が存在しない |
| `version_mismatch` | 409 | 楽観的競合検出 (`expected_version` 不一致、下記参照) |
| `service_unavailable` | 503 | 依存する ROS 2 service が unavailable / timeout |
| `internal_error` | 500 | サーバ側の予期しない例外 (YAML 書き込み失敗等) |

### 楽観的競合検出 (`expected_version`)

`POST /api/pois` `/api/routes` `/api/custom-tags` はいずれも同じ yaml (`mapoi_config.yaml`) への書き込みのため、共通の楽観的競合検出に対応しています (`expected_version` フィールド、`/api/pois` は #241、`/api/routes` `/api/custom-tags` への展開は #343)。対応する GET (`/api/pois` `/api/routes` `/api/tag-definitions`) が返す `config_version` (yaml 内容の sha256 ハッシュ) を POST 時に `expected_version` として送り返すと、backend が現在の yaml と比較し、不一致なら `409` + `code: version_mismatch` を返します。WebUI 経由は frontend が自動でハンドリング（確認ダイアログの上でリロード）、外部 POST (curl / 別 client) で `expected_version` 省略時は check skip のため、競合上書きを避けたい場合は呼び出し側が対応する GET の `config_version` を送り返す責任を負う。

`config_version` は yaml ファイル全体の内容ハッシュのため、POI/Route/CustomTags のいずれかを保存すると、他 2 つの GET が返す `config_version` も変わります（同じ yaml を共有しているため意図した挙動です）。詳細仕様は実装コメント (`api_save_pois` / `api_save_routes` / `api_save_custom_tags`) / test (`test_api_save_pois_version_conflict.py`) を参照。

### SSE (`GET /api/events`)

frontend が `EventSource` で接続する server-sent events endpoint です（#135 (B)）。rviz / 外部 save 由来の config 変更や、走行中の reject など「polling では拾いにくい / status snapshot を汚さずに通知したいイベント」を push するために使います。各イベントは `{"type": "<event>", "payload": {...}}` の JSON（`payload` 省略可）。

| `type` | `payload` | 発火契機 |
| --- | --- | --- |
| `config_changed` | `{"map_name": "<name>"}` | `mapoi/config_path` 受信（rviz / 外部 save 由来の POI・Route・CustomTags 変更、または外部 map 切替）。frontend は 200ms debounce して `loadPois` / `loadRoutes` / `loadTagDefinitions` を再実行する |
| `command_rejected` | `{"target": "<文字列>"}` | `mapoi/nav/command_rejected` 受信（#354）。走行中の typo goal 等、`mapoi/nav/status` を汚さずに reject を伝えるイベント通知。frontend は一時的な toast（`Command rejected: <target>`、約 5 秒で自動消滅）を表示する |

接続は 30 秒周期の heartbeat コメント (`:heartbeat`) を挟み、client 切断はサーバ側で自動 discard されます（詳細は `_broadcast_sse_event` / `/api/events` の実装コメント参照）。

## 起動方法 (3 つのシナリオ)

利用目的に応じて 3 つの起動方法を使い分けてください。いずれもブラウザで `http://<ホスト>:8765` にアクセスします。

### A. webui のみ起動 (オペレーター用、`mapoi_server` を別プロセスで起動済み)

ロボットが既に動いている (`mapoi_server` / `mapoi_nav2_bridge` 等が別プロセスで稼働) 状態で、RViz の代替 UI として webui を後付けする用途。

```bash
ros2 launch mapoi_webui mapoi_webui.launch.yaml maps_path:=/path/to/maps map_name:=your_map
```

### B. webui + mapoi_server を一緒に起動 (エディター用)

PC でデスクトップ上の POI/Route/CustomTags 編集だけ行いたい用途 (ロボット動作 / Nav2 / RViz は不要)。`mapoi_bringup` を minimal config (`with_nav2_bridge=false`, `with_rviz_publisher=false`, `simulator=none`) で include する。

```bash
ros2 launch mapoi_webui mapoi_editor.launch.yaml maps_path:=/path/to/maps map_name:=your_map
```

### C. 統合運用 / デモ (Nav2 + sim + webui を全部)

`mapoi_turtlebot3_example` の `turtlebot3_navigation.launch.yaml` のような bringup + webui 統合 launch を使う。詳細はそのパッケージの README を参照。

## maps_path 未設定時の縮退

`maps_path` パラメータは `""` (未設定) でも `mapoi_webui_node` は起動します。`mapoi_server` が `maps_path` 空で FATAL ログを出して即 throw する (起動しない) のとは非対称ですが、これは意図的な設計です — webui は「ロボットが既に稼働中の環境に、RViz の代替としてオペレーター用 nav UI を後付けする」用途 (起動方法 A) が正当に存在し、その用途では地図ディレクトリへのアクセスを必要としないため、fail-fast にはしていません。

`maps_path` 未設定時の挙動は以下の通りです:

- 起動時に `maps_path parameter not set. Set it to use the web editor.` を `WARN` ログ出力し、そのまま起動を継続する
- **editor 機能 (地図一覧・地図画像・POI/Route/CustomTags の閲覧・編集) は無効になる**: `get_maps_list()` は `maps_path` が空 (または存在しないディレクトリ) の場合に空リストを返すため、`GET /api/maps` は `maps: []` を返し、`GET /api/maps/<name>/image` `/metadata` や POI/Route/CustomTags 系の endpoint (`GET`/`POST /api/pois` `/api/routes` `/api/custom-tags`) は対象 YAML/PNG が見つからず `404 Not Found` を返す
- **nav 操作系 (`/api/nav/*`, `/api/mode`, `GET /api/nav/status`) は `maps_path` に依存せず動作する**: これらは `mapoi/nav/*` topic の publish / subscribe や `mapoi_server` の service 呼び出しのみで完結するため、`maps_path` 未設定でも通常通り使える

したがって、`maps_path` を設定せずに起動した webui は「ナビゲーション操作専用の後付け UI」として機能し、地図編集機能だけが無効になります。

## REST API と server 依存

各 endpoint が `mapoi_server` (or `mapoi_nav2_bridge`) の起動を必要とするかの早見表:

| endpoint | server 必要 | 理由 |
|---|---|---|
| `GET /api/maps` `/maps/<name>/image\|metadata` | 不要 | `maps_path` を直 ls / YAML/PNG 直読み |
| `GET /api/pois` `/api/routes` | 不要 | YAML 直読み |
| `POST /api/editor/select-map` | **必要 (`mapoi_server` の `select_map` service)** | 編集対象地図の切替のみ。Nav2 / 実ロボットには作用しない |
| `POST /api/pois` `/api/routes` `/api/custom-tags` | **必要** | YAML 書き込み後に `mapoi/reload_map_info` service を呼ぶ |
| `GET /api/tag-definitions` | **必要** | `mapoi/get_tag_definitions` service 経由 |
| `POST /api/nav/{goal,route,cancel,pause,resume}` | **必要 (mapoi_nav2_bridge)** | publisher → mapoi_nav2_bridge が listener、subscriber 0 件なら warning を返す |
| `POST /api/nav/initial-pose` | **必要 (`mapoi_server` の `mapoi/request_initial_pose` service)** | `mapoi/request_initial_pose` service 経由で `mapoi_server` が `mapoi/initialpose_poi` を publish (#211)。service 不在時は 503。さらに `mapoi/initialpose_poi` に subscriber (`mapoi_amcl_localization_bridge` 等) が居ないと initial pose は配信されず warning を返す |
| `POST /api/nav/switch-map` | **必要 (mapoi_nav2_bridge 等)** | 稼働中ロボットの Nav2 map を実際に切替える。`mapoi/nav/switch_map` publisher → navigation backend が listener、subscriber 0 件なら warning を返す |
| `GET /api/nav/status` | 不要 (subscriber 経由で受信値返却) | mapoi_nav2_bridge がいなければ default 値 |
| `GET /api/mode` | 不要 | command topic の subscriber 数から best-effort で検出 |

server / mapoi_nav2_bridge 不在時の挙動 (endpoint カテゴリ別):
- `POST /api/editor/select-map`: `503 Service Unavailable` (service 必須、unavailable / timeout 時)
- `GET /api/tag-definitions`: `503 Service Unavailable` (service 必須、unavailable / timeout 時)
- `POST /api/pois` / `/api/routes` / `/api/custom-tags`: `200 OK` + `warning` フィールド (YAML 書き込み自体は成功するため、`mapoi/reload_map_info` の unavailable / timeout / failure は warning として通知)
- `POST /api/nav/{goal,route,cancel,pause,resume}` / `/api/nav/initial-pose` / `/api/nav/switch-map`: `200 OK` + `warning` フィールド (publish 自体は成功扱い、subscriber 不在を best-effort で検出して通知)

## 開発者規約

### Flask thread から ROS 2 service を呼ぶ場合

**必ず `MapoiWebNode._call_service_sync()` 経由で呼ぶ**。直接 `client.call_async()` を fire-and-forget したり、`spin_until_future_complete()` を Flask thread から呼ぶのは禁止 (前者は silent failure、後者は main thread の `rclpy.spin` と executor 競合)。

```python
response = node._call_service_sync(
    node.some_client_, SomeReq(), 'some_service', timeout_sec=3.0)
if response is None:
    return jsonify({'error': 'service unavailable'}), 503
```

### Flask thread から ROS 2 publish する場合

**必ず `MapoiWebNode.publish_with_subscriber_check()` 経由で publish する**。subscriber 0 件 (例: `mapoi_nav2_bridge` 未起動) の silent failure を避けるため。

```python
_, warning = node.publish_with_subscriber_check(
    node.some_pub_, msg, 'some_topic')
return jsonify({'success': True, 'warning': warning} if warning else {'success': True})
```

## 依存パッケージ

- `rclpy`
- `std_msgs`, `std_srvs`
- `mapoi_interfaces`
- `tf2_ros`
- `python3-flask`
- `python3-pil`
- `python3-yaml`
