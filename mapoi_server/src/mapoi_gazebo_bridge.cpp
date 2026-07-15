// mapoi_gazebo_bridge: operator map switch 時に Gazebo Classic (gazebo_msgs) の entity を入れ替える。
//
// mapoi/config_path topic を購読し、map_name 変化を検知すると:
// - 旧マップの world_model entity を /delete_entity で削除
// - 新マップの world_model entity を /spawn_entity で生成 (model://... URI で)
// - ロボットも /delete_entity + /spawn_entity で initial_pose POI 座標に再生成
// Gazebo 本体は無停止で /clock, /tf, /odom, /scan の継続性を保つ。
//
// map 依存情報 (world_model の URI / name) は各 map の mapoi_config.yaml の
// gazebo: セクションに置き、mapoi/config_path 受信時に本 node が直接 parse する。
// ロボット依存情報 (entity_name / SDF path) は launch から parameter で渡す。
//
// NOTE: Humble (Gazebo Classic) 専用。Jazzy (gz-sim) 用は別 node
// mapoi_gz_bridge を参照。API が異なるため (gazebo_msgs vs ros_gz_interfaces)
// 同一コードでの distro 分岐はせず、別 node に分離している。

#include "mapoi_server/mapoi_gazebo_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "mapoi_server/initial_pose_resolver.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

using namespace std::chrono_literals;
using std::placeholders::_1;

namespace
{
// late /initialpose の publish 回数と間隔 (#91)。AMCL は /initialpose 受信時に particle を
// covariance ± で gaussian 再分布するため、1 回 publish 直後の数 frame は particle が σ 範囲
// (covariance 0.25 → σ=0.5m) に散ったままで /amcl_pose が一時的に ~0.5m ずれる (検証で 47-49cm 観測)。
// 2 回 publish (間隔 500ms) で初回ぶれを上書きすると過渡 drift が消える (検証済)。
// 環境非依存 (AMCL covariance 仕様で決まる) のため parameter 化せず constexpr 固定。
constexpr int kRespawnInitialposePublishCount = 2;
constexpr int kRespawnInitialposePublishIntervalMs = 500;
}  // namespace


MapoiGazeboBridge::MapoiGazeboBridge()
: Node("mapoi_gazebo_bridge")
{
  this->declare_parameter<std::string>("robot_entity_name", "burger");
  this->declare_parameter<std::string>("robot_sdf_path", "");
  // /initialpose late publish parameters (#91 仮説 A 対策、Classic 専用)。
  // delay は spawn 完了から最初の /initialpose publish までの待ち時間。Nav2 LoadMap 完了 + AMCL の
  // 新 map 反映を待つ目的。短い delay だと AMCL の laser scan が新 map と整合する前に publish
  // して再 drift する事象が #91 検証で観測された (delay=300ms で過渡 drift 1-2m)。default 500ms。
  // 環境依存 (PC スペック / Gazebo spawn 速度) のため parameter 化。
  this->declare_parameter<std::string>("initial_pose_topic", "/initialpose");
  this->declare_parameter<int>("respawn_initialpose_delay_ms", 500);

  robot_name_ = this->get_parameter("robot_entity_name").as_string();
  robot_sdf_path_ = this->get_parameter("robot_sdf_path").as_string();
  initial_pose_topic_ = this->get_parameter("initial_pose_topic").as_string();
  // 負値は std::chrono::milliseconds の符号付き duration を不用意に渡すと長時間 sleep / UB の温床
  // になるため 0 下限クランプ。
  respawn_initialpose_delay_ms_ =
    std::max(0, static_cast<int>(this->get_parameter("respawn_initialpose_delay_ms").as_int()));

  // Reentrant callback group + MultiThreadedExecutor で worker thread から
  // async_send_request().future.wait_for() が executor の別 thread で resolve される。
  cb_group_ = this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
  spawn_client_ = this->create_client<gazebo_msgs::srv::SpawnEntity>(
    "spawn_entity", rmw_qos_profile_services_default, cb_group_);
  delete_client_ = this->create_client<gazebo_msgs::srv::DeleteEntity>(
    "delete_entity", rmw_qos_profile_services_default, cb_group_);

  // /initialpose late publisher (#91): mapoi_amcl_localization_bridge と同 topic に bridge からも
  // publish する (#209 で mapoi_nav2_bridge から AMCL adapter 分離後も同じ補助役)。AMCL は最後に到着した
  // /initialpose で particle を再 init するため、bridge の spawn 完了後に late publish することで
  // 「spawn 中の laser scan 不整合」による誤収束を上書きできる。QoS は localization bridge 側
  // publisher と同じ default (depth=1)。
  initialpose_pub_ = this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
    initial_pose_topic_, 1);

  auto sub_opts = rclcpp::SubscriptionOptions();
  sub_opts.callback_group = cb_group_;
  // publisher (mapoi_server の config_path_publisher_) は transient_local なので、
  // subscriber も transient_local にして、bridge が後起動/再起動した時に
  // 直近の config_path を latched 値として受け取れるようにする。
  auto sub_qos = rclcpp::QoS(rclcpp::KeepLast(10)).transient_local().reliable();
  config_path_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/config_path", sub_qos,
    std::bind(&MapoiGazeboBridge::on_config_path, this, _1),
    sub_opts);
  // mapoi/initialpose_poi (transient_local) を subscribe して、SelectMap.initial_poi_name
  // 指定時に bridge も同じ POI を spawn 位置に採用する (#149 round 7 ヘビー high 対応)。
  initialpose_poi_sub_ = this->create_subscription<mapoi_interfaces::msg::InitialPoseRequest>(
    "mapoi/initialpose_poi", sub_qos,
    std::bind(&MapoiGazeboBridge::on_initialpose_poi, this, _1),
    sub_opts);

  worker_ = std::thread(&MapoiGazeboBridge::worker_loop, this);

  RCLCPP_INFO(this->get_logger(),
    "mapoi_gazebo_bridge initialized: robot=%s, sdf=%s",
    robot_name_.c_str(),
    robot_sdf_path_.empty() ? "(not set)" : robot_sdf_path_.c_str());
}

