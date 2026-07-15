// UNIT_TEST マクロは CMakeLists.txt の target_compile_definitions で定義する
#include <cmath>
#include <limits>

#include <gtest/gtest.h>
#include <rclcpp/rclcpp.hpp>
#include "mapoi_server/mapoi_nav2_bridge.hpp"

class Nav2BridgeTestFixture : public ::testing::Test
{
protected:
  void SetUp() override
  {
    if (!rclcpp::ok()) {
      rclcpp::init(0, nullptr);
    }
    node_ = std::make_shared<MapoiNav2Bridge>();
  }
  void TearDown() override
  {
    node_.reset();
  }

  mapoi_interfaces::msg::PointOfInterest make_poi(
    const std::string & name, double x, double y,
    double tolerance_xy, const std::vector<std::string> & tags)
  {
    mapoi_interfaces::msg::PointOfInterest poi;
    poi.name = name;
    poi.pose.position.x = x;
    poi.pose.position.y = y;
    poi.tolerance.xy = tolerance_xy;
    poi.tolerance.yaw = 0.0;
    poi.tags = tags;
    return poi;
  }

  std::shared_ptr<MapoiNav2Bridge> node_;
};

TEST_F(Nav2BridgeTestFixture, DistanceCalculation)
{
  geometry_msgs::msg::Pose pose;
  pose.position.x = 3.0;
  pose.position.y = 4.0;
  double dist = node_->distance_2d(pose, 0.0, 0.0);
  EXPECT_DOUBLE_EQ(dist, 5.0);
}

TEST_F(Nav2BridgeTestFixture, DistanceCalculationZero)
{
  geometry_msgs::msg::Pose pose;
  pose.position.x = 1.0;
  pose.position.y = 2.0;
  double dist = node_->distance_2d(pose, 1.0, 2.0);
  EXPECT_DOUBLE_EQ(dist, 0.0);
}

// --- yaw_from_quaternion / angle_diff_abs (#261) ---

namespace
{
geometry_msgs::msg::Quaternion quat_from_yaw(double yaw)
{
  geometry_msgs::msg::Quaternion q;
  q.x = 0.0;
  q.y = 0.0;
  q.z = std::sin(yaw / 2.0);
  q.w = std::cos(yaw / 2.0);
  return q;
}
}  // namespace

