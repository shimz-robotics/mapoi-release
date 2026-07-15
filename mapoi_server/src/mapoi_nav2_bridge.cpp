#include "mapoi_server/mapoi_nav2_bridge.hpp"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <cmath>
#include <thread>

using namespace std::chrono_literals;
using std::placeholders::_1;
using std::placeholders::_2;

MapoiNav2Bridge::MapoiNav2Bridge(const rclcpp::NodeOptions & options)
: Node("mapoi_nav2_bridge", options)
{
  this->get_logger().set_level(rclcpp::Logger::Level::Info);

  // #211: initialpose POI を直接 publish せず、唯一の writer である mapoi_server へ
  // request_initial_pose service 経由で依頼する。LoadMap 成功後 (on_select_map_received) という
  // timing gate は本ノードが所有し続け、wire-publish のみ mapoi_server に移す。これにより
  // transient_local の per-writer latched cache が単一化され stale POI を構造的に排除する。
  request_initial_pose_client_ = this->create_client<mapoi_interfaces::srv::RequestInitialPose>(
    "mapoi/request_initial_pose");

  // goal_pose subscriber and publisher
  mapoi_goal_pose_poi_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/goal_pose_poi", 1, std::bind(&MapoiNav2Bridge::mapoi_goal_pose_poi_cb, this, std::placeholders::_1));
  nav2_goal_pose_pub_ = this->create_publisher<geometry_msgs::msg::PoseStamped>("goal_pose", 1);

  // route subscriber
  mapoi_route_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/route", 1, std::bind(&MapoiNav2Bridge::mapoi_route_cb, this, std::placeholders::_1));

  // map switch subscriber
  mapoi_switch_map_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/switch_map", 1, std::bind(&MapoiNav2Bridge::mapoi_switch_map_cb, this, std::placeholders::_1));

  // cancel subscriber
  mapoi_cancel_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/cancel", 1, std::bind(&MapoiNav2Bridge::mapoi_cancel_cb, this, std::placeholders::_1));

  // pause / resume subscribers
  mapoi_pause_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/pause", 1, std::bind(&MapoiNav2Bridge::mapoi_pause_cb, this, _1));
  mapoi_resume_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/nav/resume", 1, std::bind(&MapoiNav2Bridge::mapoi_resume_cb, this, _1));

  this->action_client_ = rclcpp_action::create_client<FollowWaypoints>(this, "follow_waypoints");
  this->nav_to_pose_client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");

  // Nav status publisher
  // transient_local: 後起動 subscriber でも最後の状態を受信できる (mapoi/config_path と同 pattern)
  nav_status_pub_ = this->create_publisher<std_msgs::msg::String>(
    "mapoi/nav/status", rclcpp::QoS(1).transient_local());

  // Command-rejected イベント通知 publisher (#354)。status は状態 snapshot、こちらは
  // 「直近のコマンドが拒否された」という単発イベントなので、volatile (デフォルト、
  // 非 transient_local) QoS で depth を少し持たせる。後起動 subscriber へのリプレイは
  // 不要 (イベントは「今」しか意味を持たない) なので transient_local にはしない。
  command_rejected_pub_ = this->create_publisher<std_msgs::msg::String>(
    "mapoi/nav/command_rejected", rclcpp::QoS(10));

  // Navigation backend readiness publisher + 1Hz polling timer (#198)。
  // Nav2 action / service の存在は実行時に変化し得る (Nav2 を後起動 / 落とすケース) ため、
  // event-driven ではなく polling で publish する割り切り。1Hz は WebUI 表示の応答性として十分。
  // QoS は NavigationBackendStatus.msg の contract に従う (#208):
  // transient_local + MANUAL_BY_TOPIC liveliness + 5s lease。各 publish() が assert を兼ねる
  // ので 1Hz timer が止まれば 5s 後に subscriber 側 Liveliness Changed event が発火する。
  backend_status_pub_ = this->create_publisher<mapoi_interfaces::msg::NavigationBackendStatus>(
    "mapoi/nav/backend_status",
    rclcpp::QoS(1)
      .transient_local()
      .liveliness(rclcpp::LivelinessPolicy::ManualByTopic)
      .liveliness_lease_duration(5s));
  // 独立 MutuallyExclusive callback_group + MultiThreadedExecutor (main) で backend_status
  // timer を他 callback と独立 thread で動かす (#213)。`select_map_callback` 内の
  // `wait_for_service(10s)` / `spin_until_future_complete` で default group の thread が最大
  // ~11s blocking する間も 1Hz publish が継続するため、5s lease を超える false-positive
  // lost が発火しなくなる。Reentrant ではなく独立 MutuallyExclusive にする理由 (#214 cursor
  // review medium): backend_status timer は callback が単一 (`publish_backend_status` のみ)
  // で、必要なのは「default group の thread と並列に動く」こと。Reentrant は同一 callback の
  // self-overlap を許す性質で、将来 callback が長時間化した時の race risk を生む。MutuallyExclusive
  // 独立 group なら他 callback と並列実行されつつ self-overlap は起きない。`publish_backend_status`
  // は read-only な `*_is_ready()` checks + thread-safe な `Publisher::publish()` のみで member
  // 書き込みなしのため、他 callback (default group で serialize) との data race も無い。
  backend_status_callback_group_ = this->create_callback_group(
    rclcpp::CallbackGroupType::MutuallyExclusive);
  backend_status_timer_ = this->create_wall_timer(
    1s, std::bind(&MapoiNav2Bridge::publish_backend_status, this),
    backend_status_callback_group_);

  this->pois_info_client_ = this->create_client<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info");
  this->route_client_ = this->create_client<mapoi_interfaces::srv::GetRoutePois>("mapoi/get_route_pois");
  this->select_map_client_ = this->create_client<mapoi_interfaces::srv::SelectMap>("mapoi/select_map");

  // --- POI radius event detection ---
  this->declare_parameter<double>("tolerance_check_hz", 5.0);
  this->declare_parameter<double>("hysteresis_exit_multiplier", 1.15);
  this->declare_parameter<std::string>("map_frame", "map");
  this->declare_parameter<std::string>("base_frame", "base_link");
  // EVENT_PAUSED 判定 (#220 で旧 STOPPED/RESUMED から spec 変更): 線速ノルムと角速度絶対値
  // の両方が ``stopped_speed_threshold`` 未満の状態が ``stopped_dwell_time_sec`` 続いたら、
  // pause タグ付き route POI 内に居る場合のみ EVENT_PAUSED publish。線速 (m/s) と角速 (rad/s)
  // に同一閾値を使う割り切りで「その場旋回も停止扱いしない」用途を表現する。
  this->declare_parameter<double>("stopped_speed_threshold", 0.01);
  this->declare_parameter<double>("stopped_dwell_time_sec", 1.0);
  this->declare_parameter<std::string>("cmd_vel_topic", "cmd_vel");
  // EVENT_PAUSED 発火後の auto-resume timeout (#231)。default 0.0 = disabled で現行仕様 (無限待ち) を維持。
  // 正値で demo / 自動運転シナリオ向けの opt-in auto-resume を有効化。負値は invalid として 0.0 に clamp。
  // 動的 reconfigure 非対応 (起動時 1 回 read してキャッシュ)。
  this->declare_parameter<double>("auto_resume_timeout_sec", 0.0);
  // パラメータを cmd_vel_callback / tolerance_check_callback の hot path で毎回取得すると
  // lock コストが高いため、起動時にキャッシュして以降は member を読む (#176 review low #4)。
  stopped_speed_threshold_ = this->get_parameter("stopped_speed_threshold").as_double();
  stopped_dwell_time_sec_ = this->get_parameter("stopped_dwell_time_sec").as_double();
  const double auto_resume_param = this->get_parameter("auto_resume_timeout_sec").as_double();
  // 負値 / NaN / Inf は全て invalid として 0.0 に clamp する (#231)。
  // NaN は `< 0.0` でも `> 0.0` でもないため `std::isfinite` の組合せが必要。
  // 通常運用では override ミス以外で混入しないが、混入時に timer 周期が異常になる
  // (NaN: schedule_auto_resume_ で常に no-op、Inf: chrono cast でオーバーフロー) のを
  // 起動時に潰す。
  if (!std::isfinite(auto_resume_param) || auto_resume_param < 0.0) {
    RCLCPP_ERROR(this->get_logger(),
      "auto_resume_timeout_sec=%f is invalid (must be finite and non-negative); "
      "clamping to 0.0 (auto-resume disabled).",
      auto_resume_param);
    auto_resume_timeout_sec_ = 0.0;
  } else {
    auto_resume_timeout_sec_ = auto_resume_param;
  }

  // waypoint 到達モード (#243)。"nav2" (既定) は従来の FollowWaypoints 任せ、"mapoi" は
  // mapoi が tolerance.xy 到達判定で 1 waypoint ずつ NavigateToPose を送って進める。
  // 起動時 1 回 read してキャッシュ (動的 reconfigure 非対応)。未知値は "nav2" にフォールバック。
  this->declare_parameter<std::string>("waypoint_arrival_mode", "nav2");
  const std::string arrival_mode_param =
    this->get_parameter("waypoint_arrival_mode").as_string();
  if (arrival_mode_param == "nav2" || arrival_mode_param == "mapoi") {
    waypoint_arrival_mode_ = arrival_mode_param;
  } else {
    RCLCPP_WARN(this->get_logger(),
      "waypoint_arrival_mode='%s' は未知の値です。'nav2' にフォールバックしました。"
      " 有効値: 'nav2' / 'mapoi'",
      arrival_mode_param.c_str());
    waypoint_arrival_mode_ = "nav2";
  }
  RCLCPP_INFO(this->get_logger(),
    "waypoint_arrival_mode = '%s'", waypoint_arrival_mode_.c_str());

  tf_buffer_ = std::make_shared<tf2_ros::Buffer>(this->get_clock());
  tf_listener_ = std::make_shared<tf2_ros::TransformListener>(*tf_buffer_);

  poi_event_pub_ = this->create_publisher<mapoi_interfaces::msg::PoiEvent>("mapoi/events", 10);

  config_path_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/config_path", rclcpp::QoS(1).transient_local(),
    std::bind(&MapoiNav2Bridge::on_config_path_changed, this, _1));

  // cmd_vel subscribe (#140 / #249): STOPPED 判定 source。QoS は Nav2 publisher と同じ
  // default (reliable, depth=10)。distro 移行で publisher 型が変わるため subscription 型を
  // 選択する (同じ topic に 2 種類の sub を作ると rcl が invalid allocator で crash する
  // ため、必ず片方のみ)。
  this->declare_parameter<std::string>("cmd_vel_msg_type", "auto");
  const std::string cmd_vel_topic = this->get_parameter("cmd_vel_topic").as_string();
  const std::string cmd_vel_msg_type_param =
    this->get_parameter("cmd_vel_msg_type").as_string();
  const std::string cmd_vel_msg_type = resolve_cmd_vel_msg_type(cmd_vel_msg_type_param);
  // 既知の 3 値 (twist / twist_stamped / auto) 以外は WARN: 黙って distro fallback すると
  // typo を見逃す。Cursor review #250 medium #1 対応。
  if (cmd_vel_msg_type_param != "twist"
      && cmd_vel_msg_type_param != "twist_stamped"
      && cmd_vel_msg_type_param != "auto") {
    RCLCPP_WARN(this->get_logger(),
      "cmd_vel_msg_type='%s' は未知の値です。ROS_DISTRO ベースで '%s' にフォールバックしました。"
      " 有効値: 'twist' / 'twist_stamped' / 'auto'",
      cmd_vel_msg_type_param.c_str(), cmd_vel_msg_type.c_str());
  }
  if (cmd_vel_msg_type == "twist_stamped") {
    cmd_vel_stamped_sub_ = this->create_subscription<geometry_msgs::msg::TwistStamped>(
      cmd_vel_topic, 10,
      std::bind(&MapoiNav2Bridge::cmd_vel_stamped_callback, this, _1));
    RCLCPP_INFO(this->get_logger(),
      "cmd_vel subscribed as TwistStamped (msg_type=%s)", cmd_vel_msg_type.c_str());
  } else {
    // "twist" / 未知の値は安全側で Twist にフォールバック (humble 互換)。
    cmd_vel_sub_ = this->create_subscription<geometry_msgs::msg::Twist>(
      cmd_vel_topic, 10,
      std::bind(&MapoiNav2Bridge::cmd_vel_callback, this, _1));
    RCLCPP_INFO(this->get_logger(),
      "cmd_vel subscribed as Twist (msg_type=%s)", cmd_vel_msg_type.c_str());
  }

  tag_defs_client_ = this->create_client<mapoi_interfaces::srv::GetTagDefinitions>("mapoi/get_tag_definitions");
  fetch_system_tags();

  double hz = this->get_parameter("tolerance_check_hz").as_double();
  auto period = std::chrono::duration<double>(1.0 / hz);
  tolerance_check_timer_ = this->create_wall_timer(
    std::chrono::duration_cast<std::chrono::nanoseconds>(period),
    std::bind(&MapoiNav2Bridge::tolerance_check_callback, this));

  RCLCPP_INFO(this->get_logger(), "MapoiNav2Bridge initialized.");
}

