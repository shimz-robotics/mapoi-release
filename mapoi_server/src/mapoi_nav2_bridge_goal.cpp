// mapoi_nav2_bridge の責務分割 第 1 弾 (#345): goal 走行 (単発 Go / NavigateToPose) の
// メソッド定義を translation unit として分離する。
//
// これは TU 分割 (方式 b) であり、クラス構造・状態所有権は一切変更しない
// (MapoiNav2Bridge は単一クラスのまま、メソッド定義の物理配置のみを移す
// behavior-preserving refactor)。ntp_goal_response_callback / ntp_result_callback は
// NavigateToPose action の callback という点で GOAL 走行と対称だが、mapoi 主導 route
// (waypoint_arrival_mode_=="mapoi") も同じ action / 同じ current_ntp_goal_handle_ を
// 使って waypoint を進めるため、ntp_result_callback 内には route 側への分岐
// (on_waypoint_reached_() 呼び出し、mapoi_nav2_bridge_route.cpp 側で定義) が残る。
// この共有 (nav_to_pose_client_ / current_ntp_goal_handle_ / paused_goal_pose_ /
// current_target_name_ を GOAL と ROUTE-mapoi driven の両方が使う) が component
// class 化を見送った理由。詳細は PR 本文参照。
#include "mapoi_server/mapoi_nav2_bridge.hpp"

#include <chrono>

using namespace std::chrono_literals;

void MapoiNav2Bridge::mapoi_goal_pose_poi_cb(const std_msgs::msg::String::SharedPtr msg)
{
  std::string poi_name = msg->data;
  RCLCPP_INFO(this->get_logger(), "Received POI name for goal pose: %s", poi_name.c_str());
  // #220 で RESUMED event は撤去 (resume は client request + status topic で観測可能)。
  // poi_paused_published_ flag は ENTER → EXIT の lifecycle で reset される (EXIT 時 clear)。
  // target は send_goal_options に lambda capture で bind し、callback 側で
  // goal 固有の target を使って publish_nav_status する (#104 race fix)。
  // current_target_name_ は acceptance 時 (goal_response_callback) に更新され、
  // pause / resume 用の active target として参照される。

  // Fetch POI list asynchronously, then navigate in the callback
  if (!this->pois_info_client_->wait_for_service(2s)) {
    RCLCPP_ERROR(this->get_logger(), "mapoi/get_pois_info service not available");
    publish_rejected_status(poi_name);
    return;
  }
  auto request = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();
  pois_info_client_->async_send_request(
    request, [this, poi_name](rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future) {
      auto result = future.get();
      if (!result) {
        RCLCPP_ERROR(this->get_logger(), "Failed to get POI info for goal navigation.");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(data_mutex_);
        pois_list_ = result->pois_list;
      }
      rebuild_event_pois();

      for (const auto &poi : result->pois_list) {
        if (poi.name == poi_name) {
          // landmark POI は Nav2 navigation goal にできない reference 専用 (#85)。
          // goal+landmark 併用も同様に弾く。
          if (has_landmark_tag(poi)) {
            RCLCPP_ERROR(this->get_logger(),
              "POI '%s' has 'landmark' tag; cannot be set as Nav2 goal.",
              poi_name.c_str());
            publish_rejected_status(poi_name);
            return;
          }
          geometry_msgs::msg::PoseStamped goal_pose;
          goal_pose.header.frame_id = "map";
          goal_pose.header.stamp = this->now();
          goal_pose.pose = poi.pose;

          // GOAL 切替時の state 更新と generation 増分は action 利用可否に関わらず先に行う
          // (Codex review #147 round 2 / round 3 high): fallback (topic publish) でも nav_mode_
          // / generation を進めないと、ROUTE/GOAL A の遅延 callback が fallback GOAL B の state を
          // 上書きする経路が残る。
          paused_goal_pose_ = goal_pose;
          nav_mode_ = NavMode::GOAL;
          is_paused_ = false;
          // 直前まで ROUTE モードだった場合に備えて active route POI set を clear (#143)。
          // GOAL モードでは route POI 限定の pause 発火 logic を走らせない。
          {
            std::lock_guard<std::mutex> lock(data_mutex_);
            current_route_poi_names_.clear();
          }
          const size_t my_generation = ++nav_attempt_generation_;

          // Use NavigateToPose action client for result feedback.
          // Nav2 action 不在時は `goal_pose` topic への fallback を試みる (古い simple navigation 構成との互換性)。
          // ただし topic にも subscriber が居なければ backend_unavailable として WebUI / panel に通知する (#198)。
          // 旧実装の blocking `wait_for_action_server(timeout)` は single-thread executor で
          // cmd_vel / tolerance_check 等の他 callback を止めるため、即時判定に変更 (#198 review high)。
          // backend_status timer が 1Hz で readiness を更新しているので、別途 wait は不要。
          if (!this->nav_to_pose_client_->action_server_is_ready()) {
            if (nav2_goal_pose_pub_->get_subscription_count() == 0) {
              RCLCPP_ERROR(this->get_logger(),
                "Neither NavigateToPose action nor /goal_pose subscriber available; aborting GOAL '%s'.",
                poi_name.c_str());
              publish_nav_status("backend_unavailable", poi_name);
              reset_nav_state();
              return;
            }
            RCLCPP_WARN(this->get_logger(), "NavigateToPose action not available, falling back to topic");
            nav2_goal_pose_pub_->publish(goal_pose);
            return;
          }

          auto goal_msg = NavigateToPose::Goal();
          goal_msg.pose = goal_pose;

          auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
          // target + generation を lambda capture で bind し、callback が goal 固有 + stale 判定で
          // publish_nav_status / state 更新を呼べるようにする (#104 / #147)。
          send_goal_options.goal_response_callback =
            [this, target = poi_name, my_generation](const GoalHandleNavigateToPose::SharedPtr & h) {
              this->ntp_goal_response_callback(target, my_generation, h);
            };
          send_goal_options.result_callback =
            [this, target = poi_name, my_generation](const GoalHandleNavigateToPose::WrappedResult & r) {
              this->ntp_result_callback(target, my_generation, r);
            };

          this->nav_to_pose_client_->async_send_goal(goal_msg, send_goal_options);
          RCLCPP_INFO(this->get_logger(), "Sent NavigateToPose goal from POI: %s", poi_name.c_str());
          return;
        }
      }
      RCLCPP_WARN(this->get_logger(), "POI named '%s' not found!", poi_name.c_str());
      publish_rejected_status(poi_name);
    });
}