TEST_F(Nav2BridgeTestFixture, YawFromQuaternionIdentity)
{
  geometry_msgs::msg::Quaternion q;
  q.w = 1.0;  // x=y=z=0
  EXPECT_NEAR(MapoiNav2Bridge::yaw_from_quaternion(q), 0.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, YawFromQuaternionHalfPi)
{
  EXPECT_NEAR(MapoiNav2Bridge::yaw_from_quaternion(quat_from_yaw(M_PI / 2.0)),
              M_PI / 2.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, YawFromQuaternionNegHalfPi)
{
  EXPECT_NEAR(MapoiNav2Bridge::yaw_from_quaternion(quat_from_yaw(-M_PI / 2.0)),
              -M_PI / 2.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, YawFromQuaternionPi)
{
  // ±pi は atan2 上どちらも有効。絶対値で pi に一致することを確認。
  EXPECT_NEAR(std::abs(MapoiNav2Bridge::yaw_from_quaternion(quat_from_yaw(M_PI))),
              M_PI, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, AngleDiffAbsZero)
{
  EXPECT_NEAR(MapoiNav2Bridge::angle_diff_abs(1.2, 1.2), 0.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, AngleDiffAbsHalfPi)
{
  EXPECT_NEAR(MapoiNav2Bridge::angle_diff_abs(M_PI / 2.0, 0.0), M_PI / 2.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, AngleDiffAbsWrapAround)
{
  // 3.0 と -3.0 の素朴な差は 6.0 だが、wrap すると ~0.283 が最短角。
  EXPECT_NEAR(MapoiNav2Bridge::angle_diff_abs(3.0, -3.0),
              2.0 * M_PI - 6.0, 1e-9);
}

TEST_F(Nav2BridgeTestFixture, AngleDiffAbsSymmetric)
{
  EXPECT_NEAR(MapoiNav2Bridge::angle_diff_abs(0.3, 1.1),
              MapoiNav2Bridge::angle_diff_abs(1.1, 0.3), 1e-9);
}

// --- is_within_arrival_tolerance (#265 統一到達判定の pure 化、launch_test 降格分) ---
// route 中間 waypoint / 最終 goal / 単発 Go の到達判定 (OR トリガ a) をここで直接検証する。
// 旧 launch_test (route_mapoi / goal_mapoi の radius+yaw 判定) の核をこの unit が代替する。
TEST_F(Nav2BridgeTestFixture, IsWithinArrivalToleranceXyAndYawInside)
{
  EXPECT_TRUE(MapoiNav2Bridge::is_within_arrival_tolerance(0.3, 0.1, 0.5, 0.785));
}

TEST_F(Nav2BridgeTestFixture, IsWithinArrivalToleranceYawOff)
{
  // xy 内だが yaw 外 → 未到達 (Nav2 SUCCEEDED = OR トリガ b を待つ)。
  EXPECT_FALSE(MapoiNav2Bridge::is_within_arrival_tolerance(0.3, M_PI, 0.5, 0.785));
}

TEST_F(Nav2BridgeTestFixture, IsWithinArrivalToleranceXyOff)
{
  // yaw 内だが xy 外 → 未到達。
  EXPECT_FALSE(MapoiNav2Bridge::is_within_arrival_tolerance(2.0, 0.0, 0.5, 0.785));
}

TEST_F(Nav2BridgeTestFixture, IsWithinArrivalToleranceBoundaryXy)
{
  // dist == tolerance.xy は到達 (<=)。
  EXPECT_TRUE(MapoiNav2Bridge::is_within_arrival_tolerance(0.5, 0.0, 0.5, 0.785));
}

TEST_F(Nav2BridgeTestFixture, IsWithinArrivalToleranceBoundaryYaw)
{
  // yaw_diff == tolerance.yaw は到達 (<=)。
  EXPECT_TRUE(MapoiNav2Bridge::is_within_arrival_tolerance(0.3, 0.785, 0.5, 0.785));
}

// --- classify_radius_transition (#220 ENTER/EXIT ヒステリシスの pure 化) ---
// 旧 launch_test (route_integration の ENTER/EXIT) の判定核をこの unit が代替する。
TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionEnter)
{
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(false, 0.3, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::ENTER);
}

TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionExitWithHysteresis)
{
  // 内→hysteresis 倍 (0.5*1.15=0.575) を超えて離脱 → EXIT。
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(true, 0.6, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::EXIT);
}

TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionStaysInsideHysteresisBand)
{
  // 内かつ tolerance.xy < dist <= tolerance.xy*hysteresis (0.5 < 0.55 <= 0.575) → NONE (ばたつき防止)。
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(true, 0.55, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::NONE);
}

TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionStaysOutside)
{
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(false, 1.0, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::NONE);
}

TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionEnterAtBoundary)
{
  // dist == tolerance.xy は ENTER (ENTER 条件は <=)。到達判定の境界 (<=) と対称。
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(false, 0.5, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::ENTER);
}

TEST_F(Nav2BridgeTestFixture, ClassifyRadiusTransitionExitAtBoundary)
{
  // dist == tolerance.xy * hysteresis (0.575) は EXIT ではなく NONE (EXIT 条件は厳密 >)。
  // 閾値ちょうどは内側維持 = ばたつき防止のヒステリシス境界を pin する。
  EXPECT_EQ(MapoiNav2Bridge::classify_radius_transition(true, 0.5 * 1.15, 0.5, 1.15),
            MapoiNav2Bridge::RadiusTransition::NONE);
}

TEST_F(Nav2BridgeTestFixture, RebuildEventPoisIncludesAllPois)
{
  {
    std::lock_guard<std::mutex> lock(node_->data_mutex_);
    node_->pois_list_.push_back(make_poi("goal_only", 1.0, 0.0, 0.5, {"waypoint"}));
    node_->pois_list_.push_back(make_poi("with_pause", 0.0, 2.0, 0.5, {"waypoint", "pause"}));
    node_->pois_list_.push_back(make_poi("with_custom", 3.0, 0.0, 0.5, {"waypoint", "audio_info"}));
    node_->pois_list_.push_back(make_poi("landmark_ref", 0.0, 0.0, 0.5, {"landmark"}));
  }
  node_->rebuild_event_pois();
  EXPECT_EQ(node_->event_pois_.size(), 4u);
}

TEST_F(Nav2BridgeTestFixture, RebuildEventPoisEmpty)
{
  node_->rebuild_event_pois();
  EXPECT_EQ(node_->event_pois_.size(), 0u);
}

TEST_F(Nav2BridgeTestFixture, PauseTagDetection)
{
  auto poi_pause = make_poi("pause_poi", 0.0, 0.0, 0.5, {"waypoint", "pause"});
  auto poi_no_pause = make_poi("normal_poi", 0.0, 0.0, 0.5, {"waypoint", "audio_info"});

  auto has_pause = [](const mapoi_interfaces::msg::PointOfInterest & p) {
    for (const auto & tag : p.tags) {
      if (tag == "pause") return true;
    }
    return false;
  };

  EXPECT_TRUE(has_pause(poi_pause));
  EXPECT_FALSE(has_pause(poi_no_pause));
}

// SelectInitialPosePois* tests は #144 で削除。
// `initial_pose` system tag を廃止し、initial pose の選定ロジックは mapoi_server::compute_initial_poi_name
// に移動した。新ロジックの test は mapoi_server 側に置く想定 (本 PR では未追加、follow-up #148 で扱う)。

TEST_F(Nav2BridgeTestFixture, HasLandmarkTagDetection)
{
  EXPECT_FALSE(MapoiNav2Bridge::has_landmark_tag(
    make_poi("goal_only", 0.0, 0.0, 0.5, {"waypoint"})));
  EXPECT_FALSE(MapoiNav2Bridge::has_landmark_tag(
    make_poi("no_tags", 0.0, 0.0, 0.5, {})));
  EXPECT_TRUE(MapoiNav2Bridge::has_landmark_tag(
    make_poi("landmark_only", 0.0, 0.0, 0.5, {"landmark"})));
  EXPECT_TRUE(MapoiNav2Bridge::has_landmark_tag(
    make_poi("landmark_combo", 0.0, 0.0, 0.5, {"landmark", "hazard"})));
}

// --- build_route_poi_names (#143 / #148) ---

TEST_F(Nav2BridgeTestFixture, BuildRoutePoiNamesWaypointsOnly)
{
  auto wp1 = make_poi("wp1", 0.0, 0.0, 0.5, {"waypoint"});
  auto wp2 = make_poi("wp2", 1.0, 0.0, 0.5, {"waypoint", "pause"});
  auto result = MapoiNav2Bridge::build_route_poi_names({wp1, wp2}, {});
  EXPECT_EQ(result.size(), 2u);
  EXPECT_EQ(result.count("wp1"), 1u);
  EXPECT_EQ(result.count("wp2"), 1u);
}

TEST_F(Nav2BridgeTestFixture, BuildRoutePoiNamesWaypointsAndLandmarks)
{
  // route 受信時、waypoints と landmarks の両方が active set に入る (#143)。
  auto wp = make_poi("wp1", 0.0, 0.0, 0.5, {"waypoint"});
  auto lm = make_poi("lm1", 1.0, 0.0, 0.5, {"landmark"});
  auto result = MapoiNav2Bridge::build_route_poi_names({wp}, {lm});
  EXPECT_EQ(result.size(), 2u);
  EXPECT_EQ(result.count("wp1"), 1u);
  EXPECT_EQ(result.count("lm1"), 1u);
}

TEST_F(Nav2BridgeTestFixture, BuildRoutePoiNamesLandmarksEmptyBackwardCompat)
{
  // 旧 yaml (route.landmarks 未指定) は landmarks empty で読まれる。waypoints のみで
  // active set が成立し、空にならない (#143 後方互換契約)。
  auto wp = make_poi("wp1", 0.0, 0.0, 0.5, {"waypoint"});
  auto result = MapoiNav2Bridge::build_route_poi_names({wp}, {});
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result.count("wp1"), 1u);
}