void MapoiNav2Bridge::get_pois_list(){
  while(!this->pois_info_client_->wait_for_service(1s)) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for pois_info service. Exiting.");
      return;
    }
    RCLCPP_INFO(this->get_logger(), "pois_info service not available, waiting again...");
  }
  auto pois_info_request = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();
  pois_info_client_->async_send_request(
    pois_info_request, std::bind(&MapoiNav2Bridge::on_pois_info_received, this, _1));
}

void MapoiNav2Bridge::on_pois_info_received(rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future)
{
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "Failed to get Tagged POIs.");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    pois_list_ = result->pois_list;
  }
  RCLCPP_INFO(this->get_logger(), "Received %zu Tagged POIs.", pois_list_.size());
  rebuild_event_pois();

  // config_path 由来の fetch (on_config_path_changed → get_pois_list) が成功した時のみ guard を確定。
  // start_sequence 経由の初回 fetch では pending_guard_active_ が false で、ここは no-op。
  // fetch 失敗 (result == nullptr) で早期 return した場合も pending を残し、次回 publish で retry する。
  // (#144 で auto_publish_initial_pose は廃止、initial pose は mapoi_server が
  // mapoi/initialpose_poi topic に publish し、mapoi_amcl_localization_bridge 経由で
  // /initialpose に配信される (#209 で mapoi_nav2_bridge から AMCL adapter を分離)。)
  if (pending_guard_active_) {
    last_config_path_ = pending_config_path_;
    last_config_mtime_ = pending_config_mtime_;
    pending_guard_active_ = false;
  }
}

