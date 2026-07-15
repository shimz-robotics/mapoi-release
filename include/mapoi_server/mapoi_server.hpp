#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cmath>
#include <filesystem>

#include <yaml-cpp/yaml.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "mapoi_interfaces/srv/get_pois_info.hpp"
#include "mapoi_interfaces/srv/get_route_pois.hpp"
#include "mapoi_interfaces/msg/point_of_interest.hpp"
#include "mapoi_interfaces/msg/initial_pose_request.hpp"
#include "mapoi_interfaces/srv/get_maps_info.hpp"
#include "mapoi_interfaces/srv/get_routes_info.hpp"
#include "mapoi_interfaces/srv/select_map.hpp"
#include "mapoi_interfaces/srv/request_initial_pose.hpp"
#include "mapoi_interfaces/srv/get_tag_definitions.hpp"
#include "mapoi_interfaces/msg/tag_definition.hpp"


class MapoiServer : public rclcpp::Node
{
public:
  MapoiServer();

private:
  // parameters & internal state
  std::string maps_path_;
  std::string map_name_;
  std::string config_file_;
  std::string config_path_;
  // #297: last-selected map を永続化する書き込み可能ディレクトリ。空 = 永続化無効
  // (従来挙動)。非空なら <state_path_>/last_selected_map の有無で「初回起動 vs
  // 運用中再起動」を判別し、再起動時は map context 復元 + 起動時 publish を clear 化する。
  std::string state_path_;

  // publishers
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr config_path_publisher_;
  // initial pose POI 名 publisher (#144): mapoi_server が新 map の初期 POI 名を決定して
  // mapoi/initialpose_poi topic に publish。mapoi_nav2_bridge がそれを受けて /initialpose を流す。
  // QoS: transient_local (depth=1) で後起動 subscriber でも受信できる。
  rclcpp::Publisher<mapoi_interfaces::msg::InitialPoseRequest>::SharedPtr initialpose_poi_publisher_;

  // initial pose 用の POI 名 publish (#144)。compute_initial_poi_name は class-level public static。
  std::string resolve_initial_poi_name(const std::string & requested_name);
  void publish_initial_poi_name(const std::string & requested_name);
  // transient_local の latched 値を「採用候補なし」を示す skip message (poi_name 空) で
  // 上書きする (#154)。reload で呼び、reload 直前の古い POI 名が late subscriber に
  // 配信される問題を防ぐ。
  void publish_initialpose_clear();

  // mapoi/config_path topic の publish (transient_local QoS で latched)。起動時 / select_map /
  // reload_map_info で呼ぶ。定期 publish は subscriber を transient_local に揃えて廃止 (#135)。
  void publish_config_path();

  // methods
  void load_mapoi_config_file();
  void load_tag_definitions();
  // #297: 現在の map_name_ を state file に書く (state_path_ 空なら何もしない)。
  // 起動時 + select_map 成功時に呼ぶ。運用中の書き込み失敗は WARN のみで稼働継続
  // (起動時の書き込み可否は constructor で fail fast 検証済み)。
  void persist_last_selected_map();
  YAML::Node pois_list_;
  YAML::Node routes_list_;
  YAML::Node nav2_map_list_;

  // tag definitions (system tags loaded once, user tags merged on config load)
  std::vector<mapoi_interfaces::msg::TagDefinition> system_tags_;
  std::vector<mapoi_interfaces::msg::TagDefinition> tag_definitions_;

  // POI YAML conversion helper
  mapoi_interfaces::msg::PointOfInterest yaml_to_poi_msg(const YAML::Node & poi);

  // services
  rclcpp::Service<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr get_pois_info_service_;
  rclcpp::Service<mapoi_interfaces::srv::GetRoutePois>::SharedPtr get_route_pois_service_;
  rclcpp::Service<mapoi_interfaces::srv::GetMapsInfo>::SharedPtr get_maps_info_service_;
  rclcpp::Service<mapoi_interfaces::srv::GetRoutesInfo>::SharedPtr get_routes_info_service_;
  rclcpp::Service<mapoi_interfaces::srv::SelectMap>::SharedPtr select_map_service_;
  // #211: mapoi/initialpose_poi の唯一の writer として、外部 requester
  // (mapoi_nav2_bridge / WebUI / RViz panel) の publish 依頼を受ける service。
  rclcpp::Service<mapoi_interfaces::srv::RequestInitialPose>::SharedPtr request_initial_pose_service_;
  rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr reload_map_info_service_;
  rclcpp::Service<mapoi_interfaces::srv::GetTagDefinitions>::SharedPtr get_tag_definitions_srv_;

  // callbacks
  void get_pois_info_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetPoisInfo::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetPoisInfo::Response> response);
  void get_route_pois_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetRoutePois::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetRoutePois::Response> response);
  void get_maps_info_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetMapsInfo::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetMapsInfo::Response> response);
  void get_routes_info_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetRoutesInfo::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetRoutesInfo::Response> response);
  void select_map_service(
    const std::shared_ptr<mapoi_interfaces::srv::SelectMap::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::SelectMap::Response> response);
  // #211: 外部 requester からの依頼を受けて mapoi/initialpose_poi に publish する。
  // poi_name 空は clear (skip message) として publish する。
  void request_initial_pose_service(
    const std::shared_ptr<mapoi_interfaces::srv::RequestInitialPose::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::RequestInitialPose::Response> response);
  void reload_map_info_service(
    const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
    std::shared_ptr<std_srvs::srv::Trigger::Response> response);
  void get_tag_definitions_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetTagDefinitions::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetTagDefinitions::Response> response);

};