TEST_F(Nav2BridgeTestFixture, BuildRoutePoiNamesEmpty)
{
  auto result = MapoiNav2Bridge::build_route_poi_names({}, {});
  EXPECT_TRUE(result.empty());
}

TEST_F(Nav2BridgeTestFixture, BuildRoutePoiNamesDuplicateNames)
{
  // 同名が waypoint と landmark の両方に出てきても set 性質で 1 つに集約。
  auto wp = make_poi("dup", 0.0, 0.0, 0.5, {"waypoint"});
  auto lm = make_poi("dup", 1.0, 0.0, 0.5, {"landmark"});
  auto result = MapoiNav2Bridge::build_route_poi_names({wp}, {lm});
  EXPECT_EQ(result.size(), 1u);
  EXPECT_EQ(result.count("dup"), 1u);
}

// --- is_pause_eligible (#143 / #148) ---

TEST_F(Nav2BridgeTestFixture, IsPauseEligibleRouteModeActiveWithPauseTag)
{
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint", "pause"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_TRUE(MapoiNav2Bridge::is_pause_eligible(
    poi, MapoiNav2Bridge::NavMode::ROUTE, active));
}

TEST_F(Nav2BridgeTestFixture, IsPauseEligibleRouteModeActiveWithoutPauseTag)
{
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_FALSE(MapoiNav2Bridge::is_pause_eligible(
    poi, MapoiNav2Bridge::NavMode::ROUTE, active));
}

