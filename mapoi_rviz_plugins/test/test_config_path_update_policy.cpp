// config_path_update_policy.hpp の純粋判定 helper の unit test (#169)。
//
// PoiEditorPanel / MapoiPanel の ConfigPathCallback は ROS subscription で受けた
// config_path から map 名を取り出し、この helper の判定結果 (ReinitializeMap /
// RefreshCurrentMap / Noop) を UI 更新関数 (InitConfigs / UpdatePoiTable /
// SetMapComboBox 等) へ写像するだけ。判定そのものは Qt / ROS 非依存の純関数なので、
// 分岐表をここで pin し、リファクタで条件を踏み外す回帰 (`!=`↔`==` 反転、抑制対象の
// 取り違え等) を安く捕える。header は <string> と <filesystem> のみ依存 (#403 で
// mtime dedup の file_time_type が加わった)。
#include <gtest/gtest.h>

#include <chrono>

#include "mapoi_rviz_plugins/config_path_update_policy.hpp"

using mapoi_rviz_plugins::detail::ConfigPathUpdateAction;
using mapoi_rviz_plugins::detail::decide_poi_editor_config_path_action;
using mapoi_rviz_plugins::detail::apply_poi_editor_content_update_suppression;
using mapoi_rviz_plugins::detail::decide_mapoi_panel_config_path_action;
using mapoi_rviz_plugins::detail::apply_config_content_dedup;

// --- decide_poi_editor_config_path_action ---

TEST(DecidePoiEditorConfigPathAction, EmptyConfigPathReinitializesEvenForSameMap)
{
  // 初回受信 (config_path 未設定) は同一 map 名でも full 再初期化が要る。
  EXPECT_EQ(
    decide_poi_editor_config_path_action("mapA", "mapA", ""),
    ConfigPathUpdateAction::ReinitializeMap);
}

TEST(DecidePoiEditorConfigPathAction, DifferentMapReinitializes)
{
  EXPECT_EQ(
    decide_poi_editor_config_path_action("mapA", "mapB", "/maps/mapA/mapoi_config.yaml"),
    ConfigPathUpdateAction::ReinitializeMap);
}

