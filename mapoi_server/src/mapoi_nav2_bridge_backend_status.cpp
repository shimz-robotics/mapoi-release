// mapoi_nav2_bridge の責務分割 第 2 弾 (#345、最終弾): backend status publish
// (`publish_backend_status`、1Hz timer から呼ばれる) のメソッド定義を translation
// unit として分離する。
//
// これは TU 分割 (方式 b) であり、クラス構造・状態所有権は一切変更しない
// (MapoiNav2Bridge は単一クラスのまま、メソッド定義の物理配置のみを移す
// behavior-preserving refactor)。第 1 弾 (PR #369) / 本弾の map switch TU
// (mapoi_nav2_bridge_map_switch.cpp) と同じ TU 分割方針。
//
// 独立 callback group についての注記 (事故防止、必読): `publish_backend_status` は
// このリポジトリの他の nav 系メソッド (mapoi_nav2_bridge.cpp /
// mapoi_nav2_bridge_route.cpp / mapoi_nav2_bridge_goal.cpp /
// mapoi_nav2_bridge_map_switch.cpp 内の subscription・action・timer callback) とは
// 異なり、constructor (mapoi_nav2_bridge.cpp) で生成される独立 MutuallyExclusive
// callback_group (`backend_status_callback_group_`) に紐付く 1Hz timer から呼ばれ、
// MultiThreadedExecutor (2 threads) 上で default callback group の callback と
// **並行実行され得る**。他ファイルの nav 状態変更メソッドが前提とする「default
// callback group による直列化のため lock なしで nav state を読み書きできる」という
// 安全根拠は、この TU の `publish_backend_status` には適用されない。
//
// `publish_backend_status` が nav state member を一切書き込まず、read-only な
// `*_is_ready()` 判定 + thread-safe な `Publisher::publish()` のみで完結しているのは
// この並行実行を安全にするための意図的な設計であり (詳細は mapoi_nav2_bridge.cpp の
// constructor コメント #213/#214 参照)、このメソッドを将来拡張して nav_mode_ 等の
// state に触れる処理を足す場合は、この「他 callback と並行実行される」前提を崩さない
// よう注意すること (mutex 保護の追加、または default callback group への付け替えを
// 検討する)。
#include "mapoi_server/mapoi_nav2_bridge.hpp"

void MapoiNav2Bridge::publish_backend_status()
{
  // Nav2 bridge としての readiness を 1Hz polling で集約して publish する (#198)。
  // contract は minimal 3 フィールドだけ (#205 review): bridge 実装者は backend_ready を
  // 真にするだけで mapoi UI と統合できる。Per-capability の内訳が必要なら reason 文字列に
  // 詰める。Localization readiness は別軸 (#209) で、本 msg では扱わない。
  // 二重管理に見える点 (1Hz timer の集約 + 各 cb 内 `action_server_is_ready()` 即時判定) は
  // 意図的: cb 内で 1Hz timer の cache を読むと最大 1 秒の lag が発生し、operator が ready
  // 表示直後に Run を押した場合に偽の backend_unavailable を出しかねない。即時判定で current
  // を見る (#205 review low #2)。
  // backend_ready の AND に `select_map` service を含めるのは README contract が「map switch
  // を含む」と書いていることと整合させるため (#205 round 3 review high)。bridge 単独起動で
  // mapoi_server が居ない構成では `backend_ready=false` になるが、これは妥当な挙動。
  // この AND は Nav2 bridge が `goal` / `route` / `switch_map` の 3 capability 全部を expose
  // する前提に閉じた算出 (#207)。custom bridge は自前で expose する capability だけを AND
  // すること (例: goal-only bridge なら `backend_ready = goal_ready`)。詳細は
  // `mapoi_interfaces/msg/NavigationBackendStatus.msg` 冒頭コメント参照。
  const bool goal_ready =
    nav_to_pose_client_ && nav_to_pose_client_->action_server_is_ready();
  const bool route_ready =
    action_client_ && action_client_->action_server_is_ready();
  const bool switch_map_ready =
    select_map_client_ && select_map_client_->service_is_ready();

  mapoi_interfaces::msg::NavigationBackendStatus msg;
  msg.backend_type = "nav2";
  msg.backend_ready = goal_ready && route_ready && switch_map_ready;
  if (!msg.backend_ready) {
    std::vector<std::string> missing;
    if (!goal_ready) {
      missing.emplace_back("navigate_to_pose action");
    }
    if (!route_ready) {
      missing.emplace_back("follow_waypoints action");
    }
    if (!switch_map_ready) {
      missing.emplace_back("mapoi/select_map service");
    }
    std::string joined;
    for (size_t i = 0; i < missing.size(); ++i) {
      if (i > 0) {
        joined += ", ";
      }
      joined += missing[i];
    }
    msg.reason = "not ready: " + joined;
  }
  backend_status_pub_->publish(msg);
}
