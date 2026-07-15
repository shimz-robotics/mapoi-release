#ifndef MAPOI_SERVER__MAPOI_GZ_BRIDGE_HPP_
#define MAPOI_SERVER__MAPOI_GZ_BRIDGE_HPP_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <ros_gz_interfaces/srv/spawn_entity.hpp>
#include <ros_gz_interfaces/srv/delete_entity.hpp>
#include <ros_gz_interfaces/srv/set_entity_pose.hpp>

#include "mapoi_interfaces/msg/initial_pose_request.hpp"


struct GzWorldModelInfo
{
  std::string uri;
  std::string name;
};

struct MapGzInfo
{
  GzWorldModelInfo world_model;
  double initial_x = 0.0;
  double initial_y = 0.0;
  double initial_yaw = 0.0;
  bool has_gazebo = false;
  bool has_initial_pose = false;
};

enum class ConfigLoadStatus
{
  Ok,               // parse 成功 + gazebo section あり
  NoGazeboSection,  // parse 成功 + gazebo section 無し (legitimate)
  ParseError,       // YAML parse or file I/O 失敗 (transient、retry すべき)
};


class MapoiGzBridge : public rclcpp::Node
{
public:
  MapoiGzBridge();
  ~MapoiGzBridge() override;

private:
  void on_config_path(const std_msgs::msg::String::SharedPtr msg);
  void worker_loop();
  void process_config_path(const std::string & path);
  ConfigLoadStatus load_gazebo_info(const std::string & config_path, MapGzInfo & out);
  bool switch_world(
    const std::string & prev_map, const std::string & new_map,
    const MapGzInfo & info);
  bool delete_model_entity(const std::string & name);
  bool spawn_model_from_uri(const std::string & name, const std::string & uri);
  bool teleport_robot(double x, double y, double yaw);

  // parameters
  std::string robot_name_;
  std::string init_world_name_;

  // state (worker thread のみ touch)
  std::string current_map_name_;
  std::string current_world_model_name_;
  std::string current_config_path_;

  // service clients (Reentrant callback group で worker thread から wait_for 可能に)
  rclcpp::CallbackGroup::SharedPtr cb_group_;
  rclcpp::Client<ros_gz_interfaces::srv::SpawnEntity>::SharedPtr spawn_client_;
  rclcpp::Client<ros_gz_interfaces::srv::DeleteEntity>::SharedPtr delete_client_;
  rclcpp::Client<ros_gz_interfaces::srv::SetEntityPose>::SharedPtr set_pose_client_;

  // subscription
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config_path_sub_;
  // mapoi/initialpose_poi (#149 round 7-8 ヘビー high 対応): SelectMap.initial_poi_name 指定時に
  // mapoi_server がここに {map_name, poi_name} を publish するので、bridge も map 一致時に
  // 同じ POI を spawn 位置に採用する。
  rclcpp::Subscription<mapoi_interfaces::msg::InitialPoseRequest>::SharedPtr initialpose_poi_sub_;
  void on_initialpose_poi(
    const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg);
  std::string requested_initial_pose_map_;
  std::string requested_initial_pose_poi_;
  std::mutex requested_initial_pose_mutex_;

  // worker thread + queue (subscribe callback は queue.push のみ、worker が直列処理)
  std::thread worker_;
  std::atomic<bool> stop_worker_{false};
  std::queue<std::string> queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
};

#endif  // MAPOI_SERVER__MAPOI_GZ_BRIDGE_HPP_