TEST_F(Nav2BridgeTestFixture, IsPauseEligibleRouteModeNonActivePoi)
{
  // pause タグはあるが active route POI ではない (= 別 route の POI に偶然 ENTER)。
  // 厳格化前は発火していたが #143 で発火しないようになった。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint", "pause"});
  std::unordered_set<std::string> active = {"p2"};
  EXPECT_FALSE(MapoiNav2Bridge::is_pause_eligible(
    poi, MapoiNav2Bridge::NavMode::ROUTE, active));
}

TEST_F(Nav2BridgeTestFixture, IsPauseEligibleGoalMode)
{
  // GOAL 走行中は pause タグ + active set 一致でも発火しない (#143)。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint", "pause"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_FALSE(MapoiNav2Bridge::is_pause_eligible(
    poi, MapoiNav2Bridge::NavMode::GOAL, active));
}

TEST_F(Nav2BridgeTestFixture, IsPauseEligibleIdleMode)
{
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint", "pause"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_FALSE(MapoiNav2Bridge::is_pause_eligible(
    poi, MapoiNav2Bridge::NavMode::IDLE, active));
}

// --- is_active_route_poi (#193, test_poi_event_integration の降格先) ---
// 非 ROUTE mode (IDLE/GOAL) では route 登録 POI に進入しても ENTER/EXIT/PAUSED の
// 発火対象にならない、という gate を pure 化して pin する。is_pause_eligible とは別述語
// (pause タグ条件を含まない superset) なので、ここで独立に検証する。pause タグの無い
// POI を使うことで「is_pause_eligible なら必ず false」のケースと区別できる。

TEST_F(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInIdleMode)
{
  // IDLE では active route POI のど真ん中でも event 発火対象にならない。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_FALSE(MapoiNav2Bridge::is_active_route_poi(
    poi, MapoiNav2Bridge::NavMode::IDLE, active));
}

TEST_F(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInGoalMode)
{
  // GOAL (単発 Go) でも route POI の event gate は開かない。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_FALSE(MapoiNav2Bridge::is_active_route_poi(
    poi, MapoiNav2Bridge::NavMode::GOAL, active));
}

