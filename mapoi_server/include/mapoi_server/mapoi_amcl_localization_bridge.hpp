#ifndef MAPOI_SERVER__MAPOI_AMCL_LOCALIZATION_BRIDGE_HPP_
#define MAPOI_SERVER__MAPOI_AMCL_LOCALIZATION_BRIDGE_HPP_

#include <chrono>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/pose.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>

#include "mapoi_interfaces/msg/initial_pose_request.hpp"
#include "mapoi_interfaces/msg/localization_backend_status.hpp"
#include "mapoi_interfaces/srv/get_pois_info.hpp"
#include "mapoi_interfaces/msg/point_of_interest.hpp"

namespace mapoi
{

// AMCL (or AMCL-compatible localization) 向け localization bridge (#209)。
// `mapoi/initialpose_poi` (transient_local) を sub して POI を resolve、
// `/initialpose` (`PoseWithCovarianceStamped`) に配信する単機能 node。
// 1Hz で `mapoi/localization/backend_status` を publish し、WebUI / RViz panel が
// Initial Pose 操作 UI を gate するための minimal contract を提供する。
//
// mapoi_nav2_bridge から AMCL adapter を分離した目的は #209 を参照: navigation backend と
// localization backend を独立した契約として扱い、将来 AMCL 以外 (slam_toolbox /
// 自前 localization 等) に切替えやすくする。
class MapoiAmclLocalizationBridge : public rclcpp::Node
{
public:
  explicit MapoiAmclLocalizationBridge(
    const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  // mapoi/initialpose_poi (transient_local) を受けて、POI list を fetch、
  // 該当 POI の pose を /initialpose に流す。
  rclcpp::Subscription<mapoi_interfaces::msg::InitialPoseRequest>::SharedPtr
    initialpose_poi_sub_;
  void initialpose_poi_callback(
    const mapoi_interfaces::msg::InitialPoseRequest::SharedPtr msg);

  // /initialpose (default) publisher。topic 名は `initial_pose_topic` parameter で変更可。
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr
    initialpose_pub_;

  rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr pois_info_client_;

  // /initialpose の単一エントリポイント。subscriber 不在なら async retry を起動する。
  void publish_initial_pose(
    const geometry_msgs::msg::Pose & pose, const std::string & source);

  // /initialpose subscriber (主に AMCL) が後起動した場合の retry 機構 (旧 mapoi_nav_server (#204 で rename) から移植、#152)。
  // single-thread executor で blocking wait は避ける必要があるため、wall timer ベースで polling。
  void schedule_initialpose_retry(
    const geometry_msgs::msg::Pose & pose, const std::string & source);
  void initialpose_retry_callback();
  rclcpp::TimerBase::SharedPtr initialpose_retry_timer_;
  geometry_msgs::msg::Pose initialpose_retry_pose_;
  std::string initialpose_retry_source_;
  int initialpose_retry_attempt_ {0};
  // subscriber 検知後の追加 republish カウント。AMCL が「subscriber visible だが処理 ready 直前」の
  // ケースで初回 publish を取りこぼすため、検知後も短いインターバルで N 回連続 publish する。
  int initialpose_post_subscribe_republish_done_ {0};

  // localization backend readiness publisher + 1Hz polling timer (#209)。
  // /initialpose subscriber 数 > 0 を「downstream localization が ready」proxy として採用。
  // contract は minimal 3 フィールド (backend_type / backend_ready / reason)。
  rclcpp::Publisher<mapoi_interfaces::msg::LocalizationBackendStatus>::SharedPtr
    backend_status_pub_;
  rclcpp::TimerBase::SharedPtr backend_status_timer_;
  void publish_backend_status();

  // landmark tag 判定 (#85)。landmark POI は initial pose 候補から除外する。
  static bool has_landmark_tag(const mapoi_interfaces::msg::PointOfInterest & poi);
};

}  // namespace mapoi

#endif  // MAPOI_SERVER__MAPOI_AMCL_LOCALIZATION_BRIDGE_HPP_
