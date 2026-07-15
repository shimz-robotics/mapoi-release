// MapoiPanel の nav 操作系メソッド定義 (#397 step 4 で mapoi_panel.cpp から別 TU へ分離)。
// MapComboBox / Nav2GoalComboBox / MapoiRouteComboBox / LocalizationButton / RunGoalButton /
// RunRouteButton / PauseButton / ResumeButton / StopButton / PublishHighlightPois を収録。
// initial pose 拒否・route 取得失敗の UI 通知 (#401) 実装の直前分割として実施。
#include "mapoi_rviz_plugins/mapoi_panel.hpp"
#include "ui_mapoi_panel.h"

using namespace std::chrono_literals;

namespace mapoi_rviz_plugins
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("mapoi_rviz_plugins.mapoi_panel");

void MapoiPanel::MapComboBox()
{
  const int index = ui_->MapComboBox->currentIndex();
  if (index < 0 || index >= static_cast<int>(map_name_list_.size())) {
    return;
  }
  std_msgs::msg::String msg;
  msg.data = map_name_list_[index];
  if (mapoi_switch_map_pub_->get_subscription_count() == 0) {
    RCLCPP_WARN(LOGGER, "mapoi/nav/switch_map has no subscribers; mapoi_nav2_bridge may not be running.");
  }
  mapoi_switch_map_pub_->publish(msg);
  RCLCPP_INFO(LOGGER, "Published map switch request: %s", msg.data.c_str());
}

void MapoiPanel::Nav2GoalComboBox()
{
  goal_combobox_ind_ = ui_->Nav2GoalComboBox->currentIndex();
  if (goal_combobox_ind_ >= 0 && goal_combobox_ind_ < static_cast<int>(pois_.size())) {
    highlighted_goal_name_ = pois_[goal_combobox_ind_].name;
  } else {
    highlighted_goal_name_.clear();
  }
  PublishHighlightPois();
}

void MapoiPanel::MapoiRouteComboBox()
{
  route_combobox_ind_ = ui_->MapoiRouteComboBox->currentIndex();
  highlighted_route_poi_names_.clear();
  if (route_combobox_ind_ < 1) {
    PublishHighlightPois();
    return;
  }
  int route_index = route_combobox_ind_ - 1;
  if (route_index >= 0 && route_index < static_cast<int>(route_name_list_.size())) {
    if (get_route_pois_client_->wait_for_service(3s)) {
      auto request = std::make_shared<mapoi_interfaces::srv::GetRoutePois::Request>();
      request->route_name = route_name_list_[route_index];
      auto result = get_route_pois_client_->async_send_request(request);
      if (rclcpp::spin_until_future_complete(service_node_, result, 5s) == rclcpp::FutureReturnCode::SUCCESS) {
        auto response = result.get();
        if (response && response->success) {
          for (const auto & poi : response->pois_list) {
            highlighted_route_poi_names_.push_back(poi.name);
          }
        } else if (response) {
          // #342: route が見つからない (typo 等)。highlight は設定しない
          // (highlighted_route_poi_names_ は上で既に clear 済み)。
          RCLCPP_ERROR(LOGGER, "get_route_pois failed for '%s': %s",
                       route_name_list_[route_index].c_str(), response->error_message.c_str());
          // #401: route 取得失敗も UI へ。error_message はそのまま PlainText ラベルに載る
          // (空なら文言が宙に浮くためフォールバック、PR #423 review low)。
          const std::string err =
            response->error_message.empty() ? tr("(no details)").toStdString() : response->error_message;
          ShowTransientNotice(tr("Route fetch failed: ") + QString::fromStdString(err));
        } else {
          // response が null (future 完了だが応答本体なし)。LocalizationButton と同じ 3 分岐に
          // 揃え、無表示のまま取りこぼさない (PR #423 review)。
          RCLCPP_ERROR(LOGGER, "get_route_pois call failed for '%s' (null response).",
                       route_name_list_[route_index].c_str());
          ShowTransientNotice(tr("Route fetch failed (no response)"));
        }
      }
    } else {
      // #401: service 未起動もログのみだと無反応。UI にも一時通知する。
      RCLCPP_ERROR(LOGGER, "mapoi/get_route_pois service not available after 3s timeout.");
      ShowTransientNotice(tr("Route fetch service not connected"));
    }
  }
  PublishHighlightPois();
}