TEST_F(Nav2BridgeTestFixture, IsActiveRoutePoiTrueInRouteModeListedPoi)
{
  // ROUTE 走行中 + active set に含まれる POI のみ発火対象 (pause タグは不要)。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint"});
  std::unordered_set<std::string> active = {"p1"};
  EXPECT_TRUE(MapoiNav2Bridge::is_active_route_poi(
    poi, MapoiNav2Bridge::NavMode::ROUTE, active));
}

TEST_F(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInRouteModeUnlistedPoi)
{
  // ROUTE 走行中でも active set 外の POI (別 route の POI 等) は発火対象外。
  auto poi = make_poi("p1", 0.0, 0.0, 0.5, {"waypoint"});
  std::unordered_set<std::string> active = {"p2"};
  EXPECT_FALSE(MapoiNav2Bridge::is_active_route_poi(
    poi, MapoiNav2Bridge::NavMode::ROUTE, active));
}

// --- reset_nav_state (#143 / #148) ---

TEST_F(Nav2BridgeTestFixture, ResetNavStateClearsRouteContext)
{
  // route 走行中 + pause 中に相当する状態を fixture から直接 set し、
  // reset_nav_state() が route lifecycle 終了時 (cancel / SUCCEEDED / ABORTED /
  // GOAL 切替) に行うクリーンアップ動作を再現する。`reset_nav_state()` の
  // 契約に含まれる全 member をまとめて検証することで、将来 reset 対象が漏れた
  // 場合を unit test で検出できる。
  {
    std::lock_guard<std::mutex> lock(node_->data_mutex_);
    node_->current_route_poi_names_.insert("wp1");
    node_->current_route_poi_names_.insert("lm1");
  }
  node_->nav_mode_ = MapoiNav2Bridge::NavMode::ROUTE;
  node_->is_paused_ = true;
  node_->current_waypoint_index_ = 3;

  geometry_msgs::msg::PoseStamped wp;
  wp.pose.position.x = 1.0;
  node_->current_route_waypoints_.push_back(wp);
  node_->paused_waypoints_.push_back(wp);

  geometry_msgs::msg::PoseStamped paused_goal;
  paused_goal.pose.position.x = 9.0;
  node_->paused_goal_pose_ = paused_goal;

  node_->reset_nav_state();

  EXPECT_TRUE(node_->current_route_poi_names_.empty());
  EXPECT_EQ(node_->nav_mode_, MapoiNav2Bridge::NavMode::IDLE);
  EXPECT_FALSE(node_->is_paused_);
  EXPECT_EQ(node_->current_waypoint_index_, 0u);
  EXPECT_TRUE(node_->current_route_waypoints_.empty());
  EXPECT_TRUE(node_->paused_waypoints_.empty());
  // paused_goal_pose_ は default-constructed PoseStamped に戻る (pose.position は 0)。
  EXPECT_DOUBLE_EQ(node_->paused_goal_pose_.pose.position.x, 0.0);
}

// (#220 で compute_stopped_transition / StoppedDetectionInputs / StoppedTransition を撤去。
//  EVENT_STOPPED / EVENT_RESUMED 自体が消え、PAUSED は cmd_vel dwell ベースの単純判定で
//  純関数 state machine 不要になったため、unit test 群も併せて削除した。)

// --- auto_resume_timeout_sec (#231) ---

TEST_F(Nav2BridgeTestFixture, AutoResumeTimeoutDefaultDisabled)
{
  // default では disabled (= 0.0)。負値以外の正値検証は launch_test / 結合 test に委ねる。
  EXPECT_DOUBLE_EQ(node_->auto_resume_timeout_sec_, 0.0);
}

TEST_F(Nav2BridgeTestFixture, AutoResumeTimeoutNegativeClampedToZero)
{
  // 負値は constructor で 0.0 に clamp する。RAII で別 node を作って検証する。
  rclcpp::NodeOptions options;
  options.append_parameter_override("auto_resume_timeout_sec", -1.5);
  auto node_with_negative = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_DOUBLE_EQ(node_with_negative->auto_resume_timeout_sec_, 0.0);
}

