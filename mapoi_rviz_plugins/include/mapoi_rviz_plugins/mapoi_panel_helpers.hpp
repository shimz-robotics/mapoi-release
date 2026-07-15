#pragma once

#include <string>

namespace mapoi_rviz_plugins::detail
{

// バックエンド接続状態バッジの表示文字列を組み立てる純関数 (#400、PR #419 review で
// UpdateNavButtonsEnabled から切り出し)。表示規則 (Nav / Loc 共通):
//   - received == false → 空文字 (contract 未実装 publisher / 旧構成では UI 不変の後方互換)
//   - received && !alive → "<prefix>: Disconnected (bridge stopped)" — liveliness lost。保持中の
//     reason は死亡直前の stale 値のため表示しない (issue #400 の要件)
//   - alive && ready → "<prefix>: Connected"
//   - alive && !ready → "<prefix>: Not ready (<reason>)" (reason 空なら括弧ごと省略)
// Qt / ROS 非依存 (呼び出し側が QString へ変換して QLabel に載せる)。gtest で分岐表を pin する。
inline std::string build_backend_badge_text(
  const std::string & prefix, bool received, bool alive, bool ready,
  const std::string & reason)
{
  if (!received) {
    return "";
  }
  if (!alive) {
    return prefix + ": Disconnected (bridge stopped)";
  }
  if (ready) {
    return prefix + ": Connected";
  }
  if (!reason.empty()) {
    return prefix + ": Not ready (" + reason + ")";
  }
  return prefix + ": Not ready";
}

}  // namespace mapoi_rviz_plugins::detail