bool MapoiNav2Bridge::has_landmark_tag(const mapoi_interfaces::msg::PointOfInterest & poi)
{
  for (const auto & tag : poi.tags) {
    if (tag == "landmark") {
      return true;
    }
  }
  return false;
}

void MapoiNav2Bridge::on_goal_radius_arrival_()
{
  // GOAL モード (#261): 単発 Go が POI tolerance.xy + yaw に到達 (OR トリガ a)。進行中の
  // NavigateToPose goal を cancel して succeeded で完了する。cancel に伴う CANCELED result が
  // ntp_result_callback で "canceled" を publish して "succeeded" を上書きしないよう、先に
  // generation を進めて cancel result を stale 化する (同 callback group で直列化されるので
  // increment → cancel → (後で) stale result の順序が保証される)。
  const std::string target = current_target_name_;
  ++nav_attempt_generation_;
  if (current_ntp_goal_handle_) {
    nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    current_ntp_goal_handle_.reset();
  }
  reset_nav_state();
  publish_nav_status("succeeded", target);
  RCLCPP_INFO(this->get_logger(),
    "GOAL '%s' reached by tolerance.xy + yaw (mapoi-driven); completed.", target.c_str());
}

void MapoiNav2Bridge::ntp_goal_response_callback(std::string target, size_t nav_generation,
                                                  const GoalHandleNavigateToPose::SharedPtr & goal_handle)
{
  // stale check (Codex review #147 round 2 high): GOAL A の goal_response が新 navigation
  // 受理後に届く race を防ぐ。
  if (nav_generation != nav_attempt_generation_) {
    RCLCPP_INFO(this->get_logger(),
      "Stale NavigateToPose goal response for '%s' (gen=%zu, current=%zu); ignoring.",
      target.c_str(), nav_generation, nav_attempt_generation_);
    return;
  }
  if (!goal_handle) {
    RCLCPP_ERROR(this->get_logger(), "NavigateToPose goal was rejected by server");
  } else {
    current_ntp_goal_handle_ = goal_handle;
    // Acceptance 確定時に current_target_name_ を更新 (pause / resume 用)。
    current_target_name_ = target;
    publish_nav_status("navigating", target);
    RCLCPP_INFO(this->get_logger(), "NavigateToPose goal accepted, waiting for result");
  }
}

void MapoiNav2Bridge::ntp_result_callback(std::string target, size_t nav_generation,
                                            const GoalHandleNavigateToPose::WrappedResult & result)
{
  // stale check (Codex review #147 round 2 high): GOAL A の result が新 navigation 受理後に
  // 届くと、新 nav の current_route_poi_names_ / current_ntp_goal_handle_ が消える race を防ぐ。
  if (nav_generation != nav_attempt_generation_) {
    RCLCPP_INFO(this->get_logger(),
      "Stale NavigateToPose result for '%s' (gen=%zu, current=%zu); ignoring.",
      target.c_str(), nav_generation, nav_attempt_generation_);
    return;
  }
  current_ntp_goal_handle_.reset();
  // bound target を使う (#104 race fix)。
  switch (result.code) {
    case rclcpp_action::ResultCode::SUCCEEDED:
      if (nav_mode_ == NavMode::ROUTE && waypoint_arrival_mode_ == "mapoi") {
        // mapoi 主導 route (#243): Nav2 が現 waypoint に到達 (OR トリガ b)。次 waypoint へ
        // 進む。最終 goal の場合は on_waypoint_reached_ → send_current_waypoint_goal_ が
        // index 末尾検出で route succeeded を publish する。最終 goal だけは tolerance.xy
        // 進入では進めず、この SUCCEEDED (yaw 厳密着地込み) を待つ設計。
        RCLCPP_INFO(this->get_logger(),
          "NavigateToPose SUCCEEDED at route waypoint[%u] (mapoi-driven).",
          current_waypoint_index_);
        on_waypoint_reached_();
      } else {
        reset_nav_state();
        publish_nav_status("succeeded", target);
        RCLCPP_INFO(this->get_logger(), "NavigateToPose SUCCEEDED!");
        // #220 で Nav2 SUCCEEDED hook 経由の event publish は撤去。
      }
      break;
    case rclcpp_action::ResultCode::ABORTED:
      reset_nav_state();
      publish_nav_status("aborted", target);
      RCLCPP_ERROR(this->get_logger(), "NavigateToPose ABORTED");
      break;
    case rclcpp_action::ResultCode::CANCELED:
      if (is_paused_) {
        RCLCPP_INFO(this->get_logger(), "Cancel confirmed (pause triggered). Waiting for resume.");
      } else {
        reset_nav_state();
        publish_nav_status("canceled", target);
        RCLCPP_WARN(this->get_logger(), "NavigateToPose CANCELED");
      }
      break;
    default:
      RCLCPP_ERROR(this->get_logger(), "NavigateToPose unknown result code");
      break;
  }
}
