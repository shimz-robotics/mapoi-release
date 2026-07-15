# 自分のロボットへの導入方法

> English version (primary): [integration.md](./integration.md)
> 本ファイルは日本語スナップショットです。最新の内容は英語版を参照してください。

現状は本リポジトリを clone して colcon build で組み込んでください。**将来的に apt/rosdep での提供を予定しています**（[#20](https://github.com/shimz-robotics/mapoi/issues/20)）。

パッケージ・ノードの全体像は [docs/architecture.ja.md](./architecture.ja.md) を参照してください。

## 1. 地図ディレクトリの作成

パッケージ内に `maps/<地図名>/` ディレクトリを作成し、地図ファイルと設定ファイルを配置します。

```
maps/
├── site_a/
│   ├── mapoi_config.yaml    # POI・ルート・ユーザータグ設定
│   ├── map.pgm              # 地図画像
│   └── map.yaml             # 地図メタデータ
└── site_b/
    ├── mapoi_config.yaml
    ├── map.pgm
    └── map.yaml
```

`mapoi_config.yaml` のフォーマットについては [`mapoi_server` の README](../mapoi_server/README.ja.md) を参照してください。

## 2. launch ファイルへの追加

`your_robot_launch.yaml` に mapoi のノードを追加します。

```yaml
# mapoi_server: 地図・POI 情報の管理と mapoi service の提供
- node:
    pkg: mapoi_server
    exec: mapoi_server
    param:
      - {name: maps_path, value: "$(find-pkg-share your_package)/maps"}
      - {name: map_name, value: "site_a"}
      - {name: config_file, value: "mapoi_config.yaml"}

# mapoi_nav2_bridge: POI 名指定の goal を Nav2 に橋渡し (自律走行 + POI 半径イベント検知)
- node:
    pkg: mapoi_server
    exec: mapoi_nav2_bridge

# mapoi_amcl_localization_bridge: 初期位置を /initialpose 経由で AMCL に配信
- node:
    pkg: mapoi_server
    exec: mapoi_amcl_localization_bridge

# mapoi_rviz2_publisher: RViz2 向けに POI マーカーを配信
- node:
    pkg: mapoi_server
    exec: mapoi_rviz2_publisher

# Mapoi Web UI（オプション）: ブラウザから地図表示・POI 編集・ナビ操作
- node:
    pkg: mapoi_webui
    exec: mapoi_webui_node.py
    param:
      - {name: maps_path, value: "$(find-pkg-share your_package)/maps"}
      - {name: map_name, value: "site_a"}
      - {name: web_port, value: 8765}
```

CLI から `mapoi_bringup.launch.yaml` を直接起動する例 (引数 semantics 含む) は [`mapoi_server/README.ja.md`](../mapoi_server/README.ja.md) を参照してください。

## 3. AMCL パラメータの設定

初期位置は `mapoi_config.yaml` の **POI list 先頭の非 landmark POI** を default として `mapoi_amcl_localization_bridge` が `/initialpose` topic に自動配信します（#144 で旧 `initial_pose` system tag を廃止し、yaml 順序で表現する semantics に統一。#209 で `mapoi_nav2_bridge` から AMCL adapter を分離）。明示的に POI を指定したい場合は `mapoi/select_map` service の `initial_poi_name` 引数を使ってください。AMCL の `set_initial_pose` (Nav2 native の self-init) と二重管理にならないよう、AMCL 側はそれを無効化し、地図切り替えは `first_map_only_` を `False` にしてください。

> **Note (distro 差)**: Humble の AMCL ではパラメータ名が `first_map_only_` (末尾 underscore 付き)、Jazzy では `first_map_only` です。書き分けの実例は `mapoi_turtlebot3_example/param/humble/burger.yaml` と `mapoi_turtlebot3_example/param/jazzy/burger.yaml` を参照してください。

```yaml
amcl:
  ros__parameters:
    # 初期位置は mapoi_amcl_localization_bridge から /initialpose topic 経由で配信される
    # POI single source of truth 化のため AMCL native の self-init は使わない
    set_initial_pose: False
    # 地図切り替えの有効化 (Humble の表記。Jazzy ではパラメータ名は first_map_only)
    first_map_only_: False
```

`mapoi_config.yaml` 側は普通の POI 定義のままで OK (default initial pose に使う POI を yaml の **先頭** に置くだけ):

```yaml
poi:
  # POI 名は構造を示す汎用例 (turtlebot3 demo の実 POI 名とは独立)
  - name: entrance        # この POI が default 初期位置になる (POI list 先頭)
    pose: {x: -2.0, y: -0.5, yaw: 0.0}
    tags: [waypoint]
  - name: room_a
    pose: {x: 1.0, y: 0.5, yaw: 1.57}
    tags: [waypoint]
```

別 topic 名・別 message 型を使う localization パッケージへの対応については [`mapoi_server/README.ja.md`](../mapoi_server/README.ja.md#対応-localization-パッケージの要件) の「対応 localization パッケージの要件」節を参照してください。

## 4. POI 半径イベントの利用（オプション）

route 走行中に route 登録 POI に入ると `mapoi/events` トピックに `EVENT_ENTER` / `EVENT_PAUSED` (pause タグのみ) / `EVENT_EXIT` の 3 種別 event が配信されます。route 走行外 (IDLE / GOAL mode、`NavigateToPose` 単発 goal や手動操縦) では発火しません。

```sh
# イベントの確認
ros2 topic echo /mapoi/events
```

詳細仕様は [`mapoi_server` の README](../mapoi_server/README.ja.md#poi-半径イベント検知-poievent) の「POI 半径イベント検知」節を参照してください。