bool MapoiNav2Bridge::is_pause_eligible(
  const mapoi_interfaces::msg::PointOfInterest & poi,
  NavMode nav_mode,
  const std::unordered_set<std::string> & active_route_poi_names)
{
  if (nav_mode != NavMode::ROUTE) {
    return false;
  }
  if (active_route_poi_names.count(poi.name) == 0) {
    return false;
  }
  for (const auto & tag : poi.tags) {
    if (tag == "pause") {
      return true;
    }
  }
  return false;
}

bool MapoiNav2Bridge::is_active_route_poi(
  const mapoi_interfaces::msg::PointOfInterest & poi,
  NavMode nav_mode,
  const std::unordered_set<std::string> & active_route_poi_names)
{
  if (nav_mode != NavMode::ROUTE) {
    return false;
  }
  return active_route_poi_names.count(poi.name) != 0;
}

void MapoiNav2Bridge::publish_nav_status(const std::string & status, const std::string & target)
{
  std_msgs::msg::String msg;
  // target 空なら "status" のみ、有りなら "status:target" 形式で送る (#104)。
  // subscriber 側 (mapoi_panel / mapoi_webui_node) は : split で target を復元。
  // 後方互換: 旧 "status" 単独 payload しか読まない subscriber は target 部を
  // 読まないだけで従来通り動く。
  msg.data = target.empty() ? status : status + ':' + target;
  nav_status_pub_->publish(msg);
}

void MapoiNav2Bridge::publish_rejected_status(const std::string & target)
{
  // #354: status (状態 snapshot) とは別軸のイベント通知。nav_mode_ による suppress は
  // 下の status 側ロジックにのみ適用し、command_rejected は常に publish する — 走行中の
  // typo goal 等でも操作者が気づけるようにするのが目的 (#339 の "rejected" status
  // suppress の副作用で、走行中の reject に気づく手段が無かった課題への対応)。
  // payload は reject 対象の target のみ、reason は従来通り ERROR/WARN ログに残す。
  std_msgs::msg::String rejected_msg;
  rejected_msg.data = target;
  command_rejected_pub_->publish(rejected_msg);

  // #339: 「受理する前に無効と判定して実行しなかった」新規コマンド (存在しない POI 名、
  // landmark POI を goal 指定、get_pois_info 未 ready、route 取得失敗、空 route 等) を
  // "rejected" で通知する。ただし進行中の navigation (nav_mode_ != IDLE) がある間は publish
  // しない: この reject は新規コマンドの入力検証失敗であって進行中の navigation には一切
  // 影響しない (state・action は未変更のまま走行を継続する) ため、"rejected" で "navigating"
  // / "paused" / "map_switching" 等の実際の走行状態を上書きすると、ロボットは走行を継続して
  // いるのに UI が停止したかのように誤表示される (Cursor review PR #352 round 2 high 指摘)。
  // 走行中は従来通り ERROR/WARN ログのみに留め、status は実際の走行状態を優先する
  // (command_rejected イベント通知は上で常時 publish 済み、#354)。
  if (nav_mode_ != NavMode::IDLE) {
    return;
  }
  publish_nav_status("rejected", target);
}

void MapoiNav2Bridge::mapoi_cancel_cb(const std_msgs::msg::String::SharedPtr msg)
{
  (void)msg;
  bool canceled = false;
  if (current_goal_handle_) {
    RCLCPP_INFO(this->get_logger(), "Canceling FollowWaypoints goal...");
    action_client_->async_cancel_goal(current_goal_handle_);
    canceled = true;
  }
  if (current_ntp_goal_handle_) {
    RCLCPP_INFO(this->get_logger(), "Canceling NavigateToPose goal...");
    nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    canceled = true;
  }
  if (!canceled) {
    RCLCPP_WARN(this->get_logger(), "No active navigation goal to cancel.");
  }
  reset_nav_state();
}

void MapoiNav2Bridge::reset_nav_state()
{
  nav_mode_ = NavMode::IDLE;
  is_paused_ = false;
  paused_goal_pose_ = geometry_msgs::msg::PoseStamped{};
  current_route_waypoints_.clear();
  paused_waypoints_.clear();
  current_waypoint_index_ = 0;
  // mapoi 主導モードの route 状態も clear (#243)。
  current_route_pois_.clear();
  current_route_name_.clear();
  // active route POI set / per-POI 状態をここで clear (#143 / #220)。
  // route 終端 / cancel / failure 等で route が終わったあと、次回 route mode 開始時に
  // 「既に inside / paused 状態」のまま開始されて ENTER / PAUSED が発火しない、
  // または lifecycle 順序が崩れる、を防ぐ。
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    current_route_poi_names_.clear();
    poi_inside_state_.clear();
    poi_paused_published_.clear();
  }
  // route が終わる経路 (cancel / SUCCEEDED / ABORTED / GOAL 切替 / 新 route 投入) では
  // pending auto-resume timer も必ず破棄する (#231)。残すと別 nav 進行中に古い timer が
  // 満了して mapoi_resume_cb を呼んでしまう。
  cancel_auto_resume_timer_();
}

