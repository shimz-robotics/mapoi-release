#include "mapoi_turtlebot3_example/navigate_to_pose_client.hpp"
#include "tf2/LinearMath/Quaternion.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.hpp"

NavigateToPoseClient::NavigateToPoseClient()
: Node("navigate_to_pose_client")
{
  client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");

  // Nav2 のアクションサーバー待ち
  if (!client_->wait_for_action_server(std::chrono::seconds(5))) {
    RCLCPP_ERROR(this->get_logger(), "navigate_to_pose action server not available!");
  }
}

void NavigateToPoseClient::send_goal(double x, double y, double yaw)
{
  NavigateToPose::Goal goal;
  goal.pose.header.frame_id = "map";
  goal.pose.header.stamp = now();

  // Yaw を quaternion に変換
  tf2::Quaternion q;
  q.setRPY(0, 0, yaw);

  goal.pose.pose.position.x = x;
  goal.pose.pose.position.y = y;
  goal.pose.pose.orientation = tf2::toMsg(q);

  RCLCPP_INFO(this->get_logger(),
              "Sending goal: x=%.2f y=%.2f yaw=%.2f", x, y, yaw);

  auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
  send_goal_options.result_callback =
      std::bind(&NavigateToPoseClient::result_callback, this, std::placeholders::_1);

  client_->async_send_goal(goal, send_goal_options);
}

void NavigateToPoseClient::result_callback(
    const GoalHandleNav::WrappedResult & result)
{
  switch (result.code) {
    case rclcpp_action::ResultCode::SUCCEEDED:
      RCLCPP_INFO(this->get_logger(), "Navigation succeeded!");
      break;
    case rclcpp_action::ResultCode::ABORTED:
      RCLCPP_ERROR(this->get_logger(), "Navigation aborted");
      break;
    case rclcpp_action::ResultCode::CANCELED:
      RCLCPP_WARN(this->get_logger(), "Navigation canceled");
      break;
    default:
      RCLCPP_ERROR(this->get_logger(), "Unknown result code");
      break;
  }
}


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);

  auto node = std::make_shared<NavigateToPoseClient>();

  // 例: x=1.0, y=2.0, yaw=90°（=1.57 rad）
  node->send_goal(-0.5, 0.5, 1.57);

  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