MapoiGazeboBridge::~MapoiGazeboBridge()
{
  stop_worker_ = true;
  queue_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void MapoiGazeboBridge::on_initialpose_poi(
  const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg)
{
  // {map_name, poi_name} を保持。worker thread の load_gazebo_info で「処理中 map と一致するか」
  // を check してから採用する (#149 round 8 high)。
  std::lock_guard<std::mutex> lock(requested_initial_pose_mutex_);
  requested_initial_pose_map_ = msg->map_name;
  requested_initial_pose_poi_ = msg->poi_name;
}

void MapoiGazeboBridge::on_config_path(const std_msgs::msg::String::SharedPtr msg)
{
  // 軽量。queue に push して即 return。state 操作と service call は worker。
  // mapoi_server は config_path を 500ms 周期で re-publish するため、worker が
  // service 不達などで blocking 中だと queue が際限なく膨らむ。常に latest 1 件のみ
  // 保持する coalesce 戦略で、stale な path は破棄する。
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!queue_.empty()) {
      queue_.pop();
    }
    queue_.push(msg->data);
  }
  queue_cv_.notify_one();
}

void MapoiGazeboBridge::worker_loop()
{
  while (!stop_worker_) {
    std::string path;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !queue_.empty() || stop_worker_; });
      if (stop_worker_) {
        break;
      }
      path = queue_.front();
      queue_.pop();
    }
    try {
      process_config_path(path);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(this->get_logger(), "worker exception: %s", e.what());
    }
  }
}

