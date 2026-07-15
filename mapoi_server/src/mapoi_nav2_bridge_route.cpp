// mapoi_nav2_bridge の責務分割 第 1 弾 (#345): route 走行 (FollowWaypoints +
// mapoi 主導 waypoint 到達モード) のメソッド定義を translation unit として分離する。
//
// これは TU 分割 (方式 b) であり、クラス構造・状態所有権は一切変更しない
// (MapoiNav2Bridge は単一クラスのまま、メソッド定義の物理配置のみを移す
// behavior-preserving refactor)。route 専用と言えそうな状態
// (current_route_waypoints_ / current_waypoint_index_ / current_route_pois_ /
// current_route_name_ / action_client_ / current_goal_handle_) も、
// current_ntp_goal_handle_ / nav_to_pose_client_ / paused_goal_pose_ /
// current_target_name_ / nav_mode_ を GOAL 走行 (mapoi 主導 waypoint 到達も
// NavigateToPose を共用) や tolerance_check_callback (goal/route 両モードの
// PoiEvent 判定エンジン、本 PR では触らない) と生の member アクセスで共有しており、
// component class 化すると lock 粒度・アクセス経路が変わるリスクがあるため
// 見送った。詳細は PR 本文参照。
//
// mutex 境界の注記 (PR #369 review medium): この TU の nav 状態更新
// (nav_mode_ / current_route_waypoints_ / current_waypoint_index_ 等) の多くは
// data_mutex_ を取らずに行われる。これは default の MutuallyExclusive callback
// group 内で subscription / action / timer callback が直列化される既存契約による
// 意図的なもの (MultiThreadedExecutor 2 threads だが、並行するのは独立 group の
// backend_status timer のみ)。data_mutex_ が守るのは pois_list_ / event_pois_ /
// poi_inside_state_ / current_route_poi_names_ 等の限定された共有データだけ
// (詳細は mapoi_nav2_bridge.cpp の tolerance_check_callback 冒頭コメント参照)。
// この TU だけを読んで「lock 漏れ」と誤認して lock を追加したり、逆に lock 下の
// 箇所を外したりしないこと。
#include "mapoi_server/mapoi_nav2_bridge.hpp"

#include <chrono>

using namespace std::chrono_literals;

void MapoiNav2Bridge::mapoi_route_cb(const std_msgs::msg::String::SharedPtr msg)
{
  RCLCPP_INFO(this->get_logger(), "Received route name: %s", msg->data.c_str());
  // #220 で RESUMED event は撤去 (resume は client request + status topic で観測可能)。
  // target は on_route_received → send_goal_options に lambda capture で bind し、
  // callback 側で goal 固有の target を使って publish_nav_status する (#104 race fix)。
  // current_target_name_ は acceptance 時 (goal_response_callback) に更新される。

  // goal 側 (`get_pois_info`) と対称の readiness check (#355)。service unreachable の
  // まま async_send_request すると応答待ちのまま status が更新されず、#339 と同型の
  // 「status が居座って操作者が気づけない」経路が route コマンドに残るため。
  if (!this->route_client_->wait_for_service(2s)) {
    RCLCPP_ERROR(this->get_logger(), "mapoi/get_route_pois service not available");
    publish_rejected_status(msg->data);
    return;
  }

  auto route_request = std::make_shared<mapoi_interfaces::srv::GetRoutePois::Request>();
  route_request->route_name = msg->data;

  // route_name を lambda capture で渡す。std::bind は rclcpp Jazzy の
  // function_traits::same_arguments テンプレート制約をパスせず compile error
  // になるので lambda を使う (PR #119 regression fix)。
  this->route_client_->async_send_request(
    route_request,
    [this, route_name = msg->data](
      rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedFuture future) {
        this->on_route_received(route_name, future);
    });
}

std::unordered_set<std::string> MapoiNav2Bridge::build_route_poi_names(
  const std::vector<mapoi_interfaces::msg::PointOfInterest> & waypoints,
  const std::vector<mapoi_interfaces::msg::PointOfInterest> & landmarks)
{
  std::unordered_set<std::string> result;
  for (const auto & p : waypoints) {
    result.insert(p.name);
  }
  for (const auto & p : landmarks) {
    result.insert(p.name);
  }
  return result;
}