void MapoiNav2Bridge::cancel_auto_resume_timer_()
{
  if (auto_resume_timer_) {
    auto_resume_timer_->cancel();
    auto_resume_timer_.reset();
    auto_resume_target_poi_.clear();
  }
}

void MapoiNav2Bridge::schedule_auto_resume_(const std::string & poi_name)
{
  // EVENT_PAUSED 発火直後の入口 (#231): default disabled (0.0) の場合は no-op。
  if (auto_resume_timeout_sec_ <= 0.0) {
    return;
  }
  // 連続して pause タグ POI が並ぶ route で前の timer が生きている場合は
  // 上書きする (新しい PAUSED に従う)。idempotent な cancel を経由するので二重 cancel
  // しても安全。
  cancel_auto_resume_timer_();
  auto_resume_target_poi_ = poi_name;
  const auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::duration<double>(auto_resume_timeout_sec_));
  // create_wall_timer は default callback group (mapoi_pause_cb / mapoi_resume_cb /
  // tolerance_check_callback と同 MutuallyExclusive) に紐付くため、callback 実行は他の
  // nav state 操作と直列化される (race free)。
  auto_resume_timer_ = this->create_wall_timer(period, [this, poi_name]() {
    // 自己 cancel: one-shot として扱う。先に timer を cancel/reset しないと callback 内で
    // 再度 schedule_auto_resume_ → cancel_auto_resume_timer_ を呼んだ際に「自身を cancel
    // する」経路が複雑になるため、ここで明示的に切り離す。
    if (auto_resume_timer_) {
      auto_resume_timer_->cancel();
      auto_resume_timer_.reset();
    }
    auto_resume_target_poi_.clear();
    // 既に外部 resume / route 切替 / new pause で paused 状態が降りていれば何もしない。
    if (!is_paused_) {
      RCLCPP_DEBUG(this->get_logger(),
        "Auto-resume timer fired for POI '%s' but already resumed; ignoring.",
        poi_name.c_str());
      return;
    }
    RCLCPP_INFO(this->get_logger(),
      "Auto-resuming after timeout (%.2fs) at POI '%s'.",
      auto_resume_timeout_sec_, poi_name.c_str());
    auto trigger = std::make_shared<std_msgs::msg::String>();
    trigger->data = "auto_resume_timeout:" + poi_name;
    mapoi_resume_cb(trigger);
  });
}

void MapoiNav2Bridge::mapoi_pause_cb(const std_msgs::msg::String::SharedPtr msg)
{
  (void)msg;

  if (is_paused_) {
    RCLCPP_WARN(this->get_logger(), "Already paused — ignoring mapoi/nav/pause.");
    return;
  }

  if (nav_mode_ == NavMode::GOAL && current_ntp_goal_handle_) {
    RCLCPP_INFO(this->get_logger(), "Pausing NavigateToPose goal.");
    nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    is_paused_ = true;
    publish_nav_status("paused", current_target_name_);

  } else if (nav_mode_ == NavMode::ROUTE && waypoint_arrival_mode_ == "mapoi") {
    // mapoi 主導モード (#243): 進行中の NavigateToPose waypoint goal を cancel して停止。
    // FollowWaypoints の paused_waypoints_ slice は使わず、resume は index++ で次 waypoint を
    // 送る。current_ntp_goal_handle_ は Nav2 SUCCEEDED 経由 pause で既に null のこともある。
    RCLCPP_INFO(this->get_logger(),
      "Pausing mapoi-driven route at waypoint[%u].", current_waypoint_index_);
    if (current_ntp_goal_handle_) {
      nav_to_pose_client_->async_cancel_goal(current_ntp_goal_handle_);
    }
    is_paused_ = true;
    // mapoi モードの status target は route 名で統一する (waypoint goal の acceptance
    // タイミングに依存する current_target_name_ ではなく current_route_name_ を使う)。
    publish_nav_status("paused", current_route_name_);

  } else if (nav_mode_ == NavMode::ROUTE && current_goal_handle_) {
    uint32_t from = std::min(
      current_waypoint_index_,
      static_cast<uint32_t>(current_route_waypoints_.size()));
    paused_waypoints_ = std::vector<geometry_msgs::msg::PoseStamped>(
      current_route_waypoints_.begin() + from,
      current_route_waypoints_.end());
    RCLCPP_INFO(this->get_logger(),
      "Pausing FollowWaypoints: saving %zu remaining waypoints (from index %u).",
      paused_waypoints_.size(), from);
    action_client_->async_cancel_goal(current_goal_handle_);
    is_paused_ = true;
    publish_nav_status("paused", current_target_name_);

  } else {
    RCLCPP_WARN(this->get_logger(), "mapoi/nav/pause received but no active navigation.");
  }
}