TEST_F(Nav2BridgeTestFixture, AutoResumeTimeoutNonFiniteClampedToZero)
{
  // NaN / Inf も constructor で 0.0 に clamp する (#231 / cursor review medium 対応)。
  // NaN は `< 0.0` でも `> 0.0` でもないため isfinite 込みで弾く必要がある。
  for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                     std::numeric_limits<double>::infinity(),
                     -std::numeric_limits<double>::infinity()}) {
    rclcpp::NodeOptions options;
    options.append_parameter_override("auto_resume_timeout_sec", bad);
    auto node_with_bad = std::make_shared<MapoiNav2Bridge>(options);
    EXPECT_DOUBLE_EQ(node_with_bad->auto_resume_timeout_sec_, 0.0)
      << "auto_resume_timeout_sec=" << bad << " should be clamped to 0.0";
  }
}

TEST_F(Nav2BridgeTestFixture, CancelAutoResumeTimerIsIdempotent)
{
  // 何も schedule していない状態で cancel を呼んでも安全 (二重 resume 経路で呼ばれる想定)。
  EXPECT_NO_THROW(node_->cancel_auto_resume_timer_());
  EXPECT_EQ(node_->auto_resume_timer_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ResetNavStateCancelsAutoResumeTimer)
{
  // reset_nav_state は pending auto-resume timer も明示的に破棄する契約 (#231)。
  // 直接 timer を生やして reset で消えることを確認する (実際の schedule は ROUTE mode 起点で
  // ros 時計が必要だが、ここでは契約のみを検証)。
  node_->auto_resume_timer_ = node_->create_wall_timer(
    std::chrono::seconds(60), []() {});
  node_->auto_resume_target_poi_ = "dummy_poi";
  EXPECT_NE(node_->auto_resume_timer_, nullptr);

  node_->reset_nav_state();

  EXPECT_EQ(node_->auto_resume_timer_, nullptr);
  EXPECT_TRUE(node_->auto_resume_target_poi_.empty());
}

// --- cmd_vel callback / TwistStamped 互換 (#249) ---
//
// jazzy で Nav2 が cmd_vel を TwistStamped で publish するように変わったため、
// 旧 Twist subscriber だけだと 1 通も受信できず EVENT_PAUSED が永久に発火しない
// silent regression が起きていた。両 callback が同じ zero-velocity 判定 helper
// (update_zero_velocity_state) を呼ぶ contract を pin する。

TEST_F(Nav2BridgeTestFixture, CmdVelTwistCallbackUpdatesZeroVelocityState)
{
  // Humble 系の Twist publisher: zero 入力で zero_velocity_active_ が true に遷移。
  EXPECT_FALSE(node_->zero_velocity_active_);
  auto msg = std::make_shared<geometry_msgs::msg::Twist>();
  msg->linear.x = 0.0;
  msg->linear.y = 0.0;
  msg->linear.z = 0.0;
  msg->angular.z = 0.0;
  node_->cmd_vel_callback(msg);
  EXPECT_TRUE(node_->zero_velocity_active_);
}

TEST_F(Nav2BridgeTestFixture, CmdVelTwistStampedCallbackUpdatesZeroVelocityState)
{
  // Jazzy 系の TwistStamped publisher: 内包 twist を unwrap して同じ判定を通す。
  EXPECT_FALSE(node_->zero_velocity_active_);
  auto msg = std::make_shared<geometry_msgs::msg::TwistStamped>();
  msg->twist.linear.x = 0.0;
  msg->twist.linear.y = 0.0;
  msg->twist.linear.z = 0.0;
  msg->twist.angular.z = 0.0;
  node_->cmd_vel_stamped_callback(msg);
  EXPECT_TRUE(node_->zero_velocity_active_);
}

TEST_F(Nav2BridgeTestFixture, CmdVelNonZeroClearsZeroVelocityState)
{
  // zero で active 化 → 非 zero で即 clear。Twist / TwistStamped どちらの経路でも同じ。
  auto zero = std::make_shared<geometry_msgs::msg::Twist>();
  node_->cmd_vel_callback(zero);
  EXPECT_TRUE(node_->zero_velocity_active_);

  auto moving = std::make_shared<geometry_msgs::msg::TwistStamped>();
  moving->twist.linear.x = 0.5;  // 閾値 0.01 を超える
  node_->cmd_vel_stamped_callback(moving);
  EXPECT_FALSE(node_->zero_velocity_active_);
}

TEST_F(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeExplicit)
{
  // 明示指定はそのまま透過。typo / 未知値は "twist" にフォールバック。
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("twist"), "twist");
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("twist_stamped"), "twist_stamped");
}

