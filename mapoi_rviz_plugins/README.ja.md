# mapoi_rviz_plugins

> English version (primary): [README.md](./README.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

mapoi 用の RViz2 プラグインパッケージです。GUI から地図切替・POI 選択・自律走行の操作、および POI の編集ができます。

## プラグイン

### MapoiPanel（パネルプラグイン）

RViz2 のパネルとして追加できるナビゲーション操作パネルです。

- 地図の切替をドロップダウンから選択
- 地図に応じた目的地（`waypoint` タグ付きの POI。`landmark` タグ併用の POI は除外）をリストから選択
- ルートをリストから選択して走行開始
- 選択した POI の位置に自己位置を修正（Initial Pose の設定）
- 選択した POI への自律走行を開始
- 自律走行の一時停止・再開・停止
- ナビゲーション状態の表示（`navigating`, `succeeded`, `aborted`, `canceled`, `paused`, `map_switching`, `map_switch_succeeded`, `map_switch_failed`, `backend_unavailable`, `rejected`。各値の意味は [`mapoi_server` README](../mapoi_server/README.ja.md) の `mapoi/nav/status` 項を参照）
- 選択中の POI/ルートを RViz2 上でハイライト表示

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/nav/goal_pose_poi` | `std_msgs/String` | ゴール POI 名の配信。実際の Nav2 action 起動は navigation bridge (`mapoi_nav2_bridge` ほか) が担当 |
| `mapoi/nav/pause` | `std_msgs/String` | ナビゲーションの一時停止 |
| `mapoi/nav/resume` | `std_msgs/String` | ナビゲーションの再開 |
| `mapoi/nav/cancel` | `std_msgs/String` | ナビゲーションのキャンセル |
| `mapoi/nav/route` | `std_msgs/String` | ルート走行の開始 |
| `mapoi/nav/switch_map` | `std_msgs/String` | 選択した地図への切替要求。実際の地図切替は navigation bridge が担当 |
| `mapoi/highlight/goal` | `std_msgs/String` | ゴールマーカーのハイライト |
| `mapoi/highlight/route` | `std_msgs/String` | ルートマーカーのハイライト |

#### サービスクライアント

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/get_maps_info` | `GetMapsInfo` | 地図ドロップダウン用の現在地図名・地図リストの取得 |
| `mapoi/get_pois_info` | `GetPoisInfo` | 目的地ドロップダウン用の POI リストの取得 |
| `mapoi/get_routes_info` | `GetRoutesInfo` | ルートドロップダウン用のルートリストの取得 |
| `mapoi/get_route_pois` | `GetRoutePois` | ハイライト表示用に選択ルートの POI を取得 |
| `mapoi/request_initial_pose` | `RequestInitialPose` | Initial Pose 設定時に `mapoi_server` へ publish を依頼 (#211)。panel は直接 `mapoi/initialpose_poi` を publish しない |

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/config_path` | `std_msgs/String` | 設定ファイルパスの変更検知 |
| `mapoi/nav/status` | `std_msgs/String` | ナビゲーション状態の表示（`"status"` / `"status:target"` 形式、transient_local QoS）。target が含まれていれば後起動 panel でも "Navigating: target" / "Arrived: target" 等を復元可能 |
| `mapoi/nav/backend_status` | `mapoi_interfaces/NavigationBackendStatus` | navigation backend の readiness と liveliness に応じて走行操作 UI（Run Goal / Run Route / Pause / Resume / Stop ボタンと地図ドロップダウン）を enable/disable (#198, #209)。QoS は msg contract (#208) に従う。[docs/backend-status.ja.md](../docs/backend-status.ja.md) 参照 |
| `mapoi/localization/backend_status` | `mapoi_interfaces/LocalizationBackendStatus` | localization backend の readiness と liveliness に応じて Initial Pose ボタンを enable/disable (#209) |

### PoiEditor（パネルプラグイン）

POI の情報を表形式で表示・編集・保存できるパネルです。

- 現在の設定ファイルから POI 情報を読み込み
- 編集対象の地図をドロップダウンから切替（`mapoi/select_map` で `mapoi_server` の編集 context を切替。稼働中ロボットの地図は切替えない）
- 表形式での POI 情報の確認・編集（name, pose, tolerance, tags, description の 5 column 構成、#158）
  - **pose** column は `x, y, yaw` 形式（例: `1.00, 2.00, 0.79`、x/y は m、yaw は rad、表示は小数点以下 2 桁）
  - **tolerance** column は `xy, yaw` 形式の 1 column 統合表記（例: `0.50, 0.79`、xy は m、yaw は rad、表示は小数点以下 2 桁）
    - validation 制約: `xy >= 0.001 m` / `0.001 rad <= yaw <= 2π rad`
    - max `2π rad` (= 360°) は、deg → rad 単位変更 (#158) 後の旧 deg 入力 (例: `45`)
      を誤って rad として入れたケースを弾くガード
  - **description** は末尾 column（長文 OK、横幅圧迫を回避）
- POI の追加・コピー・削除
- 編集の undo/redo（行の追加・コピー・削除、セル編集、行並べ替え）を Undo/Redo ボタンまたは `Ctrl+Z` / `Ctrl+Shift+Z` で実行可能 (#407, #435)
  - ボタンは focus の有無に関わらず動作する。キーボードショートカットはパネル内に focus がある時のみ有効（先に表のセルをクリックして focus を入れる）
  - 履歴は保存成功時・タグフィルタ変更時・表の全再構築時（地図切替や保存後の自動リロードなど）にクリアされる
- タグによるフィルタリング表示
- TagHelperComboBox によるタグ入力補助（システムタグは `[S]` 表記）
- バリデーション付き保存（名前重複・座標形式・tolerance.xy チェック、未定義タグの警告）
- MapoiPoseTool と連携した位置入力
- 保存後に mapoi_server の設定を自動リロード

#### サービスクライアント

| サービス名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi/select_map` | `SelectMap` | `mapoi_server` の編集 context を選択した地図へ切替（稼働中ロボットの Nav2 地図は切替えない） |
| `mapoi/get_maps_info` | `GetMapsInfo` | 現在地図名・地図リストの取得 |
| `mapoi/get_pois_info` | `GetPoisInfo` | 表に表示する POI リストの取得 |
| `mapoi/get_tag_definitions` | `GetTagDefinitions` | TagHelperComboBox と保存時 validation 用の system/user タグ定義の取得 |
| `mapoi/reload_map_info` | `std_srvs/Trigger` | 保存後に `mapoi_server` へ設定のリロードを依頼 |

#### サブスクライバー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi_rviz_pose` | `geometry_msgs/PoseStamped` | MapoiPoseTool からの位置入力 |
| `mapoi/config_path` | `std_msgs/String` | 設定ファイルパスの変更検知 |

### MapoiPoseTool（ツールプラグイン）

RViz2 のツールバーに追加できるポーズ指定ツールです。ショートカットキー: `i`

- RViz2 上でクリック＆ドラッグして位置・姿勢を指定
- PoiEditor の選択行に位置を反映
- 使用後は自動的にデフォルトツールに戻る
- POI の位置は `map` frame で保存されるため、RViz2 の Fixed Frame が `map` であることが前提です。
  異なる frame の場合は反映せず、警告ダイアログを表示します。

#### パブリッシャー

| トピック名 | 型 | 説明 |
| --- | --- | --- |
| `mapoi_rviz_pose` | `geometry_msgs/PoseStamped` | 指定した位置・姿勢の配信 |

## RViz2 への追加方法

1. RViz2 を起動
2. Panels > Add New Panel から `MapoiPanel` または `PoiEditor` を追加
3. ツールバーの「+」ボタンから `MapoiPoseTool` を追加

## 依存パッケージ

- `rviz_common`
- `rviz_default_plugins`
- `std_srvs`
- `mapoi_interfaces`
