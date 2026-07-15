#include "mapoi_server/mapoi_amcl_localization_bridge.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

using namespace std::chrono_literals;
using std::placeholders::_1;

namespace mapoi
{

MapoiAmclLocalizationBridge::MapoiAmclLocalizationBridge(
  const rclcpp::NodeOptions & options)
: Node("mapoi_amcl_localization_bridge", options)
{
  this->get_logger().set_level(rclcpp::Logger::Level::Info);

  // localization-agnostic 化のための parameter (#209 で mapoi_nav2_bridge から移設):
  // - initial_pose_topic: 配信先 topic 名 (default `/initialpose`、AMCL/slam_toolbox 等の de-facto standard)
  // - initialpose_retry_interval_sec / initialpose_retry_max_attempts: subscriber 後起動時の
  //   async retry 設定 (#152)。default 0.1s × 50 = 最大 5 秒待つ。
  // - initialpose_post_subscribe_republish_count: subscriber 検知後の追加 republish 回数 (#149 round 5)
  // - map_frame: PoseWithCovarianceStamped.header.frame_id
  this->declare_parameter<std::string>("initial_pose_topic", "/initialpose");
  this->declare_parameter<double>("initialpose_retry_interval_sec", 0.1);
  this->declare_parameter<int>("initialpose_retry_max_attempts", 50);
  this->declare_parameter<int>("initialpose_post_subscribe_republish_count", 3);
  this->declare_parameter<std::string>("map_frame", "map");

  // mapoi/initialpose_poi (transient_local): mapoi_server / mapoi_nav2_bridge / WebUI 等が publish する
  // {map_name, poi_name} を受けて、POI list を fetch して /initialpose に流す。
  initialpose_poi_sub_ = this->create_subscription<mapoi_interfaces::msg::InitialPoseRequest>(
    "mapoi/initialpose_poi", rclcpp::QoS(1).transient_local(),
    std::bind(&MapoiAmclLocalizationBridge::initialpose_poi_callback, this, _1));

  initialpose_pub_ = this->create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
    this->get_parameter("initial_pose_topic").as_string(), 1);

  pois_info_client_ = this->create_client<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info");

  // Localization backend readiness publisher + 1Hz polling timer (#209)。
  // /initialpose の subscriber 有無は実行時に変化し得る (AMCL を後起動 / 落とすケース) ため、
  // event-driven ではなく polling で publish する。1Hz は WebUI 表示の応答性として十分
  // (Navigation 側 #198 と同じ割り切り)。
  // QoS は LocalizationBackendStatus.msg の contract に従う (#208):
  // transient_local + MANUAL_BY_TOPIC liveliness + 5s lease。各 publish() が assert を兼ねる
  // ので 1Hz timer が止まれば 5s 後に subscriber 側 Liveliness Changed event が発火する。
  backend_status_pub_ = this->create_publisher<mapoi_interfaces::msg::LocalizationBackendStatus>(
    "mapoi/localization/backend_status",
    rclcpp::QoS(1)
      .transient_local()
      .liveliness(rclcpp::LivelinessPolicy::ManualByTopic)
      .liveliness_lease_duration(5s));
  backend_status_timer_ = this->create_wall_timer(
    1s, std::bind(&MapoiAmclLocalizationBridge::publish_backend_status, this));

  RCLCPP_INFO(this->get_logger(),
    "MapoiAmclLocalizationBridge initialized (initial_pose_topic=%s).",
    this->get_parameter("initial_pose_topic").as_string().c_str());
}

