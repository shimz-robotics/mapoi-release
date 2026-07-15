#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>

#include "mapoi_interfaces/srv/get_pois_info.hpp"

using namespace std::chrono_literals;

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);

  std::shared_ptr<rclcpp::Node> node = rclcpp::Node::make_shared("get_pois_info_client");
  rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr client =
    node->create_client<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info");

  auto request = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();

  while(!client->wait_for_service(1s)){
    if(!rclcpp::ok()){
      RCLCPP_ERROR(rclcpp::get_logger("get_pois_info_client"), "Interrupted while waiting for the service. Exiting.");
      return 0;
    }
    RCLCPP_INFO(rclcpp::get_logger("get_pois_info_client"), "service not available, waiting again...");
  }

  auto result = client->async_send_request(request);
  // Wait for the result.
  if(rclcpp::spin_until_future_complete(node, result) == rclcpp::FutureReturnCode::SUCCESS)
  {
    auto pois = result.get()->pois_list;
    RCLCPP_INFO(rclcpp::get_logger("get_pois_info_client"), "Size of result: %ld", pois.size());
    for(auto poi : pois){
      RCLCPP_INFO(rclcpp::get_logger("get_pois_info_client"), "POI Name: %s", poi.name.c_str());
    }
  }else{
    RCLCPP_ERROR(rclcpp::get_logger("get_pois_info_client"), "Failed to call service mapoi/get_pois_info");
  }

  rclcpp::shutdown();
  return 0;
}
