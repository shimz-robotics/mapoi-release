// mapoi_gz_bridge: operator map switch 時に gz-sim (ros_gz_interfaces) の entity を入れ替える。
//
// mapoi/config_path topic を購読し、map_name 変化を検知すると:
// - 旧マップの world_model entity を /world/<init_world>/remove で削除
// - 新マップの world_model entity を /world/<init_world>/create で生成 (model:// URI で)
// - ロボットは /world/<init_world>/set_pose で initial_pose POI 座標に teleport
//   (gz-sim は SetEntityPose が実装されているため delete + spawn は不要)
// gz-sim 本体は無停止で /clock, /tf, /odom, /scan の継続性を保つ。
//
// map 依存情報 (world_model の URI / name) は各 map の mapoi_config.yaml の
// gazebo: セクションに置き、mapoi/config_path 受信時に本 node が直接 parse する。
// ロボット依存情報 (entity_name) と gz-sim の world 名 (init_world_name) は
// launch から parameter で渡す。
//
// NOTE: Jazzy 以降 (gz-sim) 専用。Humble (Gazebo Classic) 用は別 node
// mapoi_gazebo_bridge を参照。API が異なるため (gazebo_msgs vs ros_gz_interfaces)
// 同一コードでの distro 分岐はせず、別 node に分離している。
//
// gz-sim の native service (gz transport) を ROS 2 service として使うために、
// launch から parameter_bridge を同時起動する想定 (mapoi_bringup.launch.yaml 参照)。

#include "mapoi_server/mapoi_gz_bridge.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>

#include <yaml-cpp/yaml.h>
#include <ros_gz_interfaces/msg/entity.hpp>

#include "mapoi_server/initial_pose_resolver.hpp"

using namespace std::chrono_literals;
using std::placeholders::_1;


MapoiGzBridge::MapoiGzBridge()
: Node("mapoi_gz_bridge")
{
  this->declare_parameter<std::string>("robot_entity_name", "burger");
  this->declare_parameter<std::string>("init_world_name", "default");

  robot_name_ = this->get_parameter("robot_entity_name").as_string();
  init_world_name_ = this->get_parameter("init_world_name").as_string();

  // gz-sim native service 名 (parameter_bridge で同名 ROS 2 service として bridge される想定)
  const std::string spawn_srv = "/world/" + init_world_name_ + "/create";
  const std::string remove_srv = "/world/" + init_world_name_ + "/remove";
  const std::string set_pose_srv = "/world/" + init_world_name_ + "/set_pose";

  // Reentrant callback group + MultiThreadedExecutor で worker thread から
  // async_send_request().future.wait_for() が executor の別 thread で resolve される。
  cb_group_ = this->create_callback_group(rclcpp::CallbackGroupType::Reentrant);
  // Jazzy: create_client(name, rmw_qos_profile_t, group) は deprecated、
  // rclcpp::ServicesQoS() を使う。
  spawn_client_ = this->create_client<ros_gz_interfaces::srv::SpawnEntity>(
    spawn_srv, rclcpp::ServicesQoS(), cb_group_);
  delete_client_ = this->create_client<ros_gz_interfaces::srv::DeleteEntity>(
    remove_srv, rclcpp::ServicesQoS(), cb_group_);
  set_pose_client_ = this->create_client<ros_gz_interfaces::srv::SetEntityPose>(
    set_pose_srv, rclcpp::ServicesQoS(), cb_group_);

  auto sub_opts = rclcpp::SubscriptionOptions();
  sub_opts.callback_group = cb_group_;
  // publisher (mapoi_server の config_path_publisher_) は transient_local なので、
  // subscriber も transient_local にして、bridge が後起動/再起動した時に
  // 直近の config_path を latched 値として受け取れるようにする。
  auto sub_qos = rclcpp::QoS(rclcpp::KeepLast(10)).transient_local().reliable();
  config_path_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/config_path", sub_qos,
    std::bind(&MapoiGzBridge::on_config_path, this, _1),
    sub_opts);
  // mapoi/initialpose_poi (transient_local) を subscribe して、SelectMap.initial_poi_name
  // 指定時に bridge も同じ POI を spawn 位置に採用する (#149 round 7 ヘビー high 対応)。
  initialpose_poi_sub_ = this->create_subscription<mapoi_interfaces::msg::InitialPoseRequest>(
    "mapoi/initialpose_poi", sub_qos,
    std::bind(&MapoiGzBridge::on_initialpose_poi, this, _1),
    sub_opts);

  worker_ = std::thread(&MapoiGzBridge::worker_loop, this);

  RCLCPP_INFO(this->get_logger(),
    "mapoi_gz_bridge initialized: robot=%s, init_world_name=%s",
    robot_name_.c_str(), init_world_name_.c_str());
}