void MapoiNav2Bridge::on_route_received(
  std::string route_name,
  rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedFuture future)
{
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "Failed to get Route info.");
    publish_rejected_status(route_name);
    return;
  }
  if (!result->success) {
    RCLCPP_ERROR(this->get_logger(),
      "get_route_pois failed for '%s': %s", route_name.c_str(), result->error_message.c_str());
    publish_rejected_status(route_name);
    return;
  }

  const auto & route_poi = result->pois_list;
  const auto & route_landmarks = result->landmark_pois;
  RCLCPP_INFO(this->get_logger(),
    "Received Route '%s' with %zu waypoints + %zu landmarks.",
    route_name.c_str(), route_poi.size(), route_landmarks.size());

  // Nav2 FollowWaypoints へ送るのは waypoints のみ。landmark は radius 監視専用 (#143)。
  std::vector<geometry_msgs::msg::PoseStamped> waypoints;
  for (size_t i = 0; i < route_poi.size(); ++i) {
    geometry_msgs::msg::PoseStamped wp;
    wp.header.frame_id = "map";
    wp.header.stamp = this->now();
    wp.pose = route_poi[i].pose;
    waypoints.push_back(wp);
  }

  if (waypoints.empty()) {
    RCLCPP_ERROR(this->get_logger(), "Route is empty. Cannot navigate.");
    publish_rejected_status(route_name);
    return;
  }

  // active route の POI 名 set を構築 (waypoints + landmarks 両方を tolerance_check で
  // pause 発火対象として扱う)。lock 下で書き込み (#143)。
  // 構築ロジックは pure helper に切り出し、unit test で検証する (#148)。
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    current_route_poi_names_ = build_route_poi_names(route_poi, route_landmarks);
  }

  current_route_waypoints_ = waypoints;
  current_waypoint_index_ = 0;
  nav_mode_ = NavMode::ROUTE;
  is_paused_ = false;

  // mapoi 主導モード (#243): waypoints を Nav2 に一括投入せず、mapoi が tolerance.xy 到達
  // 判定で 1 waypoint ずつ NavigateToPose を送って進める。route POI 列を保持し、先頭へ送る。
  if (waypoint_arrival_mode_ == "mapoi") {
    current_route_pois_ = route_poi;
    current_route_name_ = route_name;
    RCLCPP_INFO(this->get_logger(),
      "Route '%s' in mapoi-driven mode: %zu waypoints, advancing by tolerance.xy.",
      route_name.c_str(), current_route_pois_.size());
    send_current_waypoint_goal_();
    return;
  }

  // navigation attempt generation を増分し、callback の stale 判定で使う
  // (Codex review #147 round 1 + 2 high)。
  const size_t my_generation = ++nav_attempt_generation_;

  // Send waypoints to Nav2 with action client.
  // Nav2 が起動していない場合は無限待ちせず、backend_unavailable を publish して route を放棄する (#198)。
  // 旧実装の `while(!wait_for_action_server(1s))` ループおよび review fix の blocking timeout は
  // Nav2 不在時に single-thread executor で callback を blocking する原因だった。
  // 即時判定 + 1Hz backend_status polling で readiness は別途維持される (#198 review high)。
  if (!this->action_client_->action_server_is_ready()) {
    RCLCPP_ERROR(this->get_logger(),
      "FollowWaypoints action server not available; aborting route '%s'.", route_name.c_str());
    publish_nav_status("backend_unavailable", route_name);
    reset_nav_state();
    return;
  }
  RCLCPP_INFO(this->get_logger(), "Sending goal with %zu waypoints.", waypoints.size());

  auto goal_msg = FollowWaypoints::Goal();
  goal_msg.poses = waypoints;

  auto send_goal_options = rclcpp_action::Client<FollowWaypoints>::SendGoalOptions();

  // target を lambda capture で bind し、callback が goal 固有の target で
  // publish_nav_status を呼べるようにする (#104 Codex round 2 medium 対応)。
  send_goal_options.goal_response_callback =
    [this, target = route_name, my_generation](const GoalHandleFollowWaypoints::SharedPtr & h) {
      this->goal_response_callback(target, my_generation, h);
    };

  send_goal_options.feedback_callback =
    [this, my_generation](GoalHandleFollowWaypoints::SharedPtr h,
                          const std::shared_ptr<const FollowWaypoints::Feedback> f) {
      this->feedback_callback(my_generation, h, f);
    };

  send_goal_options.result_callback =
    [this, target = route_name, my_generation](const GoalHandleFollowWaypoints::WrappedResult & r) {
      this->result_callback(target, my_generation, r);
    };

  this->action_client_->async_send_goal(goal_msg, send_goal_options);
}