void MapoiGazeboBridge::process_config_path(const std::string & path)
{
  if (path == current_config_path_) {
    return;
  }

  // path の構造: <maps_path>/<map_name>/<config_file>
  std::filesystem::path p(path);
  std::string map_name = p.parent_path().filename().string();
  RCLCPP_INFO(this->get_logger(),
    "config_path changed → map_name=%s", map_name.c_str());

  if (map_name == current_map_name_) {
    current_config_path_ = path;
    return;
  }

  std::string prev_map_name = current_map_name_;

  MapGazeboInfo info;
  const auto status = load_gazebo_info(path, info);
  if (status == ConfigLoadStatus::ParseError) {
    // transient な失敗 (YAML parse / I/O)。state 進めず、mapoi_server の
    // 周期 re-publish (500ms) で retry する。
    RCLCPP_WARN(this->get_logger(),
      "load_gazebo_info parse error for %s; keeping state for retry",
      path.c_str());
    return;
  }
  if (status == ConfigLoadStatus::NoGazeboSection) {
    // legitimate: gazebo セクション無し map (例: 実機用 map config)。
    // sim 側に旧 world_model が残っていれば cleanup してから state を進める
    // (sim 側 state と map state の乖離防止)。delete に失敗したら state を進めず retry。
    if (!current_world_model_name_.empty()) {
      RCLCPP_INFO(this->get_logger(),
        "No gazebo section in %s; cleaning up stale world_model=%s",
        path.c_str(), current_world_model_name_.c_str());
      if (!delete_entity(current_world_model_name_)) {
        RCLCPP_WARN(this->get_logger(),
          "delete world_model failed during NoGazeboSection cleanup; "
          "keeping state for retry");
        return;
      }
      current_world_model_name_.clear();
    } else {
      RCLCPP_INFO(this->get_logger(),
        "No gazebo section in %s; skipping entity swap", path.c_str());
    }
    current_map_name_ = map_name;
    current_config_path_ = path;
    return;
  }
  // status == Ok: 以下通常処理

  if (prev_map_name.empty()) {
    // 起動時の最初の通知: launch が既に対応する world で起動済みと仮定。
    // 内部 state のみ更新、entity 入れ替えはしない。
    current_world_model_name_ = info.world_model.name;
    current_map_name_ = map_name;
    current_config_path_ = path;
    RCLCPP_INFO(this->get_logger(),
      "Initial map=%s; assuming Gazebo already loaded matching world",
      map_name.c_str());
    return;
  }

  if (switch_world(prev_map_name, map_name, info)) {
    current_map_name_ = map_name;
    current_config_path_ = path;
  } else {
    RCLCPP_WARN(this->get_logger(),
      "switch_world %s → %s failed; keeping state for retry",
      prev_map_name.c_str(), map_name.c_str());
  }
}

ConfigLoadStatus MapoiGazeboBridge::load_gazebo_info(
  const std::string & config_path, MapGazeboInfo & out)
{
  try {
    YAML::Node cfg = YAML::LoadFile(config_path);
    if (cfg["gazebo"] && cfg["gazebo"]["world_model"]) {
      const auto & wm = cfg["gazebo"]["world_model"];
      out.world_model.uri = wm["uri"] ? wm["uri"].as<std::string>() : "";
      out.world_model.name = wm["name"] ? wm["name"].as<std::string>() : "";
      out.has_gazebo = !out.world_model.uri.empty();
    }
    // initial pose は (1) latched mapoi/initialpose_poi (= SelectMap.initial_poi_name 指定) があれば
    // それを優先、(2) なければ POI list 先頭 (landmark 除外、pose 妥当性 check) を採用。選定 logic は
    // `mapoi::select_initial_poi_name` で共通化 (#144 / #149 round 7 ヘビー high / #150)。
    // 注: map_name による世代検証は #149 round 10 で取り下げ (#174 で publisher 側 latched 上書きに移行)。
    //   stale 排除は publisher 上書き (transient_local depth=1) に依存する。
    std::string requested;
    {
      std::lock_guard<std::mutex> lock(requested_initial_pose_mutex_);
      requested = requested_initial_pose_poi_;
    }
    const std::string target = mapoi::select_initial_poi_name(cfg["poi"], requested);
    if (!target.empty()) {
      if (!requested.empty() && target != requested) {
        // requested 名は届いていたが採用に失敗した (= name not found / landmark / pose 不備) (#149 round 9 low)。
        RCLCPP_WARN(this->get_logger(),
          "Requested initial POI '%s' not adopted for spawn (not found / landmark / invalid pose); "
          "falling back to POI list first ('%s').",
          requested.c_str(), target.c_str());
      } else if (!requested.empty()) {
        RCLCPP_INFO(this->get_logger(),
          "Adopted requested initial POI '%s' for spawn position.", target.c_str());
      }
      // select_initial_poi_name で valid pose 保証済 (landmark 除外 / x/y/yaw 全 numeric)。
      for (const auto & poi : cfg["poi"]) {
        if (poi["name"].as<std::string>("") != target) continue;
        const auto & pose = poi["pose"];
        out.initial_x = pose["x"].as<double>();
        out.initial_y = pose["y"].as<double>();
        out.initial_yaw = pose["yaw"].as<double>();
        out.has_initial_pose = true;
        break;
      }
    }
  } catch (const YAML::Exception & e) {
    RCLCPP_ERROR(this->get_logger(),
      "Failed to parse %s: %s", config_path.c_str(), e.what());
    return ConfigLoadStatus::ParseError;
  }
  return out.has_gazebo ? ConfigLoadStatus::Ok : ConfigLoadStatus::NoGazeboSection;
}

