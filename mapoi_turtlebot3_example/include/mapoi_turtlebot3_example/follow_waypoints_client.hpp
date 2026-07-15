#pragma once

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <nav2_msgs/action/follow_waypoints.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <vector>

// アクション型のエイリアス
using FollowWaypoints = nav2_msgs::action::FollowWaypoints;
using GoalHandleFollowWaypoints = rclcpp_action::Client<FollowWaypoints>::GoalHandle;

// WaypointClientクラスの定義
class FollowWaypointsClient : public rclcpp::Node
{
public:
  // コンストラクタ
  explicit FollowWaypointsClient(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

  // 公開API: ウェイポイントリストを送信するための関数
  void send_waypoints(const std::vector<geometry_msgs::msg::PoseStamped>& waypoints);

private:
  rclcpp_action::Client<FollowWaypoints>::SharedPtr client_ptr_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::vector<geometry_msgs::msg::PoseStamped> waypoints_to_send_;
  
  // サーバーの準備が整うまで待機し、ウェイポイントを送信するトリガー関数
  void check_and_send_goal();

  // コールバック関数
  void goal_response_callback(const GoalHandleFollowWaypoints::SharedPtr & goal_handle);
  void feedback_callback(
    GoalHandleFollowWaypoints::SharedPtr,
    const std::shared_ptr<const FollowWaypoints::Feedback> feedback);
  void result_callback(const GoalHandleFollowWaypoints::WrappedResult & result);
};
