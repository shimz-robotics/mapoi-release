// mapoi_nav2_bridge の責務分割 第 2 弾 (#345、最終弾): map switch 系 (switch_map
// subscribe → select_map 同期 → Nav2 LoadMap → initial pose request) のメソッド
// 定義を translation unit として分離する。
//
// これは TU 分割 (方式 b) であり、クラス構造・状態所有権は一切変更しない
// (MapoiNav2Bridge は単一クラスのまま、メソッド定義の物理配置のみを移す
// behavior-preserving refactor)。第 1 弾 (mapoi_nav2_bridge_route.cpp /
// mapoi_nav2_bridge_goal.cpp、PR #369) と同じ判断基準を踏襲している。
//
// mapoi_switch_map_cb は route/goal 両ドメインが共有する nav 状態 (current_goal_handle_ /
// current_ntp_goal_handle_ / nav_mode_ 等) を reset_nav_state() 経由で touch するが、
// reset_nav_state() 自体は GOAL / ROUTE-nav2 / ROUTE-mapoi 主導の 3 モードを 1 関数内で
// 扱う cross-domain dispatcher であり、node 側 (mapoi_nav2_bridge.cpp) に残置されている
// (第 1 弾 PR #369 と同じ理由: nav_mode_ の定義・遷移そのものを 1 ファイルに割ると
// 追いにくくなるため)。本 TU からは呼び出すだけに留め、定義は動かさない。
//
// mutex 境界の注記: この TU のメソッドは data_mutex_ を一切取らない。select_map /
// LoadMap / request_initial_pose はいずれも service 応答 callback 経由で、default の
// MutuallyExclusive callback group 内で他 callback (route/goal action callback や
// tolerance_check_callback) と直列化される契約に依存している (詳細は
// mapoi_nav2_bridge_route.cpp 冒頭コメント、および mapoi_nav2_bridge.cpp の
// tolerance_check_callback 冒頭コメント参照)。data_mutex_ が守るのは pois_list_ /
// event_pois_ / poi_inside_state_ / current_route_poi_names_ 等の限定された共有データ
// だけで、この TU にはそれらへのアクセスは無い。この TU だけを読んで「lock 漏れ」と
// 誤認して lock を追加しないこと。
#include "mapoi_server/mapoi_nav2_bridge.hpp"

#include <chrono>

using namespace std::chrono_literals;

void MapoiNav2Bridge::mapoi_switch_map_cb(const std_msgs::msg::String::SharedPtr msg)
{
  const std::string map_name = msg->data;
  if (map_name.empty()) {
    RCLCPP_ERROR(this->get_logger(), "mapoi/nav/switch_map received empty map name.");
    publish_nav_status("map_switch_failed", "empty_map_name");
    return;
  }
  RCLCPP_INFO(this->get_logger(), "Received map switch request: %s", map_name.c_str());

  if (current_goal_handle_) {
    action_client_->async_cancel_goal(current_goal_handle_);
    current_goal_handle_.reset();
  }
  if (current_ntp_goal_handle_) {
    nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    current_ntp_goal_handle_.reset();
  }
  reset_nav_state();
  ++nav_attempt_generation_;

  if (!this->select_map_client_->wait_for_service(2s)) {
    RCLCPP_ERROR(this->get_logger(), "mapoi/select_map service not available");
    publish_nav_status("map_switch_failed", map_name);
    return;
  }

  publish_nav_status("map_switching", map_name);
  auto request = std::make_shared<mapoi_interfaces::srv::SelectMap::Request>();
  request->map_name = map_name;
  select_map_client_->async_send_request(
    request,
    [this, map_name](rclcpp::Client<mapoi_interfaces::srv::SelectMap>::SharedFuture future) {
      this->on_select_map_received(map_name, future);
    });
}