void MapoiAmclLocalizationBridge::initialpose_poi_callback(
  const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg)
{
  const std::string poi_name = msg->poi_name;
  if (poi_name.empty()) {
    RCLCPP_DEBUG(this->get_logger(),
      "Received empty initialpose POI name for map '%s'; skipping.", msg->map_name.c_str());
    return;
  }
  // 注: map_name 世代検証は旧 mapoi_nav_server 実装 (#204 で nav2_bridge へ rename)と同じく行わない。理由は #149 round 10 のコメント
  //   (current map と不一致なら無視) が、map switch 中に InitialPoseRequest が config_path より
  //   先に到着する正当ケースを「stale」と誤判定する regression を生むため。現状は publisher 上書き
  //   (transient_local depth=1) で stale を排除する想定。
  RCLCPP_INFO(this->get_logger(),
    "Received initialpose POI '%s' for map '%s'.", poi_name.c_str(), msg->map_name.c_str());

  if (!this->pois_info_client_->wait_for_service(2s)) {
    RCLCPP_ERROR(this->get_logger(), "mapoi/get_pois_info service not available");
    return;
  }
  auto request = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();
  pois_info_client_->async_send_request(
    request,
    [this, poi_name](rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future) {
      auto result = future.get();
      if (!result) {
        RCLCPP_ERROR(this->get_logger(), "Failed to get POI info for initialpose.");
        return;
      }
      for (const auto & poi : result->pois_list) {
        if (poi.name == poi_name) {
          // landmark + initial_pose は排他 (#85)。explicit POI 指定でも reject する。
          if (has_landmark_tag(poi)) {
            RCLCPP_ERROR(this->get_logger(),
              "POI '%s' has 'landmark' tag; cannot be used as initial pose.",
              poi_name.c_str());
            return;
          }
          // subscriber readiness race (#152): publish_initial_pose 内で subscriber 0 を検知したら
          // async retry timer が起動するので、blocking wait は不要。
          publish_initial_pose(poi.pose, "POI '" + poi_name + "'");
          return;
        }
      }
      RCLCPP_WARN(this->get_logger(), "POI named '%s' not found!", poi_name.c_str());
    });
}

void MapoiAmclLocalizationBridge::publish_initial_pose(
  const geometry_msgs::msg::Pose & pose, const std::string & source)
{
  geometry_msgs::msg::PoseWithCovarianceStamped msg;
  msg.header.frame_id = this->get_parameter("map_frame").as_string();
  msg.header.stamp = this->now();
  msg.pose.pose = pose;
  msg.pose.covariance[0] = 0.25;
  msg.pose.covariance[7] = 0.25;
  msg.pose.covariance[35] = 0.06853891945200942;
  // 過去の retry が pending な状態で新 publish が来たら、必ず先に cancel する (#149 round 4 high)。
  // でないと「新 publish 後に古い pose を retry が再送 → localization が古い位置へ戻る」回帰になる。
  if (initialpose_retry_timer_) {
    initialpose_retry_timer_->cancel();
    initialpose_retry_timer_.reset();
  }

  initialpose_pub_->publish(msg);
  RCLCPP_INFO(this->get_logger(), "Published initial pose (%s).", source.c_str());

  // subscriber 未 ready の場合のみ async retry を起動 (#149 round 7 high)。
  // 「常時 retry」は subscriber visible but not ready 取りこぼし対策には有効だが、運用中
  // (AMCL fully ready) で `mapoi/initialpose_poi` が来た際に意図しない自己位置 jump を
  // 引き起こす副作用が大きい。グレーゾーン (起動直後の visible but not ready) は捨て、
  // 「count == 0 のみ retry」=「後起動 path のみ retry」のロバスト性を優先する。
  if (initialpose_pub_->get_subscription_count() == 0) {
    schedule_initialpose_retry(pose, source);
  }
}

void MapoiAmclLocalizationBridge::schedule_initialpose_retry(
  const geometry_msgs::msg::Pose & pose, const std::string & source)
{
  initialpose_retry_pose_ = pose;
  initialpose_retry_source_ = source;
  initialpose_retry_attempt_ = 0;
  initialpose_post_subscribe_republish_done_ = 0;
  if (initialpose_retry_timer_) {
    initialpose_retry_timer_->cancel();
    initialpose_retry_timer_.reset();
  }
  // パラメータの最低限の clamp (#149 round 4 low): 0 以下は default にフォールバック。
  // 過密タイマや無限 retry の防止。
  double interval_sec =
    this->get_parameter("initialpose_retry_interval_sec").as_double();
  if (interval_sec < 0.01) {
    RCLCPP_WARN(this->get_logger(),
      "initialpose_retry_interval_sec=%.3f is too small; clamping to 0.01.", interval_sec);
    interval_sec = 0.01;
  }
  int max_attempts =
    this->get_parameter("initialpose_retry_max_attempts").as_int();
  if (max_attempts < 1) {
    RCLCPP_WARN(this->get_logger(),
      "initialpose_retry_max_attempts=%d is invalid; clamping to 1.", max_attempts);
    max_attempts = 1;
  }
  initialpose_retry_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(static_cast<int>(interval_sec * 1000)),
    std::bind(&MapoiAmclLocalizationBridge::initialpose_retry_callback, this));
  const int post_n =
    this->get_parameter("initialpose_post_subscribe_republish_count").as_int();
  RCLCPP_INFO(this->get_logger(),
    "Scheduled initialpose retry/republish every %.2fs "
    "(max %d wait attempts; %d post-subscribe republish).",
    interval_sec, max_attempts, post_n);
}

