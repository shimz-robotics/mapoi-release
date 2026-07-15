#pragma once

#include <memory>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "nav2_msgs/action/navigate_to_pose.hpp"

class NavigateToPoseClient : public rclcpp::Node {
public:
  using NavigateToPose = nav2_msgs::action::NavigateToPose;
  using GoalHandleNav = rclcpp_action::ClientGoalHandle<NavigateToPose>;

  NavigateToPoseClient();

  void send_goal(double x, double y, double yaw);

private:
  rclcpp_action::Client<NavigateToPose>::SharedPtr client_;

  void result_callback(const GoalHandleNav::WrappedResult & result);
};
