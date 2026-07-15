#ifndef MAPOI_SERVER__MAPOI_GAZEBO_BRIDGE_HPP_
#define MAPOI_SERVER__MAPOI_GAZEBO_BRIDGE_HPP_

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <gazebo_msgs/srv/spawn_entity.hpp>
#include <gazebo_msgs/srv/delete_entity.hpp>

#include "mapoi_interfaces/msg/initial_pose_request.hpp"


struct GazeboWorldModelInfo
{
  std::string uri;
  std::string name;
};

struct MapGazeboInfo
{
  GazeboWorldModelInfo world_model;
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


class MapoiGazeboBridge : public rclcpp::Node
{
public:
  MapoiGazeboBridge();
  ~MapoiGazeboBridge() override;

private:
  void on_config_path(const std_msgs::msg::String::SharedPtr msg);
  void worker_loop();
  void process_config_path(const std::string & path);
  ConfigLoadStatus load_gazebo_info(const std::string & config_path, MapGazeboInfo & out);
  bool switch_world(
    const std::string & prev_map, const std::string & new_map,
    const MapGazeboInfo & info);
  bool delete_entity(const std::string & name);
  bool spawn_entity_from_uri(const std::string & name, const std::string & uri);
  bool respawn_robot(double x, double y, double yaw);
  // Classic 固有 (#91 仮説 A): respawn_robot の delete + spawn 中に robot がシーンから一時消失 →
  // AMCL の laser scan が新 map と不整合な過渡状態で誤った peak に収束する問題への対策。
  // spawn 完了後に短い delay を挟んで bridge から /initialpose を late publish し、AMCL に
  // 「正しい spawn 位置」を最後の権威として教える。gz-sim 経路 (mapoi_gz_bridge) は SetEntityPose で
  // atomic teleport なので不要、本対策は Classic 専用。
  void publish_initialpose_after_respawn(double x, double y, double yaw);

  // parameters
  std::string robot_name_;
  std::string robot_sdf_path_;
  // /initialpose late publish parameters (#91)。default は declare_parameter (cpp) 側を SSOT。
  // delay のみ parameter 化 (環境依存: PC スペック / Gazebo spawn 速度で調整余地)。
  // publish 回数 / 間隔は AMCL の covariance 拡散ぶれ抑制に必要な最小構成として cpp 側 constexpr で固定。
  std::string initial_pose_topic_;
  int respawn_initialpose_delay_ms_;

  // state (worker thread のみが touch)
  std::string current_map_name_;
  std::string current_world_model_name_;
  std::string current_config_path_;

  // service clients (Reentrant callback group で worker thread から wait_for 可能に)
  rclcpp::CallbackGroup::SharedPtr cb_group_;
  rclcpp::Client<gazebo_msgs::srv::SpawnEntity>::SharedPtr spawn_client_;
  rclcpp::Client<gazebo_msgs::srv::DeleteEntity>::SharedPtr delete_client_;

  // /initialpose late publisher (#91)
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr initialpose_pub_;

  // subscription
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config_path_sub_;
  // mapoi/initialpose_poi (#149 round 7-8 ヘビー high 対応): SelectMap.initial_poi_name 指定時に
  // mapoi_server がここに {map_name, poi_name} を publish するので、bridge も map 一致時に
  // 同じ POI を spawn 位置に採用する。
  rclcpp::Subscription<mapoi_interfaces::msg::InitialPoseRequest>::SharedPtr initialpose_poi_sub_;
  void on_initialpose_poi(
    const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg);
  // {map_name, poi_name} を保持。worker thread が「処理中の map と一致する場合のみ」採用。
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

#endif  // MAPOI_SERVER__MAPOI_GAZEBO_BRIDGE_HPP_