void MapoiPanel::LocalizationButton()
{
  // POI 選択ガード: GoalComboBox で選択されていない / 範囲外なら何もしない。
  // (LocalizationButton は GoalComboBox の POI を流用する仕様、cf. PublishHighlightPois)
  if (goal_combobox_ind_ < 0 || goal_combobox_ind_ >= static_cast<int>(pois_.size())) {
    RCLCPP_WARN(LOGGER, "No POI selected for initial pose; pick one in the goal ComboBox first.");
    return;
  }
  // #211: `/initialpose` も `mapoi/initialpose_poi` も直接叩かず、唯一の writer である mapoi_server に
  // request_initial_pose service 経由で publish を依頼する。これにより transient_local の
  // per-writer latched cache が単一化され stale POI を構造的に排除する。bridge が POI resolve /
  // landmark 排他 / `/initialpose` 配信 / retry を一元処理する点は不変 (#209)。
  if (!request_initial_pose_client_->wait_for_service(3s)) {
    // #401: service 未起動もログのみだと無反応で操作失敗に気づけない。UI にも一時通知する。
    RCLCPP_ERROR(LOGGER, "mapoi/request_initial_pose service not available after 3s timeout.");
    ShowTransientNotice(tr("Initial pose service not connected"));
    return;
  }
  auto request = std::make_shared<mapoi_interfaces::srv::RequestInitialPose::Request>();
  request->map_name = current_map_;
  request->poi_name = pois_[goal_combobox_ind_].name;
  auto result = request_initial_pose_client_->async_send_request(request);
  if (rclcpp::spin_until_future_complete(service_node_, result, 5s) == rclcpp::FutureReturnCode::SUCCESS) {
    // #299: server は非空 map_name が現在 map と不一致な要求を publish せず success=false で
    // 返すようになった (panel の current_map_ が stale な窓など)。future 完了 = 成功ではない
    // ので response を確認し、reject を operator に成功と誤認させない。
    // #421 review: null チェックは MapoiRouteComboBox と同じく `response && response->success` に統一。
    const auto response = result.get();
    if (response && response->success) {
      RCLCPP_INFO(LOGGER, "Requested initial pose: %s (map: %s).",
                  request->poi_name.c_str(), current_map_.c_str());
      // #401: 成功フィードバック。オペレータが初期位置設定の受理を確認できるようにする。
      // 成功は情報通知 (緑)。エラーの赤と区別して誤警戒を避ける。
      ShowTransientNotice(tr("Initial pose set: ") + QString::fromStdString(request->poi_name),
                          /*is_error=*/false);
    } else if (response) {
      RCLCPP_ERROR(LOGGER, "request_initial_pose rejected: %s",
                   response->error_message.c_str());
      // #401: 拒否も UI へ。error_message はそのまま PlainText ラベルに載る (#398 で rich text 無効化済み)。
      // 空 message は文言が宙に浮くためフォールバック (PR #423 review low)。
      const std::string err =
        response->error_message.empty() ? tr("(no details)").toStdString() : response->error_message;
      ShowTransientNotice(tr("Initial pose rejected: ") + QString::fromStdString(err));
    } else {
      // response が null (future 完了だが応答本体なし)。timeout/failed と同じ扱い。
      RCLCPP_ERROR(LOGGER, "request_initial_pose call failed or timed out.");
      ShowTransientNotice(tr("Initial pose failed (no response)"));
    }
  } else {
    // #401: 応答なし (timeout/失敗) もログのみだと無反応。UI にも通知する。
    RCLCPP_ERROR(LOGGER, "request_initial_pose call failed or timed out.");
    ShowTransientNotice(tr("Initial pose failed (no response)"));
  }
}

void MapoiPanel::RunGoalButton()
{
  if (goal_combobox_ind_ < 0 || goal_combobox_ind_ >= static_cast<int>(pois_.size())) {
    return;
  }
  // Nav2 native `goal_pose` を直接叩かず、`mapoi/nav/goal_pose_poi` 経由で bridge に POI 名を
  // 渡す (#209 review #3)。bridge が POI resolve → Nav2 action 起動 → `mapoi/nav/status` 配信を
  // 担当するので、status / cancel / pause / resume の状態管理が WebUI と一貫する。
  std_msgs::msg::String msg;
  msg.data = pois_[goal_combobox_ind_].name;
  mapoi_goal_pose_poi_pub_->publish(msg);

  current_nav_mode_ = "goal";
  current_nav_target_ = msg.data;
  SetNavStatus(
      tr("Navigating to POI: ") + QString::fromStdString(current_nav_target_));
  RCLCPP_INFO(LOGGER, "Published goal POI request: %s", current_nav_target_.c_str());
}

void MapoiPanel::RunRouteButton()
{
  if (route_combobox_ind_ < 1) {
    return;
  }
  int route_index = route_combobox_ind_ - 1;
  if (route_index >= static_cast<int>(route_name_list_.size())) {
    return;
  }
  std_msgs::msg::String msg;
  msg.data = route_name_list_[route_index];
  mapoi_route_pub_->publish(msg);

  current_nav_mode_ = "route";
  current_nav_target_ = route_name_list_[route_index];
  SetNavStatus(
      tr("Navigating route: ") + QString::fromStdString(current_nav_target_));
  RCLCPP_INFO(LOGGER, "A route was set: %s", msg.data.c_str());
}

void MapoiPanel::PauseButton()
{
  std_msgs::msg::String msg;
  msg.data = "mapoi_panel";
  mapoi_pause_pub_->publish(msg);
  SetNavStatus(tr("Paused"));
  RCLCPP_INFO(LOGGER, "Pause requested");
}

void MapoiPanel::ResumeButton()
{
  std_msgs::msg::String msg;
  msg.data = "mapoi_panel";
  mapoi_resume_pub_->publish(msg);
  SetNavStatus(tr("Resuming..."));
  RCLCPP_INFO(LOGGER, "Resume requested");
}

void MapoiPanel::StopButton()
{
  std_msgs::msg::String msg;
  msg.data = "mapoi_panel";
  mapoi_cancel_pub_->publish(msg);

  current_nav_mode_ = "idle";
  SetNavStatus(tr("Canceled"));
  RCLCPP_INFO(LOGGER, "The robot was stopped");
}

void MapoiPanel::PublishHighlightPois()
{
  if (mapoi_highlight_goal_pub_) {
    std_msgs::msg::String goal_msg;
    goal_msg.data = highlighted_goal_name_;
    mapoi_highlight_goal_pub_->publish(goal_msg);
  }

  if (mapoi_highlight_route_pub_) {
    std_msgs::msg::String route_msg;
    for (const auto & name : highlighted_route_poi_names_) {
      if (!route_msg.data.empty()) {
        route_msg.data += ",";
      }
      route_msg.data += name;
    }
    mapoi_highlight_route_pub_->publish(route_msg);
  }
}

}  // namespace mapoi_rviz_plugins