void MapoiNav2Bridge::on_select_map_received(
  std::string map_name,
  rclcpp::Client<mapoi_interfaces::srv::SelectMap>::SharedFuture future)
{
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "select_map returned no response for '%s'.", map_name.c_str());
    publish_nav_status("map_switch_failed", map_name);
    return;
  }
  if (!result->success) {
    RCLCPP_ERROR(this->get_logger(),
      "select_map failed for '%s': %s", map_name.c_str(), result->error_message.c_str());
    publish_nav_status("map_switch_failed", map_name);
    return;
  }
  if (result->nav2_node_names.size() != result->nav2_map_urls.size()) {
    RCLCPP_ERROR(this->get_logger(),
      "select_map returned mismatched Nav2 map lists for '%s' (%zu node names, %zu URLs).",
      map_name.c_str(), result->nav2_node_names.size(), result->nav2_map_urls.size());
    publish_nav_status("map_switch_failed", map_name);
    return;
  }
  if (result->nav2_node_names.empty()) {
    RCLCPP_ERROR(this->get_logger(),
      "select_map returned no Nav2 map entries for '%s'. Operator map switch requires map entries.",
      map_name.c_str());
    publish_nav_status("map_switch_failed", map_name);
    return;
  }

  for (size_t i = 0; i < result->nav2_node_names.size(); ++i) {
    const auto & node_name = result->nav2_node_names[i];
    const auto & map_url = result->nav2_map_urls[i];
    if (!send_load_map_request(node_name, map_url)) {
      RCLCPP_ERROR(this->get_logger(), "Failed to load Nav2 map '%s' via '%s'.",
        map_url.c_str(), node_name.c_str());
      publish_nav_status("map_switch_failed", map_name);
      return;
    }
    RCLCPP_INFO(this->get_logger(), "Loaded Nav2 map '%s' via '%s'.",
      map_url.c_str(), node_name.c_str());
  }

  request_initial_pose(map_name, result->resolved_initial_poi_name);
  publish_nav_status("map_switch_succeeded", map_name);
  RCLCPP_INFO(this->get_logger(), "Map switch completed: %s", map_name.c_str());
}

bool MapoiNav2Bridge::send_load_map_request(const std::string & server_name, const std::string & map_file)
{
  auto node = rclcpp::Node::make_shared("mapoi_load_map_client");
  auto load_map_client = node->create_client<nav2_msgs::srv::LoadMap>(server_name + "/load_map");
  auto req = std::make_shared<nav2_msgs::srv::LoadMap::Request>();
  req->map_url = map_file;

  if (!load_map_client->wait_for_service(10s)) {
    RCLCPP_ERROR(this->get_logger(), "%s/load_map service is not available.", server_name.c_str());
    return false;
  }
  auto future = load_map_client->async_send_request(req);
  if (rclcpp::spin_until_future_complete(node, future, 1s) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(this->get_logger(), "%s/load_map did not return.", server_name.c_str());
    return false;
  }
  auto response = future.get();
  if (!response || response->result != nav2_msgs::srv::LoadMap::Response::RESULT_SUCCESS) {
    const uint8_t result_code = response ? response->result : 0;
    RCLCPP_ERROR(this->get_logger(),
      "%s/load_map rejected '%s' (result=%u).", server_name.c_str(), map_file.c_str(), result_code);
    return false;
  }
  return true;
}

void MapoiNav2Bridge::request_initial_pose(const std::string & map_name, const std::string & poi_name)
{
  // #211: 直接 publish せず、唯一の writer である mapoi_server に request_initial_pose service で
  // publish を依頼する。空 poi_name は従来どおり skip (clear は select_map/reload 経路で
  // mapoi_server が担うため、本 trigger では送らない)。
  if (poi_name.empty()) {
    RCLCPP_WARN(this->get_logger(),
      "No initial pose POI resolved for map '%s'; skipping request_initial_pose.",
      map_name.c_str());
    return;
  }
  // mapoi_server へは直前の select_map で応答を受け取った直後なので、通常 service は ready。
  // 未 ready の場合は publish できず initial pose が設定されないため、明示的に ERROR ログを出す
  // (今日の「subscriber 不在で latched されない」と同等の degrade、silent swallow はしない)。
  if (!request_initial_pose_client_->service_is_ready()) {
    RCLCPP_ERROR(this->get_logger(),
      "mapoi/request_initial_pose service not available; initial pose for map '%s' was NOT set "
      "(mapoi_server unreachable?).", map_name.c_str());
    return;
  }
  auto request = std::make_shared<mapoi_interfaces::srv::RequestInitialPose::Request>();
  request->map_name = map_name;
  request->poi_name = poi_name;
  // on_select_map_received (default callback group) 内から呼ばれるため、blocking せず async で
  // 投げて応答は callback でログするだけ。LoadMap は既に成功しているので publish 順序は保たれる。
  request_initial_pose_client_->async_send_request(
    request,
    [this, map_name, poi_name](
      rclcpp::Client<mapoi_interfaces::srv::RequestInitialPose>::SharedFuture future) {
      auto result = future.get();
      if (!result || !result->success) {
        RCLCPP_ERROR(this->get_logger(),
          "request_initial_pose failed for POI '%s' (map '%s').",
          poi_name.c_str(), map_name.c_str());
        return;
      }
      RCLCPP_INFO(this->get_logger(),
        "Requested initial pose POI '%s' for switched map '%s' (#211).",
        poi_name.c_str(), map_name.c_str());
    });
}