bool MapoiGazeboBridge::switch_world(
  const std::string & prev_map, const std::string & new_map,
  const MapGazeboInfo & info)
{
  (void)prev_map;
  (void)new_map;

  // 旧 world_model delete
  if (!current_world_model_name_.empty()) {
    if (!delete_entity(current_world_model_name_)) {
      RCLCPP_WARN(this->get_logger(),
        "delete world_model failed; aborting world switch (旧 world_model stays)");
      return false;
    }
    current_world_model_name_.clear();
  }

  bool all_ok = true;

  // 新 world_model spawn
  if (!info.world_model.uri.empty()) {
    if (spawn_entity_from_uri(info.world_model.name, info.world_model.uri)) {
      current_world_model_name_ = info.world_model.name;
    } else {
      all_ok = false;
    }
  }

  // robot respawn (initial_pose POI がある場合のみ)
  if (info.has_initial_pose) {
    if (!respawn_robot(info.initial_x, info.initial_y, info.initial_yaw)) {
      all_ok = false;
    } else {
      // Classic 固有 (#91 仮説 A): respawn 成功直後に /initialpose を late publish して AMCL を
      // spawn 位置に強制的に再 init する。delete + spawn 中に AMCL が誤収束しても、最後の
      // /initialpose で particle を spawn 位置に戻せる。gz-sim 経路は SetEntityPose で atomic
      // teleport なので不要 (mapoi_gz_bridge には実装しない)。
      publish_initialpose_after_respawn(info.initial_x, info.initial_y, info.initial_yaw);
    }
  } else {
    RCLCPP_WARN(this->get_logger(),
      "No initial_pose POI for %s; skipping robot respawn", new_map.c_str());
  }

  return all_ok;
}

bool MapoiGazeboBridge::delete_entity(const std::string & name)
{
  if (!delete_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(), "delete_entity service not available");
    return false;
  }
  auto req = std::make_shared<gazebo_msgs::srv::DeleteEntity::Request>();
  req->name = name;
  auto future = delete_client_->async_send_request(req);
  if (future.wait_for(5s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "delete_entity(%s) timed out", name.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "delete_entity(%s) failed: %s",
      name.c_str(), res->status_message.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(), "delete_entity(%s): success", name.c_str());
  return true;
}

bool MapoiGazeboBridge::spawn_entity_from_uri(
  const std::string & name, const std::string & uri)
{
  if (!spawn_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(), "spawn_entity service not available");
    return false;
  }
  std::ostringstream sdf;
  sdf << "<?xml version=\"1.0\"?>"
      << "<sdf version=\"1.6\"><world name=\"default\">"
      << "<include><uri>" << uri << "</uri></include>"
      << "</world></sdf>";
  auto req = std::make_shared<gazebo_msgs::srv::SpawnEntity::Request>();
  req->name = name;
  req->xml = sdf.str();
  auto future = spawn_client_->async_send_request(req);
  if (future.wait_for(10s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "spawn_entity(%s) timed out", name.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "spawn_entity(%s) failed: %s",
      name.c_str(), res->status_message.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(),
    "spawn_entity(%s, %s): success", name.c_str(), uri.c_str());
  return true;
}