void MapoiNav2Bridge::mapoi_resume_cb(const std_msgs::msg::String::SharedPtr msg)
{
  (void)msg;

  if (!is_paused_) {
    RCLCPP_WARN(this->get_logger(), "Not paused — ignoring mapoi/nav/resume.");
    // Not paused でも timer が残るパスは無いはずだが、念のため idempotent に cancel する
    // (#231 二重 resume 防止)。
    cancel_auto_resume_timer_();
    return;
  }
  // 外部 resume publish が auto-resume timer 満了より先に来た場合は pending timer を捨てる
  // (#231)。同 callback group で直列化されるので、timer callback と本 callback の race は
  // 発生しない。
  cancel_auto_resume_timer_();
  is_paused_ = false;

  // Resume の readiness check も即時判定に統一 (#198 review high): blocking wait は
  // single-thread executor で他 callback を止めるため避ける。1Hz backend_status timer で
  // readiness は維持されており、operator は ready 表示を見て resume を押す前提。
  if (nav_mode_ == NavMode::GOAL) {
    if (!nav_to_pose_client_->action_server_is_ready()) {
      RCLCPP_ERROR(this->get_logger(), "NavigateToPose action server not available for resume.");
      is_paused_ = true;
      publish_nav_status("backend_unavailable", current_target_name_);
      return;
    }
    RCLCPP_INFO(this->get_logger(), "Resuming NavigateToPose goal.");

    auto goal_msg = NavigateToPose::Goal();
    goal_msg.pose = paused_goal_pose_;

    auto send_goal_options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
    // resume: pause 直前と同じ active nav の継続だが、resume 自体は別 action attempt なので
    // generation を進めて pre-pause cancel result を stale 化する (Codex review #147 round 3 high)。
    // 新 navigation が割り込んできた場合の stale 判定もこの increment で同時に成立する。
    const size_t resumed_generation = ++nav_attempt_generation_;
    send_goal_options.goal_response_callback =
      [this, target = current_target_name_, resumed_generation](const GoalHandleNavigateToPose::SharedPtr & h) {
        this->ntp_goal_response_callback(target, resumed_generation, h);
      };
    send_goal_options.result_callback =
      [this, target = current_target_name_, resumed_generation](const GoalHandleNavigateToPose::WrappedResult & r) {
        this->ntp_result_callback(target, resumed_generation, r);
      };

    nav_to_pose_client_->async_send_goal(goal_msg, send_goal_options);

  } else if (nav_mode_ == NavMode::ROUTE && waypoint_arrival_mode_ == "mapoi") {
    // mapoi 主導モード (#243): pause は「現 waypoint に到達して停止」なので、resume は
    // 次 waypoint へ進める (index++ → send)。最終 waypoint で pause していた場合は index が
    // 末尾を越え、send_current_waypoint_goal_ が route succeeded を publish する。
    if (!nav_to_pose_client_->action_server_is_ready()) {
      RCLCPP_ERROR(this->get_logger(), "NavigateToPose action server not available for resume.");
      is_paused_ = true;
      publish_nav_status("backend_unavailable", current_route_name_);
      return;
    }
    RCLCPP_INFO(this->get_logger(),
      "Resuming mapoi-driven route: advancing from waypoint[%u].", current_waypoint_index_);
    ++current_waypoint_index_;
    send_current_waypoint_goal_();

  } else if (nav_mode_ == NavMode::ROUTE) {
    if (paused_waypoints_.empty()) {
      RCLCPP_ERROR(this->get_logger(), "Cannot resume: no saved waypoints.");
      return;
    }
    if (!action_client_->action_server_is_ready()) {
      RCLCPP_ERROR(this->get_logger(), "FollowWaypoints action server not available for resume.");
      is_paused_ = true;
      publish_nav_status("backend_unavailable", current_target_name_);
      return;
    }
    current_route_waypoints_ = paused_waypoints_;
    paused_waypoints_.clear();
    current_waypoint_index_ = 0;

    RCLCPP_INFO(this->get_logger(),
      "Resuming FollowWaypoints with %zu remaining waypoints.", current_route_waypoints_.size());

    auto goal_msg = FollowWaypoints::Goal();
    goal_msg.poses = current_route_waypoints_;

    auto send_goal_options = rclcpp_action::Client<FollowWaypoints>::SendGoalOptions();
    // resume: pause 直前と同じ active nav の継続だが、resume 自体は別 action attempt なので
    // generation を進めて pre-pause cancel result を stale 化する (Codex review #147 round 3 high)。
    const size_t resumed_generation = ++nav_attempt_generation_;
    send_goal_options.goal_response_callback =
      [this, target = current_target_name_, resumed_generation](const GoalHandleFollowWaypoints::SharedPtr & h) {
        this->goal_response_callback(target, resumed_generation, h);
      };
    send_goal_options.feedback_callback =
      [this, resumed_generation](GoalHandleFollowWaypoints::SharedPtr h,
                                  const std::shared_ptr<const FollowWaypoints::Feedback> f) {
        this->feedback_callback(resumed_generation, h, f);
      };
    send_goal_options.result_callback =
      [this, target = current_target_name_, resumed_generation](
        const GoalHandleFollowWaypoints::WrappedResult & r) {
        this->result_callback(target, resumed_generation, r);
      };

    action_client_->async_send_goal(goal_msg, send_goal_options);

  } else {
    RCLCPP_WARN(this->get_logger(), "mapoi/nav/resume: inconsistent state (paused but IDLE).");
  }
}

// --- POI radius event detection methods ---

void MapoiNav2Bridge::fetch_system_tags()
{
  if (!tag_defs_client_->service_is_ready()) {
    RCLCPP_INFO(this->get_logger(), "mapoi/get_tag_definitions service not available yet, waiting...");
    return;  // tolerance_check_callback will retry via guard check
  }
  auto request = std::make_shared<mapoi_interfaces::srv::GetTagDefinitions::Request>();
  tag_defs_client_->async_send_request(
    request, std::bind(&MapoiNav2Bridge::on_system_tags_received, this, _1));
}

void MapoiNav2Bridge::on_system_tags_received(
  rclcpp::Client<mapoi_interfaces::srv::GetTagDefinitions>::SharedFuture future)
{
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "Failed to get tag definitions.");
    return;
  }
  system_tags_.clear();
  for (const auto & def : result->definitions) {
    if (def.is_system) {
      system_tags_.insert(def.name);
    }
  }
  system_tags_loaded_ = true;
  RCLCPP_INFO(this->get_logger(), "Loaded %zu system tags for POI event filtering.", system_tags_.size());
  rebuild_event_pois();
  // 起動時に POI リストを取得（mapoi/config_path QoS 不一致のフォールバック）
  get_pois_list();
}

void MapoiNav2Bridge::on_config_path_changed(const std_msgs::msg::String::SharedPtr msg)
{
  // mapoi_server は config path 文字列を周期 publish (default 5s) する。path だけで dedup すると
  // map switch (path 変更) は拾えるが、WebUI/Panel Save (path 不変、内容のみ変更) を取りこぼし、
  // event_pois_ が古いまま radius event 監視が外れる。YAML mtime も併せて比較する (PR #78 と同 pattern)。
  const std::string & current_path = msg->data;
  std::filesystem::file_time_type current_mtime{};
  std::error_code ec;
  auto stat_mtime = std::filesystem::last_write_time(current_path, ec);
  if (!ec) {
    current_mtime = stat_mtime;
    if (current_path == last_config_path_ && current_mtime == last_config_mtime_) {
      return;  // 周期 publish (path も内容も不変) → skip
    }
  }
  // stat 失敗時は dedup 不能とみなし refresh を試みる (起動 race などで一時的に発生し得る)

  // guard は fetch 成功時 (on_pois_info_received) に確定。失敗時は pending のまま、
  // 次回 publish (周期 5s) で path/mtime が同じでも guard が前回値のままなので retry される。
  pending_config_path_ = current_path;
  pending_config_mtime_ = current_mtime;
  pending_guard_active_ = true;

  RCLCPP_INFO(this->get_logger(), "Map config changed: %s — refreshing POI list.", current_path.c_str());
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    poi_inside_state_.clear();
    event_pois_.clear();
  }
  get_pois_list();
}