// **本 test binary は ROS_DISTRO を setenv/unsetenv で書き換える** ため、gtest を並列実行
// (`--gtest_parallel` / ctest `-j` で他 test と同居) すると env race の温床になる。現状の
// colcon 構成は逐次実行前提で問題ないが、将来 parallel に切替える場合は本 binary を
// シングル process に固定 (例: `RUN_SERIAL TRUE`) するか、ROS_DISTRO 依存テストを別 binary に
// 分割する必要がある (#252 round 2 review medium #3 メモ)。

// RAII guard for ROS_DISTRO env: テスト中の setenv/unsetenv が EXPECT 失敗 / 例外 / 早期 return で
// 復元漏れになり、後続テスト (特に "auto" 解決を含む test) へリークするのを防ぐ (#251 review medium)。
// destructor で必ず元値へ戻す。
class ScopedRosDistro
{
public:
  ScopedRosDistro()
  {
    const char * original = std::getenv("ROS_DISTRO");
    if (original != nullptr) {
      saved_ = original;
      was_set_ = true;
    }
  }
  ~ScopedRosDistro()
  {
    if (was_set_) {
      setenv("ROS_DISTRO", saved_.c_str(), 1);
    } else {
      unsetenv("ROS_DISTRO");
    }
  }
  ScopedRosDistro(const ScopedRosDistro &) = delete;
  ScopedRosDistro & operator=(const ScopedRosDistro &) = delete;

private:
  std::string saved_;
  bool was_set_ {false};
};

TEST_F(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeAutoByDistro)
{
  // "auto" は ROS_DISTRO 環境変数で決まる: humble → twist、それ以外 → twist_stamped。
  ScopedRosDistro distro_guard;

  setenv("ROS_DISTRO", "humble", 1);
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("auto"), "twist");

  setenv("ROS_DISTRO", "jazzy", 1);
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("auto"), "twist_stamped");

  setenv("ROS_DISTRO", "kilted", 1);
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("auto"), "twist_stamped");

  unsetenv("ROS_DISTRO");
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("auto"), "twist_stamped");
}

// Constructor の分岐自体 (declare_parameter → if (msg_type == "twist_stamped") { ... } else { ... })
// が壊れていないことを pin する (#251 follow-up)。resolve_cmd_vel_msg_type の純関数 test は
// 解決ロジックだけを見ているため、constructor で「解決後の型に対応する sub を 1 本だけ作る」
// 部分を独立に検証しないと、リファクタで if 分岐の typo / どちらかの create_subscription が
// 漏れても unit test が緑のまま通る余地が残る。
//
// 本 test 群が確認するのは「create_subscription が呼ばれて該当 member が non-null になる」
// ことのみで、subscription が実際に message を受信できるかは scope 外 (それは
// CmdVel*CallbackUpdatesZeroVelocityState / route_integration launch_test で別途 pin)。
//
// 同 process の fixture node が default 設定で `/cmd_vel` に sub を貼っているため、本 test 群は
// 必ず専用 `cmd_vel_topic` を割り当てる: 同 topic に違う型の sub を作ると rcl が
// `invalid allocator` で crash する (#249 lessons)。

