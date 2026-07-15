#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>

#include <chrono>
#include <memory>

using namespace std::chrono_literals;

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  if (argc != 2) {
    RCLCPP_INFO(rclcpp::get_logger("mapoi_switch_map_client"),
      "usage: mapoi_switch_map_client turtlebot3_world");
    return 1;
  }

  auto node = rclcpp::Node::make_shared("mapoi_switch_map_client");
  auto pub = node->create_publisher<std_msgs::msg::String>("mapoi/nav/switch_map", 1);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (rclcpp::ok() && pub->get_subscription_count() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    rclcpp::spin_some(node);
    rclcpp::sleep_for(100ms);
  }

  if (pub->get_subscription_count() == 0) {
    RCLCPP_WARN(node->get_logger(),
      "mapoi/nav/switch_map has no subscribers; mapoi_nav2_bridge may not be running.");
  }

  std_msgs::msg::String msg;
  msg.data = argv[1];
  pub->publish(msg);
  RCLCPP_INFO(node->get_logger(), "Published map switch request: %s", msg.data.c_str());

  rclcpp::spin_some(node);
  rclcpp::sleep_for(100ms);
  rclcpp::shutdown();
  return 0;
}