void MapoiAmclLocalizationBridge::initialpose_retry_callback()
{
  if (initialpose_pub_->get_subscription_count() > 0) {
    // subscriber 検知後は N 回 republish する (#149 round 5 medium)。
    // AMCL が「subscriber visible でも処理 ready 直前」のケースで取りこぼすため。
    geometry_msgs::msg::PoseWithCovarianceStamped msg;
    msg.header.frame_id = this->get_parameter("map_frame").as_string();
    msg.header.stamp = this->now();
    msg.pose.pose = initialpose_retry_pose_;
    msg.pose.covariance[0] = 0.25;
    msg.pose.covariance[7] = 0.25;
    msg.pose.covariance[35] = 0.06853891945200942;
    initialpose_pub_->publish(msg);
    ++initialpose_post_subscribe_republish_done_;
    int post_n =
      this->get_parameter("initialpose_post_subscribe_republish_count").as_int();
    if (post_n < 1) post_n = 1;
    if (initialpose_post_subscribe_republish_done_ >= post_n) {
      RCLCPP_INFO(this->get_logger(),
        "initialpose subscriber detected; re-published %d times after %d retries (%s).",
        initialpose_post_subscribe_republish_done_, initialpose_retry_attempt_,
        initialpose_retry_source_.c_str());
      initialpose_retry_timer_->cancel();
      initialpose_retry_timer_.reset();
    }
    return;
  }
  ++initialpose_retry_attempt_;
  int max_attempts =
    this->get_parameter("initialpose_retry_max_attempts").as_int();
  if (max_attempts < 1) max_attempts = 1;
  if (initialpose_retry_attempt_ >= max_attempts) {
    RCLCPP_WARN(this->get_logger(),
      "initialpose subscriber not detected after %d retries; giving up "
      "(user can re-publish via WebUI/RViz/mapoi/initialpose_poi).",
      max_attempts);
    initialpose_retry_timer_->cancel();
    initialpose_retry_timer_.reset();
  }
}

void MapoiAmclLocalizationBridge::publish_backend_status()
{
  // localization bridge としての readiness を 1Hz polling で集約して publish する (#209)。
  // contract は minimal 3 フィールドだけ: bridge 実装者は backend_ready を真にするだけで
  // mapoi UI と統合できる。Per-capability の内訳が必要なら reason 文字列に詰める。
  // backend_ready の判定は「/initialpose subscriber 数 > 0」を採用。AMCL / slam_toolbox 等の
  // downstream localization が listening していることを最低限の ready proxy とする。
  // subscriber は居ても処理 ready 直前のケースは publish_initial_pose 側の retry timer で吸収する
  // (検知後 republish #149 round 5)。ここでは「subscriber 不在=明確に NG」を出すだけ。
  const bool subscriber_present =
    initialpose_pub_ && initialpose_pub_->get_subscription_count() > 0;

  mapoi_interfaces::msg::LocalizationBackendStatus msg;
  msg.backend_type = "amcl";
  msg.backend_ready = subscriber_present;
  if (!msg.backend_ready) {
    msg.reason = "no subscriber on " +
      this->get_parameter("initial_pose_topic").as_string() +
      " (localization node not running?)";
  }
  backend_status_pub_->publish(msg);
}

bool MapoiAmclLocalizationBridge::has_landmark_tag(
  const mapoi_interfaces::msg::PointOfInterest & poi)
{
  for (const auto & tag : poi.tags) {
    if (tag == "landmark") {
      return true;
    }
  }
  return false;
}

}  // namespace mapoi

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<mapoi::MapoiAmclLocalizationBridge>());
  rclcpp::shutdown();
  return 0;
}