void MapoiNav2Bridge::rebuild_event_pois()
{
  std::lock_guard<std::mutex> lock(data_mutex_);
  event_pois_ = pois_list_;
  RCLCPP_INFO(this->get_logger(), "Monitoring %zu POIs for radius events.", event_pois_.size());
}

void MapoiNav2Bridge::tolerance_check_callback()
{
  if (!system_tags_loaded_) {
    fetch_system_tags();
    return;
  }

  std::string map_frame = this->get_parameter("map_frame").as_string();
  std::string base_frame = this->get_parameter("base_frame").as_string();

  geometry_msgs::msg::TransformStamped transform;
  try {
    transform = tf_buffer_->lookupTransform(map_frame, base_frame, tf2::TimePointZero);
  } catch (const tf2::TransformException & ex) {
    // TF not yet available — silently skip
    return;
  }

  double rx = transform.transform.translation.x;
  double ry = transform.transform.translation.y;
  // GOAL 到達の yaw 一致判定用 (#261)。robot 現在 yaw を tick 先頭で 1 回取り出す。
  double r_yaw = yaw_from_quaternion(transform.transform.rotation);
  double hysteresis = this->get_parameter("hysteresis_exit_multiplier").as_double();
  // PAUSED 判定 (#220): zero_velocity が dwell threshold 以上続いたら停止扱い。
  // 全 POI 共通の判定なので timer tick 内で 1 回計算して使い回す。
  bool zero_velocity_dwelled = false;
  if (zero_velocity_active_) {
    const double dwell = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - last_zero_velocity_start_).count();
    zero_velocity_dwelled = (dwell >= stopped_dwell_time_sec_);
  }

  // pause タグ POI の ENTER を収集（mutex 外で auto-pause trigger するため）
  std::string pause_triggered_poi;
  // PAUSED 発火 POI 名 (今 tick で publish したもの)。lock 外で auto-resume timer を
  // schedule するため (#231)。1 tick 内で複数 POI が同時に PAUSED 化することは pause タグ
  // 重なり等で理論上ありうるが、最後に publish した POI に従う (cancel 上書き含め
  // schedule_auto_resume_ が idempotent に動く)。
  std::string paused_event_poi;
  // 統一到達判定 (#265): mapoi モードでは route waypoint / 単発 Go ともに到達 =
  // OR((tolerance.xy ∧ tolerance.yaw) ∨ Nav2 SUCCEEDED)。ここは OR トリガ a (xy ∧ yaw) を
  // レベル判定 (毎 tick) で見る。lock 外で on_waypoint_reached_ / on_goal_radius_arrival_ を
  // 呼ぶための flag。route mode と GOAL mode は nav_mode_ で排他なので同時には立たない。
  bool route_waypoint_arrival = false;

  // GOAL モード (#261/#265): 単発 Go (mapoi モード) の POI 個別 tolerance.xy + yaw 到達判定。
  // active な NavigateToPose goal がある時のみ対象 (current_ntp_goal_handle_ 非 null = acceptance
  // 済 = current_target_name_ が goal POI 名で確定済)。topic fallback (action 不在) では handle が
  // null のままなので対象外。nav state member は default callback group で本 callback と直列化される
  // ため lock なしで読める (data_mutex_ は pois_list_/event_pois_/poi_inside_state_ を守る用)。
  const bool goal_mode =
    (waypoint_arrival_mode_ == "mapoi" && nav_mode_ == NavMode::GOAL &&
     !is_paused_ && current_ntp_goal_handle_ != nullptr);
  const std::string goal_target_name = goal_mode ? current_target_name_ : std::string();
  // lock 外で on_goal_radius_arrival_ (cancel + succeeded) を呼ぶための flag。
  bool goal_radius_yaw_arrival = false;

  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    if (event_pois_.empty()) {
      return;
    }

    // mapoi 主導モードの現 target waypoint 名を tick 先頭で snapshot (#265)。最終 goal も
    // 含め全 waypoint が同じ (tolerance.xy ∧ tolerance.yaw) ∨ Nav2 SUCCEEDED で進むので、
    // 旧来の「末尾だけ radius 除外」特別扱いは廃止した。
    // !is_paused_ を含める: arrival は level 判定 (毎 tick) なので、pause 中に (xy ∧ yaw) を
    // 満たすと on_waypoint_reached_ が誤って次へ進めてしまう (特に非 pause waypoint で外部
    // pause された場合)。GOAL の goal_mode と同じく paused 中は arrival を抑止する。
    const bool mapoi_mode =
      (waypoint_arrival_mode_ == "mapoi" && nav_mode_ == NavMode::ROUTE && !is_paused_);
    std::string mapoi_target_wp_name;
    if (mapoi_mode && current_waypoint_index_ < current_route_pois_.size()) {
      mapoi_target_wp_name = current_route_pois_[current_waypoint_index_].name;
    }

    // EVENT_ENTER / EVENT_PAUSED / EVENT_EXIT は route 走行中 + route 登録 POI のみが
    // publish 対象 (#220)。route 走行外 (IDLE / GOAL mode) や non-route POI への侵入は通知
    // しない。lifecycle invariant: ENTER → [PAUSED] → EXIT を守るため、is_route_poi で
    // ない POI の inside/paused state は **書き換えない** (旧 build / route 外で勝手に内部
    // 状態が進むと、ROUTE 開始時に既に inside=true で ENTER が出ない / PAUSED が
    // ENTER を経ずに出る等の lifecycle 違反になる)。

    for (const auto & poi : event_pois_) {
      double dist = distance_2d(poi.pose, rx, ry);

      // 統一到達判定 (OR トリガ a: tolerance.xy ∧ tolerance.yaw) は route lifecycle
      // (ENTER/EXIT/PAUSED) とは独立に、現 target POI に対してレベル判定する (#261/#265)。
      // 姿勢が tolerance.yaw 内に収まっていれば即到達、収まっていなければ false のまま →
      // Nav2 の姿勢合わせ (SUCCEEDED, OR トリガ b) を待つ。radius 進入後に旋回して yaw が
      // 合うケースも毎 tick 再評価で拾える (edge ではなく level)。poi_inside_state_ 等の
      // lifecycle state は触らない。
      // GOAL モード (単発 Go)。
      if (goal_mode && !goal_radius_yaw_arrival && poi.name == goal_target_name) {
        const double yaw_diff =
          angle_diff_abs(r_yaw, yaw_from_quaternion(poi.pose.orientation));
        if (is_within_arrival_tolerance(dist, yaw_diff, poi.tolerance.xy, poi.tolerance.yaw)) {
          goal_radius_yaw_arrival = true;
        }
      }
      // ROUTE モード (現 target waypoint、最終 goal 含む #265)。
      if (mapoi_mode && !route_waypoint_arrival && poi.name == mapoi_target_wp_name) {
        const double yaw_diff =
          angle_diff_abs(r_yaw, yaw_from_quaternion(poi.pose.orientation));
        if (is_within_arrival_tolerance(dist, yaw_diff, poi.tolerance.xy, poi.tolerance.yaw)) {
          route_waypoint_arrival = true;
        }
      }

      // route 登録 POI かつ ROUTE mode 走行中なら event 発火対象 (#220)。
      // is_pause_eligible は同じ前提 (ROUTE mode + active route POI) + pause タグの
      // 3 条件、is_active_route_poi はそこから tag check を除いた superset (#193 で pure 化)。
      // invariant: !is_route_poi → !is_pause_poi (is_pause_poi は is_route_poi の subset)。
      const bool is_pause_poi = is_pause_eligible(poi, nav_mode_, current_route_poi_names_);
      const bool is_route_poi = is_active_route_poi(poi, nav_mode_, current_route_poi_names_);

      if (!is_route_poi) {
        // route 外 POI / route 走行外 mode では state を書き換えない (lifecycle 保護)。
        // pause 自動 trigger も is_pause_eligible で false なので発火しない。
        continue;
      }

      bool was_inside = poi_inside_state_[poi.name];
      const RadiusTransition transition =
        classify_radius_transition(was_inside, dist, poi.tolerance.xy, hysteresis);

      if (transition == RadiusTransition::ENTER) {
        // ENTER event
        poi_inside_state_[poi.name] = true;
        mapoi_interfaces::msg::PoiEvent event;
        event.event_type = mapoi_interfaces::msg::PoiEvent::EVENT_ENTER;
        event.poi = poi;
        event.stamp = this->now();
        poi_event_pub_->publish(event);
        RCLCPP_INFO(this->get_logger(), "POI ENTER: %s (dist=%.2f, tolerance.xy=%.2f)",
                    poi.name.c_str(), dist, poi.tolerance.xy);
        // pause タグがあれば自動 pause 対象。ただし nav2 モードのみ ENTER (radius 進入) で
        // 発火する (#265)。mapoi モードでは pause も統一到達 (tolerance.xy ∧ tolerance.yaw)
        // で判定したいので、on_waypoint_reached_ 経由 (route_waypoint_arrival or Nav2 SUCCEEDED)
        // で pause を起こす。nav2 モードは bridge が passive observer で on_waypoint_reached_ を
        // 呼ばないため、ここで pause を起こさないと止められない。
        if (is_pause_poi && waypoint_arrival_mode_ != "mapoi") {
          pause_triggered_poi = poi.name;
        }
      } else if (transition == RadiusTransition::EXIT) {
        // EXIT event
        poi_inside_state_[poi.name] = false;
        // PAUSED 発火済 flag も EXIT で reset (#220 lifecycle: ENTER → [PAUSED] → EXIT)
        poi_paused_published_[poi.name] = false;
        mapoi_interfaces::msg::PoiEvent event;
        event.event_type = mapoi_interfaces::msg::PoiEvent::EVENT_EXIT;
        event.poi = poi;
        event.stamp = this->now();
        poi_event_pub_->publish(event);
        RCLCPP_INFO(this->get_logger(), "POI EXIT: %s (dist=%.2f, tolerance.xy=%.2f)",
                    poi.name.c_str(), dist, poi.tolerance.xy);
        continue;  // PAUSED 判定不要 (今 tick で OUTSIDE 化)
      }

      // PAUSED 判定 (#220): inside かつ pause タグ持ち かつ cmd_vel dwell かつ未発火
      if (!poi_inside_state_[poi.name]) continue;
      if (!is_pause_poi) continue;
      if (!zero_velocity_dwelled) continue;
      if (poi_paused_published_[poi.name]) continue;
      poi_paused_published_[poi.name] = true;
      mapoi_interfaces::msg::PoiEvent event;
      event.event_type = mapoi_interfaces::msg::PoiEvent::EVENT_PAUSED;
      event.poi = poi;
      event.stamp = this->now();
      poi_event_pub_->publish(event);
      RCLCPP_INFO(this->get_logger(), "POI PAUSED: %s", poi.name.c_str());
      paused_event_poi = poi.name;
    }
  }  // data_mutex_ をここで解放

  // pause タグ POI の ENTER → 自動 pause (is_pause_poi 判定で route + ROUTE mode を確認済)
  if (!pause_triggered_poi.empty() && !is_paused_) {
    RCLCPP_INFO(this->get_logger(),
      "Auto-pausing route navigation: entered pause POI '%s'", pause_triggered_poi.c_str());
    auto msg = std::make_shared<std_msgs::msg::String>();
    msg->data = "poi_event:" + pause_triggered_poi;
    mapoi_pause_cb(msg);
  }

  // EVENT_PAUSED 発火 POI に対する auto-resume timer 起動 (#231)。
  // schedule_auto_resume_ は disabled (timeout=0.0) なら no-op、前 timer が生きてれば
  // cancel して上書きする。is_paused_ チェックは callback 側 (発火時) に任せ、
  // ここではあくまで「PAUSED が出た」ことだけを契機にする (auto-pause 経路で
  // mapoi_pause_cb が直前に呼ばれて is_paused_=true になっている前提)。
  if (!paused_event_poi.empty()) {
    schedule_auto_resume_(paused_event_poi);
  }

  // mapoi 主導モード (#243): 現 waypoint の tolerance.xy 到達で次 waypoint へ進む。
  // pause トリガの後に置く: pause waypoint の場合は上で is_paused_ が立ち、
  // on_waypoint_reached_ は pause 判定で進めず resume を待つ (= 二重に進めない)。
  // 統一到達 (#265): route 現 waypoint が (tolerance.xy ∧ tolerance.yaw) に到達 → 次へ進む
  // (pause waypoint なら on_waypoint_reached_ が pause を起こす)。
  if (route_waypoint_arrival) {
    on_waypoint_reached_();
  }

  // GOAL モード (#261): radius + yaw 到達で単発 Go を完了する。route mode とは nav_mode_ で
  // 排他なので route_waypoint_arrival とは同時に立たない。
  if (goal_radius_yaw_arrival) {
    on_goal_radius_arrival_();
  }
}