TEST(DecidePoiEditorConfigPathAction, SameMapWithPathRefreshesContent)
{
  // 同一 map の内容変更 (save 後 reload による再 publish) は table 再 fetch のみで足りる。
  EXPECT_EQ(
    decide_poi_editor_config_path_action("mapA", "mapA", "/maps/mapA/mapoi_config.yaml"),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

// --- apply_poi_editor_content_update_suppression ---

TEST(ApplyPoiEditorContentUpdateSuppression, RefreshSuppressedBecomesNoop)
{
  // 自分自身の save 直後 (1.5s の SAVED! feedback 保護) は content refresh を抑制する。
  EXPECT_EQ(
    apply_poi_editor_content_update_suppression(
      ConfigPathUpdateAction::RefreshCurrentMap, true),
    ConfigPathUpdateAction::Noop);
}

TEST(ApplyPoiEditorContentUpdateSuppression, RefreshNotSuppressedPassesThrough)
{
  EXPECT_EQ(
    apply_poi_editor_content_update_suppression(
      ConfigPathUpdateAction::RefreshCurrentMap, false),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

TEST(ApplyPoiEditorContentUpdateSuppression, ReinitializeIsNeverSuppressed)
{
  // 抑制は RefreshCurrentMap だけに効く。map 切替の再初期化は suppress でも維持される。
  EXPECT_EQ(
    apply_poi_editor_content_update_suppression(
      ConfigPathUpdateAction::ReinitializeMap, true),
    ConfigPathUpdateAction::ReinitializeMap);
}

// --- decide_mapoi_panel_config_path_action ---

TEST(DecideMapoiPanelConfigPathAction, DifferentMapReinitializes)
{
  EXPECT_EQ(
    decide_mapoi_panel_config_path_action("mapA", "mapB"),
    ConfigPathUpdateAction::ReinitializeMap);
}

TEST(DecideMapoiPanelConfigPathAction, SameMapRefreshes)
{
  // MapoiPanel には Noop 抑制が無く、同一 map でも goal/route combo は再 fetch する。
  EXPECT_EQ(
    decide_mapoi_panel_config_path_action("mapA", "mapA"),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

// --- apply_config_content_dedup (#403) ---

namespace
{
// テスト用の file_time_type 定数を作るヘルパー。epoch + offset の duration 構築にする
// (clock::from_sys は C++20 API のため、humble の C++17 ビルドでは使えない)。
std::filesystem::file_time_type make_mtime(int offset_sec)
{
  return std::filesystem::file_time_type{} + std::chrono::seconds(offset_sec);
}
}  // namespace

TEST(ApplyConfigContentDedup, RefreshSamePathAndMtimeBecomesNoop)
{
  // path も mtime も前回と同一 → 内容変化なし → Noop に落とす。
  const auto t = make_mtime(1000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::RefreshCurrentMap,
      /*stat_ok=*/true,
      "/maps/mapA/mapoi_config.yaml", t,
      "/maps/mapA/mapoi_config.yaml", t),
    ConfigPathUpdateAction::Noop);
}

TEST(ApplyConfigContentDedup, RefreshDifferentMtimePassesThrough)
{
  // mtime が変わった = save 後の reload_map_info による内容変更 (#135 flow)。従来通り再取得。
  const auto t_old = make_mtime(1000);
  const auto t_new = make_mtime(2000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::RefreshCurrentMap,
      /*stat_ok=*/true,
      "/maps/mapA/mapoi_config.yaml", t_new,
      "/maps/mapA/mapoi_config.yaml", t_old),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

TEST(ApplyConfigContentDedup, RefreshDifferentPathPassesThrough)
{
  // path が変わった場合 (decide_* 側で ReinitializeMap になるはずだが、RefreshCurrentMap で
  // 来ても path 不一致なので素通しにする — dedup は同一 path 前提)。
  const auto t = make_mtime(1000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::RefreshCurrentMap,
      /*stat_ok=*/true,
      "/maps/mapA/mapoi_config.yaml", t,
      "/maps/mapB/mapoi_config.yaml", t),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

TEST(ApplyConfigContentDedup, RefreshStatFailurePassesThrough)
{
  // stat 失敗 (stat_ok=false) は dedup 不能として素通し — publisher と同じ方針。
  const auto t = make_mtime(1000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::RefreshCurrentMap,
      /*stat_ok=*/false,
      "/maps/mapA/mapoi_config.yaml", t,
      "/maps/mapA/mapoi_config.yaml", t),
    ConfigPathUpdateAction::RefreshCurrentMap);
}

TEST(ApplyConfigContentDedup, ReinitializeAlwaysPassesThrough)
{
  // ReinitializeMap は map 切替 = dedup 対象外。mtime が同一でも素通し。
  const auto t = make_mtime(1000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::ReinitializeMap,
      /*stat_ok=*/true,
      "/maps/mapA/mapoi_config.yaml", t,
      "/maps/mapA/mapoi_config.yaml", t),
    ConfigPathUpdateAction::ReinitializeMap);
}

TEST(ApplyConfigContentDedup, NoopInputRemainsNoop)
{
  // 上流 (suppression 等) が既に Noop にした場合はそのまま Noop を返す。
  const auto t = make_mtime(1000);
  EXPECT_EQ(
    apply_config_content_dedup(
      ConfigPathUpdateAction::Noop,
      /*stat_ok=*/true,
      "/maps/mapA/mapoi_config.yaml", t,
      "/maps/mapB/mapoi_config.yaml", t),  // path が違っても Noop は素通し
    ConfigPathUpdateAction::Noop);
}
