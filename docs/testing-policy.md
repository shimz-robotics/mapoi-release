# テスト追加ポリシー

mapoi でテストを追加・削除する際の判断基準をまとめる。#193 (test 構成棚卸し) で実際に使った基準を明文化したもので、PR フロー等を含む contribution guide 全体は scope 外（将来 CONTRIBUTING.md を新設する場合はそちらに分離する）。

## 1. 判断基準: 致命性 × 実行コスト × 認知負荷

テストを追加するかどうかは、この 3 軸の総合判断で決める。「機能を追加したら反射的にテストファイルも 1 つ増やす」を既定にしない。

- **(a) 致命核**: 誤動作が走行安全やデータ破壊に直結する判定ロジック（例: 初期姿勢決定、地図切替時の初期化タイミング、システムタグ整合性）。unit test で厚く gate し、実装変更のたびに regression を確実に検知できることを優先する。判定の目安（1 つでも該当すれば致命核寄り）: ①誤動作がロボットの走行指令・停止判定に波及するか ②地図・POI 等の永続データを破壊しうるか ③失敗が UI 表示劣化にとどまらず運用者の誤判断（走行していないのに走行中と見える等）を誘発するか
- **(b) 安価な純関数**: 致命核でなくても、ROS/Qt 非依存の純関数で実行コストが極小（オーダーとして <0.01s）なものは、認知負荷（メンテコスト）が低いため追加してよい。例: #291 で再追加された `config_path_update_policy` の unit test（5 節参照）
- **(c) 重い / flaky / 冗長な層**: launch_test・e2e など実プロセス・タイミング依存のテストは、実行コスト（CI 時間）と認知負荷（flake 対応・mock 保守）が高い。追加は最小限にとどめ、既存ファイルへのケース追加や unit へ降ろせないかを先に検討する（3, 4 節）

新規機能 1 つごとに「安全側に倒してテストを足しておく」判断が積み重なると、個々は妥当でも総量が肥大し、メンテコストが機能追加のペースを上回る（6 節の背景を参照）。追加前にこの 3 軸で一度立ち止まる。

## 2. テスト層の定義と現状

mapoi には次の 5 層のテストがある（workflow はいずれも `.github/workflows/` 配下）。

| 層 | 対象 | 実行タイミング | 現状（目安、2026-07） |
| --- | --- | --- | --- |
| gtest unit | C++ 純関数・契約テスト（`mapoi_server`, `mapoi_rviz_plugins`） | PR gate（`ros-test.yml`） | 6 target |
| pytest unit | Python 純関数・API 単体（`mapoi_webui`, `mapoi_turtlebot3_example`） | PR gate（`ros-test.yml`） | 6 target |
| launch_test | `mapoi_server` の実プロセス統合テスト（最重量・flaky 層） | main push / 週次 schedule / 手動 dispatch のみ（PR gate 外） | 11 ファイル、timeout 60〜180s |
| vitest | WebUI JS の純関数・API モジュール | PR gate（`consistency-check.yml`） | 9 ファイル |
| Playwright e2e | WebUI ブラウザ smoke（jsdom 不可の Leaflet 直結部等） | PR gate（`consistency-check.yml`） | 9 ファイル |

launch_test が PR gate から外れているのは、実プロセス + `MultiThreadedExecutor` のタイミング依存で CPU 逼迫時に flake するため（`ros-test.yml` のコメント参照）。PR gate には乗らないが、main push / 週次では確実に実行され regression は追跡される。

gtest / pytest の target 数は `ros-test.yml` の `EXPECTED_TARGETS` が CMakeLists.txt 上の登録数（`ament_(auto_)add_gtest` / `ament_add_pytest_test`）から自動算出する（#351）。target 数の管理は自動化済みのため、この表の数字が古くなっても CI 側の検知は drift しない。表の数字はあくまで現状把握用の目安。launch_test / vitest / e2e の本数には同等の自動検知が無いため、これらの層でファイル数を増減させる PR ではこの表も更新すること。

## 3. launch_test 新規追加のハードル

launch_test は 1 本あたり timeout 60〜180s、実プロセス + mock executor のセットアップコストが高く、flaky 化のリスクも背負う。新規追加は次の優先順位で検討する。

1. **第一選択: 既存ファイルへのケース追加**。同じ mock/fixture 構成（`mapoi_server` + `mapoi_nav2_bridge` の起動、Nav2 mock の有無など）で検証できるなら、新しい `add_launch_test` ではなく既存ファイルにテストケースを足す
2. **新規ファイルは「新しい mock/fixture 構成が必要な時」のみ**。例: ROUTE mode 用の Nav2 `follow_waypoints` mock（`test_poi_event_route_integration.py`）と GOAL mode 用の `NavigateToPose` mock（`test_poi_event_goal_mapoi_arrival.py`）は fixture が異なるため分離されている
3. 新規ファイルを追加する場合、PR 説明に **「なぜ既存ファイルに足せないか」** を書く（fixture の差異、既存ファイルの肥大化など）

## 4. e2e（Playwright）も同様

Playwright e2e（`mapoi_webui/tests/web/e2e/`）も launch_test と同じ考え方を適用する。**「1 機能 = 1 新規ファイル」を既定にしない**。既存 spec ファイルへの `test()` ブロック追加で足りるかを先に検討し、新規ファイルは既存 spec と大きく異なるページ操作フロー・fixture が必要な時に限る。

## 5. 削除判断の記録

テストを「致命核外」として削除する場合、PR にその判断理由（致命性・実行コスト・認知負荷のどれを根拠にしたか）を残す。理由が残っていれば、後日同じテストを再追加したくなった時に、元の削除判断への反証を PR に書くだけで済み、削除と再追加が繰り返されること自体は問題にならない（根拠が両方 PR に残っていれば正常なフィードバックループ）。

**実例**: `test_config_path_update_policy.cpp`（`mapoi_rviz_plugins`）は #193 PR2（#287）で「致命核外」として削除されたが、翌日 #291 で「`ConfigPathCallback` は表示更新のみでナビ実行・データ整合に波及しない（致命核外）が、対象の判定ロジックは純関数・約 25 行・<0.01s・Qt/ROS 非依存で破格に安く、リファクタ時の regression を安く防げる」という理由を明記した上で再追加されている。削除判断そのものは誤りではなく、1 節 (b) の「安価な純関数」基準を後から適用した結果であり、PR 本文に両方の判断根拠が残っているため経緯を追跡できる。

## 6. 背景

#193 PR2（2026-06-03、#287）で「致命核外テストの削除」として -2,547 行の意図的な削減を行った。しかしその後 5 週間・20 commits でテスト関連ファイルは +2,892 / -78 行（net +2,814 行）と、削減量とほぼ同じだけ積み増しされ、削減効果が相殺された（#347）。launch_test も #193 直後の 3 本から 7 本（本ドキュメント作成時点では 10 本）へ再増加している。

内訳を見ると個々の追加は機能 PR に 1 対 1 対応しており無根拠な水増しではないが、「機能追加のたびに新規テストファイルを作る」運用が常態化していたことと、致命核の基準線が運用上あいまいだったことが揺れの原因だった（#347）。本ドキュメントは #193 で実際に使った判断基準を明文化し、テスト総量の再肥大と判断ブレを構造的に抑えることを目的とする。