double MapoiNav2Bridge::distance_2d(const geometry_msgs::msg::Pose & poi_pose, double rx, double ry)
{
  double dx = poi_pose.position.x - rx;
  double dy = poi_pose.position.y - ry;
  return std::sqrt(dx * dx + dy * dy);
}

double MapoiNav2Bridge::yaw_from_quaternion(const geometry_msgs::msg::Quaternion & q)
{
  // ZYX 分解の yaw 成分 (tf2::getYaw と同等)。正規化されていない quaternion でも
  // atan2 の比なので向きは正しく出る。
  const double siny_cosp = 2.0 * (q.w * q.z + q.x * q.y);
  const double cosy_cosp = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
  return std::atan2(siny_cosp, cosy_cosp);
}

double MapoiNav2Bridge::angle_diff_abs(double a, double b)
{
  // [-pi, pi] に正規化した差の絶対値。atan2(sin(d), cos(d)) で wrap-around を吸収する
  // (例: a=3.0, b=-3.0 の差は 6.0 ではなく ~0.283)。
  const double d = a - b;
  return std::abs(std::atan2(std::sin(d), std::cos(d)));
}

bool MapoiNav2Bridge::is_within_arrival_tolerance(
  double dist, double yaw_diff, double tolerance_xy, double tolerance_yaw)
{
  // 統一到達判定 (#265 OR トリガ a): 距離が tolerance.xy 内、かつ yaw 差が tolerance.yaw 内。
  // route 中間 waypoint / 最終 goal / 単発 Go すべて同一式 (#265 で末尾特別扱いを廃止)。
  return dist <= tolerance_xy && yaw_diff <= tolerance_yaw;
}