void MapoiNav2Bridge::send_current_waypoint_goal_()
{
  // route 終端: 全 waypoint を踏破したので route succeeded で終端処理する (#243)。
  if (current_waypoint_index_ >= current_route_pois_.size()) {
    const std::string route_name = current_route_name_;
    RCLCPP_INFO(this->get_logger(), "Route '%s' completed (mapoi-driven).", route_name.c_str());
    reset_nav_state();
    publish_nav_status("succeeded", route_name);
    return;
  }

  // NavigateToPose 不在なら route を放棄 (FollowWaypoints 経路と同じ #198 方針)。
  if (!this->nav_to_pose_client_->action_server_is_ready()) {
    RCLCPP_ERROR(this->get_logger(),
      "NavigateToPose action server not available; aborting mapoi-driven route '%s'.",
      current_route_name_.c_str());
    publish_nav_status("backend_unavailable", current_route_name_);
    reset_nav_state();
    return;
  }

  const auto & poi = current_route_pois_[current_waypoint_index_];
  geometry_msgs::msg::PoseStamped goal_pose;
  goal_pose.header.frame_id = "map";
  goal_pose.header.stamp = this->now();
  goal_pose.pose = poi.pose;
  // resume / pause 用に現 goal pose を保持 (GOAL モードと同じ用途)。
  paused_goal_pose_ = goal_pose;

  // 各 waypoint 送信は別 action attempt。generation を増分して旧 waypoint の遅延 callback
  // (cancel result 等) を stale 化する。target は route 名で統一 (status は route 単位)。
  const size_t my_generation = ++nav_attempt_generation_;
  const std::string route_name = current_route_name_;

  auto goal_msg = NavigateToPose::Goal();
  goal_msg.pose = goal_pose;

  auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
  send_goal_options.goal_response_callback =
    [this, target = route_name, my_generation](const GoalHandleNavigateToPose::SharedPtr & h) {
      this->ntp_goal_response_callback(target, my_generation, h);
    };
  send_goal_options.result_callback =
    [this, target = route_name, my_generation](const GoalHandleNavigateToPose::WrappedResult & r) {
      this->ntp_result_callback(target, my_generation, r);
    };

  this->nav_to_pose_client_->async_send_goal(goal_msg, send_goal_options);
  RCLCPP_INFO(this->get_logger(),
    "mapoi-driven route '%s': navigating to waypoint[%u] '%s' (tolerance.xy=%.2f).",
    route_name.c_str(), current_waypoint_index_, poi.name.c_str(), poi.tolerance.xy);
}

void MapoiNav2Bridge::on_waypoint_reached_()
{
  if (current_waypoint_index_ >= current_route_pois_.size()) {
    return;  // 防御: 既に終端
  }
  const auto & poi = current_route_pois_[current_waypoint_index_];

  // pause タグ waypoint に到達したら停止して resume を待つ (進めない)。auto-pause は
  // tolerance_check の ENTER 経路でも発火するが、Nav2 SUCCEEDED 経由 (ENTER 不発) でも
  // 確実に止めるためここでも idempotent に発火する (mapoi_pause_cb は is_paused_ で多重防御)。
  bool is_pause = false;
  for (const auto & tag : poi.tags) {
    if (tag == "pause") { is_pause = true; break; }
  }
  if (is_pause) {
    if (!is_paused_) {
      auto trigger = std::make_shared<std_msgs::msg::String>();
      trigger->data = "poi_event:" + poi.name;
      mapoi_pause_cb(trigger);
    }
    return;  // resume (mapoi_resume_cb) が次 waypoint へ進める
  }

  // 非 pause waypoint: 進行中の NavigateToPose goal を cancel (tolerance.xy ∧ yaw 到達トリガ時。
  // Nav2 SUCCEEDED トリガ時は ntp_result_callback で既に reset 済) して次 waypoint へ。
  // cancel に伴う CANCELED result が遅れて届く前に generation を進めて stale 化する (#265)。
  // 中間 waypoint は直後の send_current_waypoint_goal_ が generation を更に進めるので元々
  // stale 化されるが、最終 waypoint (= 末尾) では send 側が新 goal を送らず generation を
  // 進めないため、ここで進めないと CANCELED result が ntp_result_callback で "canceled" を
  // publish して route succeeded を上書きしてしまう。
  if (current_ntp_goal_handle_) {
    ++nav_attempt_generation_;
    nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    current_ntp_goal_handle_.reset();
  }
  ++current_waypoint_index_;
  send_current_waypoint_goal_();
}