bool MapoiGazeboBridge::respawn_robot(double x, double y, double yaw)
{
  // preflight: 失敗するなら delete もしない
  if (robot_sdf_path_.empty()) {
    RCLCPP_WARN(this->get_logger(),
      "robot_sdf_path is empty; skipping respawn (robot stays at old pose)");
    return false;
  }
  if (!std::filesystem::exists(robot_sdf_path_)) {
    RCLCPP_ERROR(this->get_logger(),
      "robot_sdf_path not found: %s", robot_sdf_path_.c_str());
    return false;
  }
  if (!spawn_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(),
      "spawn_entity service not available; skipping robot respawn");
    return false;
  }
  std::ifstream f(robot_sdf_path_);
  if (!f.is_open()) {
    RCLCPP_ERROR(this->get_logger(),
      "Failed to open robot SDF: %s", robot_sdf_path_.c_str());
    return false;
  }
  std::stringstream buf;
  buf << f.rdbuf();
  const std::string sdf = buf.str();

  // delete は失敗しても spawn を試みる
  // (前回 delete 成功 + spawn 失敗で robot 不在の状態からの retry 対応。
  // 重複があれば spawn 側が success=false で検知)
  if (!delete_entity(robot_name_)) {
    RCLCPP_INFO(this->get_logger(),
      "delete robot returned not-success; entity may already be gone. "
      "will still attempt spawn at new pose");
  }

  auto req = std::make_shared<gazebo_msgs::srv::SpawnEntity::Request>();
  req->name = robot_name_;
  req->xml = sdf;
  req->initial_pose.position.x = x;
  req->initial_pose.position.y = y;
  req->initial_pose.position.z = 0.01;
  req->initial_pose.orientation.z = std::sin(yaw / 2.0);
  req->initial_pose.orientation.w = std::cos(yaw / 2.0);
  auto future = spawn_client_->async_send_request(req);
  if (future.wait_for(10s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "respawn_robot(%s) timed out (robot may be deleted)",
      robot_name_.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "respawn_robot(%s) failed: %s",
      robot_name_.c_str(), res->status_message.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(),
    "respawn_robot(%s, %.2f, %.2f, yaw=%.2f): success",
    robot_name_.c_str(), x, y, yaw);
  return true;
}


void MapoiGazeboBridge::publish_initialpose_after_respawn(double x, double y, double yaw)
{
  // delay は worker thread の sleep。ROS executor (別 thread) は spin 継続するため OK。
  // 目的: Nav2 LoadMap 完了 + AMCL の新 map 反映を待ってから /initialpose を流すことで、
  // 古い map に対する AMCL の誤収束を「新 map + 正しい spawn 位置」で上書きする確率を上げる。
  if (respawn_initialpose_delay_ms_ > 0) {
    std::this_thread::sleep_for(
      std::chrono::milliseconds(respawn_initialpose_delay_ms_));
  }

  geometry_msgs::msg::PoseWithCovarianceStamped msg;
  msg.header.frame_id = "map";
  msg.pose.pose.position.x = x;
  msg.pose.pose.position.y = y;
  msg.pose.pose.position.z = 0.0;
  msg.pose.pose.orientation.z = std::sin(yaw / 2.0);
  msg.pose.pose.orientation.w = std::cos(yaw / 2.0);
  // covariance は mapoi_amcl_localization_bridge::publish_initial_pose と同じ default
  // (#209 で AMCL adapter を旧 mapoi_nav_server から分離した時点で実装はそちら側に移動)。
  // 0.25 (m^2, x/y) と ~0.069 (rad^2, yaw、約 ±15deg) は AMCL の典型値。
  msg.pose.covariance[0] = 0.25;
  msg.pose.covariance[7] = 0.25;
  msg.pose.covariance[35] = 0.06853891945200942;

  for (int i = 0; i < kRespawnInitialposePublishCount; ++i) {
    msg.header.stamp = this->now();
    initialpose_pub_->publish(msg);
    RCLCPP_INFO(this->get_logger(),
      "Published late /initialpose after respawn (%d/%d): (%.2f, %.2f, yaw=%.2f) on '%s'",
      i + 1, kRespawnInitialposePublishCount, x, y, yaw, initial_pose_topic_.c_str());
    if (i + 1 < kRespawnInitialposePublishCount) {
      std::this_thread::sleep_for(
        std::chrono::milliseconds(kRespawnInitialposePublishIntervalMs));
    }
  }
}


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MapoiGazeboBridge>();
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();
  rclcpp::shutdown();
  return 0;
}