TEST_F(Nav2BridgeTestFixture, ConstructorTwistStampedParamCreatesStampedSub)
{
  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("twist_stamped"));
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_stamped_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_stamped_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ConstructorTwistParamCreatesTwistSub)
{
  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("twist"));
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_twist_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_stamped_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ConstructorUnknownParamJazzyFallsBackToStampedSub)
{
  // 未知値 + ROS_DISTRO=jazzy: resolve_cmd_vel_msg_type 経由で "twist_stamped" に解決され
  // TwistStamped sub が wire される。caller 側の WARN log は ResolveCmdVelMsgTypeUnknownFallback
  // で pin 済なので、ここでは「fallback 後に正しい型の sub が wire されているか」だけを見る。
  ScopedRosDistro distro_guard;
  setenv("ROS_DISTRO", "jazzy", 1);

  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("Twist"));  // case-sensitive typo
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_unknown_jazzy_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_stamped_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ConstructorUnknownParamHumbleFallsBackToTwistSub)
{
  // 未知値 + ROS_DISTRO=humble: resolve_cmd_vel_msg_type 経由で "twist" に解決され Twist sub
  // が wire される。jazzy 側と同じく constructor の if/else 漏れを両 distro で潰す。
  ScopedRosDistro distro_guard;
  setenv("ROS_DISTRO", "humble", 1);

  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("twst"));  // typo
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_unknown_humble_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_stamped_sub_, nullptr);
}

// "auto" 自身の constructor 経路も両 distro で pin する (#252 round 2 review medium #1)。
// 純関数 ResolveCmdVelMsgTypeAutoByDistro は解決ロジックだけを見ているため、constructor 内で
// 「auto を渡したときに resolve 結果へ正しく分岐する」部分が壊れても (例: リファクタで auto
// 経路だけ解決結果を無視するような typo) 検知できない余地が残る。default param が "auto" で
// あることと合わせて、本番運用パスの回帰検知として両 distro 分を pin する。

TEST_F(Nav2BridgeTestFixture, ConstructorAutoParamJazzyCreatesStampedSub)
{
  ScopedRosDistro distro_guard;
  setenv("ROS_DISTRO", "jazzy", 1);

  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("auto"));
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_auto_jazzy_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_stamped_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ConstructorAutoParamHumbleCreatesTwistSub)
{
  ScopedRosDistro distro_guard;
  setenv("ROS_DISTRO", "humble", 1);

  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("auto"));
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_auto_humble_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_stamped_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ConstructorAutoParamUnsetDistroCreatesStampedSub)
{
  // ROS_DISTRO 未設定 + auto: README / launch description で「未設定 → twist_stamped」と運用上の
  // 重要ケースとして明記している経路。最小 CI / 自作 container 等で env が落ちた状態で
  // bridge を起動するシナリオの constructor 側を pin する (#252 round 3 review medium #1)。
  ScopedRosDistro distro_guard;
  unsetenv("ROS_DISTRO");

  rclcpp::NodeOptions options;
  options.append_parameter_override("cmd_vel_msg_type", std::string("auto"));
  options.append_parameter_override("cmd_vel_topic", std::string("test_cmd_vel_auto_unset_branch"));
  auto node = std::make_shared<MapoiNav2Bridge>(options);
  EXPECT_NE(node->cmd_vel_stamped_sub_, nullptr);
  EXPECT_EQ(node->cmd_vel_sub_, nullptr);
}

TEST_F(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeUnknownFallback)
{
  // 未知値 (typo / 設定ミス) は auto と同じく ROS_DISTRO ベースでフォールバック。
  // 旧仕様 (常に "twist") だと jazzy 本番で誤設定すると subscribe 不成立で
  // EVENT_PAUSED が再び silent に壊れる回帰につながるため、distro 適合型に揃える。
  // (caller 側は別途 WARN log で typo を可視化する、cursor review PR #250 medium #1)
  ScopedRosDistro distro_guard;

  setenv("ROS_DISTRO", "jazzy", 1);
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("twst"), "twist_stamped");
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type(""), "twist_stamped");
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("Twist"), "twist_stamped");  // case-sensitive

  setenv("ROS_DISTRO", "humble", 1);
  EXPECT_EQ(MapoiNav2Bridge::resolve_cmd_vel_msg_type("twst"), "twist");
}

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