MapoiNav2Bridge::RadiusTransition MapoiNav2Bridge::classify_radius_transition(
  bool was_inside, double dist, double tolerance_xy, double hysteresis_multiplier)
{
  // ENTER: 外→radius 進入。EXIT: 内→radius を hysteresis 倍超で離脱 (ばたつき防止)。
  // hysteresis band (tolerance.xy < dist <= tolerance.xy * hysteresis) では内側維持 = NONE。
  if (!was_inside && dist <= tolerance_xy) {
    return RadiusTransition::ENTER;
  }
  if (was_inside && dist > tolerance_xy * hysteresis_multiplier) {
    return RadiusTransition::EXIT;
  }
  return RadiusTransition::NONE;
}

// --- STOPPED / RESUMED 判定 (#140) ---

void MapoiNav2Bridge::cmd_vel_callback(const geometry_msgs::msg::Twist::SharedPtr msg)
{
  update_zero_velocity_state(
    msg->linear.x, msg->linear.y, msg->linear.z, msg->angular.z);
}

void MapoiNav2Bridge::cmd_vel_stamped_callback(
  const geometry_msgs::msg::TwistStamped::SharedPtr msg)
{
  update_zero_velocity_state(
    msg->twist.linear.x, msg->twist.linear.y, msg->twist.linear.z, msg->twist.angular.z);
}

std::string MapoiNav2Bridge::resolve_cmd_vel_msg_type(const std::string & param_value)
{
  if (param_value == "twist" || param_value == "twist_stamped") {
    return param_value;
  }
  // "auto" / 未知値はどちらも ROS_DISTRO ベースで自動判定する (#249 cursor review medium #1)。
  // 未知値を無条件 "twist" にフォールバックすると jazzy 以降の本番で再び silent に
  // subscribe 不成立 → EVENT_PAUSED 不発火の bug が復活する。auto と同じ判定にして
  // 「不一致だが distro 適合型を選んだ」という意図に揃える (WARN log は caller 側で出す)。
  const char * distro = std::getenv("ROS_DISTRO");
  if (distro && std::string(distro) == "humble") {
    return "twist";
  }
  // jazzy / kilted / それ以降は TwistStamped。env が unset の場合も今後の主流に倣う。
  return "twist_stamped";
}

void MapoiNav2Bridge::update_zero_velocity_state(
  double linear_x, double linear_y, double linear_z, double angular_z)
{
  // 線速ノルム (3D) と角速度絶対値 (yaw 軸) の両方が閾値以下のとき「停止」とみなす。
  // 撮影シナリオでは「その場旋回も停止扱いしない」前提のため、angular.z も判定に含める。
  // 単位が異なるが同一閾値を使う割り切り (param 説明参照、#140)。
  const double linear_norm = std::sqrt(
    linear_x * linear_x + linear_y * linear_y + linear_z * linear_z);
  const double angular_norm = std::abs(angular_z);
  const bool zero_now =
    (linear_norm < stopped_speed_threshold_) && (angular_norm < stopped_speed_threshold_);
  if (zero_now) {
    if (!zero_velocity_active_) {
      zero_velocity_active_ = true;
      last_zero_velocity_start_ = std::chrono::steady_clock::now();
    }
  } else {
    zero_velocity_active_ = false;
  }
}

#ifndef UNIT_TEST
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MapoiNav2Bridge>();
  // MultiThreadedExecutor で spin (#213): backend_status timer の Reentrant callback_group が
  // 別 thread で動くようにする。thread 数は明示的に 2 を指定する: default の
  // `std::thread::hardware_concurrency()` は Docker CPU 制限環境などで 1/0 を返し得るため、
  // 1 thread に縮退すると本 PR の修正 (blocking 中も timer を回す) が成立しない (#214 codex
  // review medium)。backend_status timer (Reentrant) + 他 callback (default MutuallyExclusive)
  // で独立 thread が 2 本あれば足りるので、上限を絞ることでリソースも節約できる。
  rclcpp::executors::MultiThreadedExecutor exec(rclcpp::ExecutorOptions(), 2);
  exec.add_node(node);
  exec.spin();
  rclcpp::shutdown();
  return 0;
}
#endif