MapoiGzBridge::~MapoiGzBridge()
{
  stop_worker_ = true;
  queue_cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void MapoiGzBridge::on_initialpose_poi(
  const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg)
{
  // {map_name, poi_name} を保持。worker thread の load_gazebo_info で「処理中 map と一致するか」
  // を check してから採用する (#149 round 8 high)。
  std::lock_guard<std::mutex> lock(requested_initial_pose_mutex_);
  requested_initial_pose_map_ = msg->map_name;
  requested_initial_pose_poi_ = msg->poi_name;
}

void MapoiGzBridge::on_config_path(const std_msgs::msg::String::SharedPtr msg)
{
  // 軽量。queue に push して即 return。state 操作と service call は worker。
  // mapoi_server は config_path を event 駆動で publish する (周期 re-publish は #135 で廃止し
  // transient_local QoS に移行。起動時 / select_map / reload_map_info の 3 箇所)。それでも worker が
  // service 不達などで blocking 中に複数 event が来ると queue が膨らみ得るため、常に latest 1 件のみ
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

void MapoiGzBridge::worker_loop()
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

void MapoiGzBridge::process_config_path(const std::string & path)
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

  MapGzInfo info;
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
      if (!delete_model_entity(current_world_model_name_)) {
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
      "Initial map=%s; assuming gz-sim already loaded matching world model",
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

ConfigLoadStatus MapoiGzBridge::load_gazebo_info(
  const std::string & config_path, MapGzInfo & out)
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

bool MapoiGzBridge::switch_world(
  const std::string & prev_map, const std::string & new_map,
  const MapGzInfo & info)
{
  (void)prev_map;
  (void)new_map;

  // 旧 world_model delete
  if (!current_world_model_name_.empty()) {
    if (!delete_model_entity(current_world_model_name_)) {
      RCLCPP_WARN(this->get_logger(),
        "delete world_model failed; aborting world switch (旧 world_model stays)");
      return false;
    }
    current_world_model_name_.clear();
  }

  bool all_ok = true;

  // 新 world_model spawn
  if (!info.world_model.uri.empty()) {
    if (spawn_model_from_uri(info.world_model.name, info.world_model.uri)) {
      current_world_model_name_ = info.world_model.name;
    } else {
      all_ok = false;
    }
  }

  // robot teleport (gz-sim は SetEntityPose で 1 回。delete+spawn は不要)
  if (info.has_initial_pose) {
    if (!teleport_robot(info.initial_x, info.initial_y, info.initial_yaw)) {
      all_ok = false;
    }
  } else {
    RCLCPP_WARN(this->get_logger(),
      "No initial_pose POI for %s; skipping robot teleport", new_map.c_str());
  }

  return all_ok;
}

bool MapoiGzBridge::delete_model_entity(const std::string & name)
{
  if (!delete_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(),
      "delete service not available (/world/%s/remove)",
      init_world_name_.c_str());
    return false;
  }
  auto req = std::make_shared<ros_gz_interfaces::srv::DeleteEntity::Request>();
  req->entity.name = name;
  req->entity.type = ros_gz_interfaces::msg::Entity::MODEL;
  auto future = delete_client_->async_send_request(req);
  if (future.wait_for(5s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "delete_model_entity(%s) timed out", name.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "delete_model_entity(%s) failed (gz returned success=false)", name.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(), "delete_model_entity(%s): success", name.c_str());
  return true;
}

bool MapoiGzBridge::spawn_model_from_uri(
  const std::string & name, const std::string & uri)
{
  if (!spawn_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(),
      "spawn service not available (/world/%s/create)",
      init_world_name_.c_str());
    return false;
  }
  // gz-sim の EntityFactory.sdf に inline SDF を渡す。
  // model:// URI は gz-sim 側 (gz_sim プロセス) の GZ_SIM_RESOURCE_PATH で解決される。
  std::ostringstream sdf;
  sdf << "<?xml version=\"1.0\"?>"
      << "<sdf version=\"1.9\">"
      << "<include><uri>" << uri << "</uri></include>"
      << "</sdf>";
  auto req = std::make_shared<ros_gz_interfaces::srv::SpawnEntity::Request>();
  req->entity_factory.name = name;
  req->entity_factory.sdf = sdf.str();
  auto future = spawn_client_->async_send_request(req);
  if (future.wait_for(10s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "spawn_model_from_uri(%s) timed out", name.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "spawn_model_from_uri(%s, %s) failed (gz returned success=false)",
      name.c_str(), uri.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(),
    "spawn_model_from_uri(%s, %s): success", name.c_str(), uri.c_str());
  return true;
}

bool MapoiGzBridge::teleport_robot(double x, double y, double yaw)
{
  if (!set_pose_client_->wait_for_service(10s)) {
    RCLCPP_WARN(this->get_logger(),
      "set_pose service not available (/world/%s/set_pose)",
      init_world_name_.c_str());
    return false;
  }
  auto req = std::make_shared<ros_gz_interfaces::srv::SetEntityPose::Request>();
  req->entity.name = robot_name_;
  req->entity.type = ros_gz_interfaces::msg::Entity::MODEL;
  req->pose.position.x = x;
  req->pose.position.y = y;
  req->pose.position.z = 0.01;
  req->pose.orientation.z = std::sin(yaw / 2.0);
  req->pose.orientation.w = std::cos(yaw / 2.0);
  auto future = set_pose_client_->async_send_request(req);
  if (future.wait_for(10s) != std::future_status::ready) {
    RCLCPP_WARN(this->get_logger(),
      "teleport_robot(%s) timed out", robot_name_.c_str());
    return false;
  }
  auto res = future.get();
  if (!res->success) {
    RCLCPP_WARN(this->get_logger(),
      "teleport_robot(%s) failed (gz returned success=false)", robot_name_.c_str());
    return false;
  }
  RCLCPP_INFO(this->get_logger(),
    "teleport_robot(%s, %.2f, %.2f, yaw=%.2f): success",
    robot_name_.c_str(), x, y, yaw);
  return true;
}


int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MapoiGzBridge>();
  rclcpp::executors::MultiThreadedExecutor executor;
  executor.add_node(node);
  executor.spin();
  rclcpp::shutdown();
  return 0;
}