void MapoiNav2Bridge::goal_response_callback(std::string target, size_t nav_generation,
                                              const GoalHandleFollowWaypoints::SharedPtr & goal_handle)
{
  // stale check (Codex review #147 round 2 high): 旧 route の goal_response が新 navigation
  // (route or GOAL) の受理後に届くと、current_goal_handle_ / current_target_name_ が旧
  // navigation に巻き戻り、pause / status publish が混乱する。bound generation と現在
  // generation を照合し、旧 navigation の callback は何もしない。
  if (nav_generation != nav_attempt_generation_) {
    RCLCPP_INFO(this->get_logger(),
      "Stale FollowWaypoints goal response for '%s' (gen=%zu, current=%zu); ignoring.",
      target.c_str(), nav_generation, nav_attempt_generation_);
    return;
  }
  if (!goal_handle) {
    RCLCPP_ERROR(this->get_logger(), "Goal was rejected by server");
  } else {
    current_goal_handle_ = goal_handle;
    // Acceptance 確定時に current_target_name_ を更新 (pause / resume が active
    // nav の target として参照する用途)。reject 時は更新しない。
    current_target_name_ = target;
    publish_nav_status("navigating", target);
    RCLCPP_INFO(this->get_logger(), "Goal accepted by server, waiting for result");
  }
}

void MapoiNav2Bridge::feedback_callback(
  size_t nav_generation,
  GoalHandleFollowWaypoints::SharedPtr,
  const std::shared_ptr<const FollowWaypoints::Feedback> feedback)
{
  // stale check (Codex review #147 round 3 medium): 旧 route の feedback が新 navigation
  // 受理後に届いた場合、current_waypoint_index_ を誤更新して次回 pause 時の paused_waypoints_
  // slice が壊れる risk があるため、bound generation と照合して stale なら無視する。
  if (nav_generation != nav_attempt_generation_) {
    return;
  }
  current_waypoint_index_ = feedback->current_waypoint;
}

void MapoiNav2Bridge::result_callback(std::string target, size_t nav_generation,
                                       const GoalHandleFollowWaypoints::WrappedResult & result)
{
  // stale check (Codex review #147 round 1 + 2 high): route A 実行中に別 navigation を
  // 開始し、A の result が新 nav 受理後に届くケースで、新 nav の current_route_poi_names_
  // / current_goal_handle_ を消さないよう、bound generation と現在 generation を照合する。
  if (nav_generation != nav_attempt_generation_) {
    RCLCPP_INFO(this->get_logger(),
      "Stale FollowWaypoints result for route '%s' (gen=%zu, current=%zu); ignoring.",
      target.c_str(), nav_generation, nav_attempt_generation_);
    return;
  }
  current_goal_handle_.reset();
  // bound target を使う (#104 race fix)。共有 current_target_name_ は別 nav の
  // 開始で上書きされる可能性があるため読まない。
  switch (result.code) {
    case rclcpp_action::ResultCode::SUCCEEDED:
      reset_nav_state();
      publish_nav_status("succeeded", target);
      RCLCPP_INFO(this->get_logger(), "Navigation SUCCEEDED!");
      // #220 で Nav2 SUCCEEDED hook 経由の event publish は撤去
      // (PAUSED は cmd_vel dwell only で trigger、STOPPED event 自体撤去)。
      break;
    case rclcpp_action::ResultCode::ABORTED:
      reset_nav_state();
      publish_nav_status("aborted", target);
      RCLCPP_ERROR(this->get_logger(), "Navigation ABORTED");
      break;
    case rclcpp_action::ResultCode::CANCELED:
      if (is_paused_) {
        RCLCPP_INFO(this->get_logger(), "Cancel confirmed (pause triggered). Waiting for resume.");
      } else {
        reset_nav_state();
        publish_nav_status("canceled", target);
        RCLCPP_WARN(this->get_logger(), "Navigation CANCELED");
      }
      break;
    default:
      RCLCPP_ERROR(this->get_logger(), "Unknown result code");
      break;
  }
}
