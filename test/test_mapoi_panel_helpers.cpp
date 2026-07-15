// mapoi_panel_helpers.hpp の build_backend_badge_text (#400) の unit test。
// バッジ表示規則 (未受信 / 切断 / 接続 / 未準備±reason) と「切断時に stale reason を
// 表示しない」契約 (issue #400) をテーブル駆動で pin する。header は <string> のみ依存。
#include <gtest/gtest.h>

#include "mapoi_rviz_plugins/mapoi_panel_helpers.hpp"

using mapoi_rviz_plugins::detail::build_backend_badge_text;

TEST(BuildBackendBadgeText, NotReceivedIsEmpty)
{
  // 未受信 (contract 未実装 publisher / 旧構成) は空表示で UI 不変の後方互換。
  // 他の引数が何であっても received=false が最優先。
  EXPECT_EQ(build_backend_badge_text("Nav", false, true, true, "reason"), "");
}

TEST(BuildBackendBadgeText, LivelinessLostHidesStaleReason)
{
  // liveliness lost 時は保持中の reason (死亡直前の stale 値) を表示しない (issue #400)。
  EXPECT_EQ(
    build_backend_badge_text("Nav", true, false, true, "stale reason"),
    "Nav: Disconnected (bridge stopped)");
}

TEST(BuildBackendBadgeText, AliveAndReadyIsConnected)
{
  EXPECT_EQ(build_backend_badge_text("Loc", true, true, true, ""), "Loc: Connected");
}

TEST(BuildBackendBadgeText, NotReadyWithReasonShowsReason)
{
  EXPECT_EQ(
    build_backend_badge_text("Nav", true, true, false, "not ready: navigate_to_pose action"),
    "Nav: Not ready (not ready: navigate_to_pose action)");
}

TEST(BuildBackendBadgeText, NotReadyWithoutReasonOmitsParens)
{
  EXPECT_EQ(build_backend_badge_text("Loc", true, true, false, ""), "Loc: Not ready");
}
