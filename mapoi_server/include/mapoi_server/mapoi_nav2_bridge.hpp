#ifndef MAPOI_SERVER__MAPOI_NAV_SERVER_HPP_
#define MAPOI_SERVER__MAPOI_NAV_SERVER_HPP_

#ifdef UNIT_TEST
#include <gtest/gtest.h>
#endif

#include <chrono>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <mutex>
#include <filesystem>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <geometry_msgs/msg/quaternion.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/twist_stamped.hpp>
#include <nav2_msgs/action/follow_waypoints.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav2_msgs/srv/load_map.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/pose_stamped.hpp>
#include <tf2_ros/buffer.h>
#include <tf2_ros/transform_listener.h>

#include "mapoi_interfaces/srv/get_pois_info.hpp"
#include "mapoi_interfaces/srv/get_route_pois.hpp"
#include "mapoi_interfaces/srv/select_map.hpp"
#include "mapoi_interfaces/srv/get_tag_definitions.hpp"
#include "mapoi_interfaces/msg/point_of_interest.hpp"
#include "mapoi_interfaces/msg/poi_event.hpp"
#include "mapoi_interfaces/srv/request_initial_pose.hpp"
#include "mapoi_interfaces/msg/navigation_backend_status.hpp"

class MapoiNav2Bridge : public rclcpp::Node
{
public:
  using FollowWaypoints = nav2_msgs::action::FollowWaypoints;
  using GoalHandleFollowWaypoints = rclcpp_action::ClientGoalHandle<FollowWaypoints>;
  using NavigateToPose = nav2_msgs::action::NavigateToPose;
  using GoalHandleNavigateToPose = rclcpp_action::ClientGoalHandle<NavigateToPose>;

  enum class NavMode { IDLE, GOAL, ROUTE };

// POI radius のヒステリシス遷移 (#220)。classify_radius_transition の戻り値。
enum class RadiusTransition { NONE, ENTER, EXIT };

  explicit MapoiNav2Bridge(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

private:
  // --- publisher & subscriber ---
  // mapoi/initialpose_poi の publisher は LoadMap 完了 trigger 用に残す (#209)。
  // sub と /initialpose 配信は mapoi_amcl_localization_bridge に移設済。
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_goal_pose_poi_sub_;
  rclcpp::Publisher<geometry_msgs::msg::PoseStamped>::SharedPtr nav2_goal_pose_pub_;

  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_route_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_switch_map_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_cancel_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_pause_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr mapoi_resume_sub_;

  void mapoi_goal_pose_poi_cb(const std_msgs::msg::String::SharedPtr msg);
  void mapoi_route_cb(const std_msgs::msg::String::SharedPtr msg);
  void mapoi_switch_map_cb(const std_msgs::msg::String::SharedPtr msg);
  void mapoi_cancel_cb(const std_msgs::msg::String::SharedPtr msg);
  void mapoi_pause_cb(const std_msgs::msg::String::SharedPtr msg);
  void mapoi_resume_cb(const std_msgs::msg::String::SharedPtr msg);

  // Service Callbacks
  void on_pois_info_received(rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future);
  void on_select_map_received(
    std::string map_name,
    rclcpp::Client<mapoi_interfaces::srv::SelectMap>::SharedFuture future);
  // route_name は #104 で current_target_name_ を action send 直前に更新するために
  // mapoi_route_cb から bind 経由で渡す。リクエスト時点で代入すると concurrent
  // request で active nav の target が汚染されるため、実際の send_goal 直前まで
  // 遅延させる。
  void on_route_received(std::string route_name,
                         rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedFuture future);

  // --- mapoi 主導 waypoint 到達モードの実行ヘルパ (#243) ---
  // current_route_pois_[current_waypoint_index_] へ NavigateToPose を送る。index が
  // 末尾を越えていれば route succeeded として終端処理する。NavigateToPose 不在なら
  // backend_unavailable で route を放棄。generation を増分して旧 callback を stale 化する。
  void send_current_waypoint_goal_();
  // 現 waypoint への到達確定時の遷移 (OR トリガ a: tolerance.xy 進入 / b: Nav2 SUCCEEDED 共通)。
  //   - pause タグ POI: auto-pause を発火し進めない (resume で次へ)。
  //   - それ以外: active goal を cancel (進入トリガ時) → index++ → 次 waypoint 送信。
  void on_waypoint_reached_();

  // GOAL モード (#261): 単発 Go が POI 個別 tolerance.xy + yaw に到達した時の完了処理。
  // mapoi モードでのみ tolerance_check_callback から呼ばれる。進行中の NavigateToPose goal を
  // cancel し、generation を進めて cancel result を stale 化したうえで "succeeded" を publish する。
  void on_goal_radius_arrival_();

  void get_pois_list();

  // Action Callbacks (FollowWaypoints — routes)
  // target は #104 race fix のため goal 固有の target を bind 経由で受け取る。
  // callback 内で publish_nav_status はこの引数を使い、共有 current_target_name_
  // を読まない (concurrent request で別 nav の target が混入するのを防ぐ)。
  // current_target_name_ は acceptance 時に更新し、pause 等で active nav の
  // target として参照される用途のみ。
  // nav_attempt_generation: navigation 開始 (route or GOAL) ごとに増分。各 callback で stale 判定に
  // 使い、旧 navigation の遅延 callback が新 navigation の global state (current_goal_handle_ /
  // current_route_poi_names_ / current_target_name_ 等) を上書き / clear しないようにする
  // (Codex review #147 round 2 high 対応)。
  void goal_response_callback(std::string target, size_t nav_generation,
                                const GoalHandleFollowWaypoints::SharedPtr & goal_handle);
  void feedback_callback(size_t nav_generation, GoalHandleFollowWaypoints::SharedPtr,
                          const std::shared_ptr<const FollowWaypoints::Feedback> feedback);
  void result_callback(std::string target, size_t nav_generation,
                       const GoalHandleFollowWaypoints::WrappedResult & result);

  // Action Callbacks (NavigateToPose — single POI)
  void ntp_goal_response_callback(std::string target, size_t nav_generation,
                                    const GoalHandleNavigateToPose::SharedPtr & goal_handle);
  void ntp_result_callback(std::string target, size_t nav_generation,
                            const GoalHandleNavigateToPose::WrappedResult & result);

  // Clients
  rclcpp_action::Client<FollowWaypoints>::SharedPtr action_client_;
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav_to_pose_client_;
  GoalHandleFollowWaypoints::SharedPtr current_goal_handle_;
  GoalHandleNavigateToPose::SharedPtr current_ntp_goal_handle_;
  rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr pois_info_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedPtr route_client_;
  rclcpp::Client<mapoi_interfaces::srv::SelectMap>::SharedPtr select_map_client_;

  // --- Pause / Resume state ---
  NavMode nav_mode_ = NavMode::IDLE;
  bool is_paused_ = false;
  geometry_msgs::msg::PoseStamped paused_goal_pose_;
  std::vector<geometry_msgs::msg::PoseStamped> current_route_waypoints_;
  uint32_t current_waypoint_index_ = 0;
  std::vector<geometry_msgs::msg::PoseStamped> paused_waypoints_;

  // mapoi 主導 waypoint 到達モード (#243)。route 実行を Nav2 FollowWaypoints 任せ
  // ("nav2", 既定) にするか、mapoi が tolerance.xy 到達判定で 1 waypoint ずつ
  // NavigateToPose を送って進める ("mapoi") かを切り替える。constructor で resolve。
  //   - "nav2":  従来挙動。Nav2 が xy_goal_tolerance で waypoint 進行、mapoi は radius observer。
  //   - "mapoi": 到達 = OR((tolerance.xy ∧ tolerance.yaw) ∨ NavigateToPose SUCCEEDED) で
  //              統一 (#265)。route 中間 waypoint / 最終 goal / 単発 Go (GOAL モード) すべて
  //              同じ判定式。姿勢が tolerance.yaw 内に自然に収まっていれば Nav2 完走を待たず
  //              即到達/前進 (snappy)、ずれていれば Nav2 の姿勢合わせ (SUCCEEDED) を待つ。
  //              「yaw を見るか」は per-POI tolerance.yaw で表現 (通過点は大きく ≒ yaw 不問、
  //              厳密点は小さく)。tolerance.xy < xy_goal_tolerance も可能 (POI を小さくできる)。
  //              "nav2" モードでは route は Nav2 FollowWaypoints の xy_goal_tolerance 任せ、
  //              Go も Nav2 goal_checker 任せ (mapoi は radius observer)。
  std::string waypoint_arrival_mode_ {"nav2"};
  // mapoi モードで進行中の route waypoint POI 列 (順序保持、landmarks は含まない)。
  // current_waypoint_index_ が指す POI へ NavigateToPose を送る。
  std::vector<mapoi_interfaces::msg::PointOfInterest> current_route_pois_;
  // mapoi モードの現 route 名 (status publish / resume の再送 target に使う)。
  std::string current_route_name_;

  // active route の POI 名 set (waypoints + landmarks 両方を含む) (#143)。
  // route 受信 (on_route_received) で set、route 終端 / cancel / GOAL 切替で clear。
  // tolerance_check_callback の pause 発火条件 (active route POI に含まれる時のみ) で参照。
  std::unordered_set<std::string> current_route_poi_names_;

  // nav_attempt_generation: navigation 開始 (route or GOAL) ごとに 1 ずつ増分。各 action
  // callback で stale 判定に使う (Codex review #147 round 1+2 high 対応)。route ↔ route /
  // route ↔ GOAL / GOAL ↔ GOAL いずれの切替でも、旧 navigation の遅延 callback が新 nav state
  // を消さないようにする。
  // 安全根拠 (#213 で MultiThreadedExecutor 化後): nav state を読み書きする callback は全て
  // default MutuallyExclusive callback group 内で直列化される。Reentrant callback group で
  // 動く backend_status timer は nav state member に触らない (read-only な *_is_ready() のみ)
  // ため、mutex なしで読み書き OK。
  size_t nav_attempt_generation_ = 0;

  void reset_nav_state();

  std::mutex data_mutex_;
  std::vector<mapoi_interfaces::msg::PointOfInterest> pois_list_;

  // Navigation backend readiness publisher (#198).
  // 1Hz timer で action / service の存在を polling し、`mapoi/nav/backend_status` に publish する。
  // transient_local QoS で後起動 subscriber も最後の状態を受信できる。WebUI / panel はこの値で
  // 「Navigation connected」UI を gate する (= command topic subscriber 数だけでは Nav2 不在を
  // 検知できなかった #198 の課題に対応)。
  rclcpp::Publisher<mapoi_interfaces::msg::NavigationBackendStatus>::SharedPtr backend_status_pub_;
  rclcpp::TimerBase::SharedPtr backend_status_timer_;
  // 独立 MutuallyExclusive callback_group + MultiThreadedExecutor で backend_status timer を
  // 他 callback と独立 thread で動かすための group (#213)。Reentrant ではなく独立
  // MutuallyExclusive を選ぶ理由は cpp 側コメント参照 (#214 cursor review medium)。
  rclcpp::CallbackGroup::SharedPtr backend_status_callback_group_;
  void publish_backend_status();

  // Nav status publisher
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr nav_status_pub_;
  // payload は target 空なら "status"、target 有りなら "status:target" を送る (#104)。
  // subscriber 側 (mapoi_panel / mapoi_webui_node) は : split で target を復元する。
  void publish_nav_status(const std::string & status, const std::string & target = "");
  // 「受理する前に無効と判定して実行しなかった」新規コマンドを "rejected" で通知する (#339)。
  // 進行中の navigation (nav_mode_ != IDLE) がある間は publish しない — 実際の走行状態
  // (navigating / paused / map_switching) を上書きしないため。詳細は .cpp 側の doc コメント参照。
  void publish_rejected_status(const std::string & target);
  // Command-rejected イベント通知 publisher (#354)。`mapoi/nav/status` は latched な
  // 状態 snapshot のため nav_mode_ != IDLE の間は "rejected" を書けない (#339) が、その
  // 副作用で走行中の reject に操作者が気づけない課題が残っていた。本 publisher は
  // 状態と独立したイベント軸として volatile (非 latched) QoS で持ち、
  // publish_rejected_status の先頭で nav_mode_ に関わらず常に publish する。
  // payload は reject 対象の target 文字列のみ (reason はログに残る従来通り)。
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr command_rejected_pub_;
  bool send_load_map_request(const std::string & server_name, const std::string & map_file);
  // #211: LoadMap 成功後に mapoi_server (唯一の writer) へ request_initial_pose service 経由で
  // initial pose POI の publish を依頼する。LoadMap 完了の timing gate は nav2_bridge が引き続き
  // 所有し、wire-publish のみ mapoi_server に移す。
  void request_initial_pose(const std::string & map_name, const std::string & poi_name);

  // 現在 nav の target POI 名 / route 名。Acceptance 時 (goal_response_callback /
  // ntp_goal_response_callback) に更新され、pause / resume が active nav の target
  // として参照する用途のみ。
  // 終端 status (succeeded / aborted / canceled) は callback に lambda capture で
  // bind された goal 固有の target を使うので、ここを読まない (#104 race fix)。
  // reset_nav_state では clear せず、次の acceptance で上書きされる前提。
  std::string current_target_name_;

  // --- POI radius event detection ---
  std::shared_ptr<tf2_ros::Buffer> tf_buffer_;
  std::shared_ptr<tf2_ros::TransformListener> tf_listener_;

  rclcpp::TimerBase::SharedPtr tolerance_check_timer_;
  rclcpp::Publisher<mapoi_interfaces::msg::PoiEvent>::SharedPtr poi_event_pub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config_path_sub_;
  rclcpp::Client<mapoi_interfaces::srv::GetTagDefinitions>::SharedPtr tag_defs_client_;

  // path だけだと WebUI/Panel Save (path 不変、内容のみ変更) を取りこぼすため、mtime も併用。
  // guard は fetch 成功 callback (on_pois_info_received) で確定し、失敗時は pending のまま
  // 次回 publish で retry する。pending_guard_active_ が false の時 (start_sequence 経由の初回 fetch)
  // は guard 確定処理を skip。single-thread executor 前提で書き込みは callback context のみ。
  // 詳細は PR #78 (#75) と issue #80。
  std::string last_config_path_;
  std::filesystem::file_time_type last_config_mtime_{};
  std::string pending_config_path_;
  std::filesystem::file_time_type pending_config_mtime_{};
  bool pending_guard_active_ = false;
  std::unordered_set<std::string> system_tags_;
  bool system_tags_loaded_ = false;
  std::unordered_map<std::string, bool> poi_inside_state_;  // key: poi.name
  // EVENT_PAUSED 発火済 per-POI 状態 (#220)。
  //   true: 既に EVENT_PAUSED publish 済 (再 publish しない、EXIT で reset)
  //   false: 未発火 / ENTER 後 / EXIT 後
  // ENTER/EXIT 判定 (poi_inside_state_) と独立して持つ: PAUSED は inside 内での
  // sub-state なので、EXIT 時は inside_state→false と同時に paused_state→false に reset する。
  std::unordered_map<std::string, bool> poi_paused_published_;  // key: poi.name
  std::vector<mapoi_interfaces::msg::PointOfInterest> event_pois_;

  // initial pose POI 要求 client (Nav2 LoadMap 完了後 trigger 用、#209 → #211 で service 化)。
  // 従来は本ノードが `mapoi/initialpose_poi` を直接 publish していたが、transient_local の
  // per-writer latched cache がクロス writer 競合を生むため (#211)、publish は mapoi_server に
  // 集約し、本ノードは request_initial_pose service で依頼するのみ。実際の `/initialpose` 配信は
  // localization bridge が担当する (#209 で mapoi_nav2_bridge から分離)。
  rclcpp::Client<mapoi_interfaces::srv::RequestInitialPose>::SharedPtr request_initial_pose_client_;

  // --- EVENT_PAUSED trigger 用 cmd_vel dwell 検知 (#220) ---
  // cmd_vel subscriber: pause タグ POI 内で navigation 停止を検知する source。
  // dwell 判定は robot 全体で 1 つ (POI ごとには持たない)。線速ノルムと角速度絶対値の両方が
  // 同じ閾値 ``stopped_speed_threshold`` 未満のとき停止扱い。線速 (m/s) と角速 (rad/s) に
  // 単位の異なる同一閾値を使う割り切りは、撮影シナリオで「その場旋回も停止扱いしたくない」
  // 用途と整合する (param 説明 / docs にも明記)。
  // 前提: 採用 controller が navigation 停止中も cmd_vel = 0 を継続 publish すること
  // (Nav2 default の挙動)。controller が静止時に cmd_vel publish を止める実装の場合、
  // EVENT_PAUSED は発火しない。
  // cmd_vel は ROS 2 distro / controller によって型が分かれる (#249):
  //   - Humble Nav2 等: geometry_msgs::msg::Twist
  //   - Jazzy 以降の Nav2: geometry_msgs::msg::TwistStamped (collision_monitor /
  //     docking_server などが TwistStamped 化済)
  // どちらか一方だけを subscribe する: 同じ topic に違う型の subscriber を 2 つ
  // 作ると rcl が `invalid allocator` で crash するため (DDS の type registry が
  // 型不一致を受け付けない)。`cmd_vel_msg_type` parameter で選択する:
  //   - "twist":         Twist で subscribe (humble デフォルト互換)
  //   - "twist_stamped": TwistStamped で subscribe (jazzy 以降 Nav2 互換)
  //   - "auto" (default): ROS_DISTRO 環境変数を見て自動選択
  //     ROS_DISTRO=humble → twist、それ以外 (jazzy, kilted, ...) → twist_stamped
  // 一方しか作らないので shared_ptr の片方は nullptr のまま。
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub_;
  rclcpp::Subscription<geometry_msgs::msg::TwistStamped>::SharedPtr cmd_vel_stamped_sub_;
  void cmd_vel_callback(const geometry_msgs::msg::Twist::SharedPtr msg);
  void cmd_vel_stamped_callback(const geometry_msgs::msg::TwistStamped::SharedPtr msg);
  // zero-velocity 判定の本体 (両 callback の共通実装、#249)。linear_x/y/z + angular_z を
  // 渡せば Twist / TwistStamped どちらでも同じ閾値判定で `zero_velocity_active_` /
  // `last_zero_velocity_start_` を更新する。
  void update_zero_velocity_state(
    double linear_x, double linear_y, double linear_z, double angular_z);
  // `cmd_vel_msg_type` parameter を実 type 名 ("twist" or "twist_stamped") に解決する (#249)。
  // "auto" は ROS_DISTRO env を見て humble なら "twist"、それ以外 (jazzy 以降) は
  // "twist_stamped"。未知値は安全側で "twist" にフォールバック。
  static std::string resolve_cmd_vel_msg_type(const std::string & param_value);
  // 速度が閾値以下になり始めた時刻 (steady_clock)。閾値超えに戻ったら reset。
  std::chrono::steady_clock::time_point last_zero_velocity_start_ {};
  bool zero_velocity_active_ {false};
  // PAUSED 判定 param のキャッシュ (cmd_vel callback で毎 tick 取得すると lock コストが高いため
  // constructor で読み込んで保持。ROS 動的 reconfigure で変更したい場合は再起動が必要)。
  double stopped_speed_threshold_ {0.01};
  double stopped_dwell_time_sec_ {1.0};

  // EVENT_PAUSED 発火後の auto-resume timeout (#231)。0.0 = disabled (現行仕様、外部からの
  // `/mapoi/nav/resume` を無限待ち)。正値で「PAUSED 発火から N 秒後に内部で resume を呼ぶ」
  // demo / 自動運転シナリオ向けの opt-in 動作。負値は constructor で reject (0.0 にフォールバック)。
  // 動的 reconfigure には非対応。
  double auto_resume_timeout_sec_ {0.0};
  // pending one-shot timer。次の PAUSED 発火時 / 外部 resume / reset_nav_state で cancel し
  // 上書きする。生存中は nav state を握る default MutuallyExclusive callback group で動くため、
  // mapoi_pause_cb / mapoi_resume_cb / tolerance_check_callback と直列化される (= race free)。
  rclcpp::TimerBase::SharedPtr auto_resume_timer_;
  // pending timer がどの POI に対して張られているか (ログ・debug 用、cancel 判断には未使用)。
  std::string auto_resume_target_poi_;
  // PAUSED 発火 POI 用の helper。tolerance_check_callback で lock 外 + dwell 観測直後に
  // 呼ぶ。呼び出し前提条件: `is_paused_` は既に true (auto-pause 経路で mapoi_pause_cb 済)。
  void schedule_auto_resume_(const std::string & poi_name);
  // 共通 cancel (mapoi_resume_cb / reset_nav_state / 新規 schedule_auto_resume_ で呼ぶ)。
  void cancel_auto_resume_timer_();

  void fetch_system_tags();
  void on_system_tags_received(rclcpp::Client<mapoi_interfaces::srv::GetTagDefinitions>::SharedFuture future);
  void on_config_path_changed(const std_msgs::msg::String::SharedPtr msg);
  void rebuild_event_pois();
  void tolerance_check_callback();
  double distance_2d(const geometry_msgs::msg::Pose & poi_pose, double rx, double ry);

  // quaternion から yaw (ZYX 分解の Z 成分) を取り出す純関数 (#261)。tf2::getYaw と同等だが
  // tf2 型変換を挟まず geometry_msgs/Quaternion を直接受ける (test も依存なしで書ける)。
  static double yaw_from_quaternion(const geometry_msgs::msg::Quaternion & q);
  // 2 つの角度差を [-pi, pi] に正規化した絶対値 (rad) を返す純関数 (#261)。
  // atan2(sin(d), cos(d)) で wrap-around (例: 3.0 と -3.0 の差は 6.0 ではなく ~0.28) を吸収する。
  static double angle_diff_abs(double a, double b);

  // landmark system tag を持つかを判定する純関数 (#85)。
  // landmark POI は Nav2 navigation goal / initial_pose に使えない reference 専用。
  static bool has_landmark_tag(const mapoi_interfaces::msg::PointOfInterest & poi);

  // active route の POI 名集合を組み立てる純関数 (#143 / #148)。
  // waypoints と landmarks の両方を 1 つの set にマージし、tolerance_check の
  // pause 発火条件 (active route POI に含まれるか) を判定する用に使う。
  // 同名の重複は set 性質で自動的に 1 つに集約される。
  static std::unordered_set<std::string> build_route_poi_names(
    const std::vector<mapoi_interfaces::msg::PointOfInterest> & waypoints,
    const std::vector<mapoi_interfaces::msg::PointOfInterest> & landmarks);

  // pause 自動発火の eligibility 判定純関数 (#143 / #148)。
  // ROUTE 走行中 + active route POI + "pause" タグありの 3 条件を全て満たす場合のみ true。
  // GOAL 走行中 / IDLE では false (操作者の意図しない停止を避けるため、route POI 限定)。
  // **lock 契約**: `current_route_poi_names_` をそのまま渡す呼び出しでは、参照が
  // 安定するように `data_mutex_` を保持中に呼ぶこと。snapshot を渡す場合は不要。
  static bool is_pause_eligible(
    const mapoi_interfaces::msg::PointOfInterest & poi,
    NavMode nav_mode,
    const std::unordered_set<std::string> & active_route_poi_names);

  // route 追従中の event 発火対象 (active route POI) かを判定する純関数 (#193)。
  // ROUTE 走行中 + active route POI の 2 条件で、is_pause_eligible から pause タグ条件を
  // 除いた superset。ENTER/EXIT/PAUSED の発火 gate に使い、非 ROUTE mode (IDLE/GOAL) では
  // route 登録 POI に進入しても常に false を返す。invariant: is_pause_eligible ⟹ is_active_route_poi。
  // **lock 契約**: is_pause_eligible と同じ (`current_route_poi_names_` を渡す場合は
  // `data_mutex_` 保持中に呼ぶ)。
  static bool is_active_route_poi(
    const mapoi_interfaces::msg::PointOfInterest & poi,
    NavMode nav_mode,
    const std::unordered_set<std::string> & active_route_poi_names);

  // 統一到達判定 (#265 OR トリガ a) を pure 化した純関数。route 中間 waypoint /
  // 最終 goal / 単発 Go すべてこの 1 式 (dist <= tolerance.xy ∧ yaw_diff <= tolerance.yaw)
  // で判定する。dist は distance_2d、yaw_diff は angle_diff_abs(r_yaw, poi_yaw) の結果を渡す。
  static bool is_within_arrival_tolerance(
    double dist, double yaw_diff, double tolerance_xy, double tolerance_yaw);

  // POI radius の ENTER/EXIT ヒステリシス遷移を pure 判定する純関数 (#220)。
  // ENTER: !was_inside ∧ dist <= tolerance.xy / EXIT: was_inside ∧ dist > tolerance.xy * hysteresis。
  // どちらでもなければ NONE。発火 (state 書換・publish) は呼び出し側に残す。
  static RadiusTransition classify_radius_transition(
    bool was_inside, double dist, double tolerance_xy, double hysteresis_multiplier);

#ifdef UNIT_TEST
  friend class Nav2BridgeTestFixture;
  FRIEND_TEST(Nav2BridgeTestFixture, DistanceCalculation);
  FRIEND_TEST(Nav2BridgeTestFixture, DistanceCalculationZero);
  FRIEND_TEST(Nav2BridgeTestFixture, YawFromQuaternionIdentity);
  FRIEND_TEST(Nav2BridgeTestFixture, YawFromQuaternionHalfPi);
  FRIEND_TEST(Nav2BridgeTestFixture, YawFromQuaternionNegHalfPi);
  FRIEND_TEST(Nav2BridgeTestFixture, YawFromQuaternionPi);
  FRIEND_TEST(Nav2BridgeTestFixture, AngleDiffAbsZero);
  FRIEND_TEST(Nav2BridgeTestFixture, AngleDiffAbsHalfPi);
  FRIEND_TEST(Nav2BridgeTestFixture, AngleDiffAbsWrapAround);
  FRIEND_TEST(Nav2BridgeTestFixture, AngleDiffAbsSymmetric);
  FRIEND_TEST(Nav2BridgeTestFixture, RebuildEventPoisIncludesAllPois);
  FRIEND_TEST(Nav2BridgeTestFixture, RebuildEventPoisEmpty);
  FRIEND_TEST(Nav2BridgeTestFixture, PauseTagDetection);
  FRIEND_TEST(Nav2BridgeTestFixture, HasLandmarkTagDetection);
  FRIEND_TEST(Nav2BridgeTestFixture, BuildRoutePoiNamesWaypointsOnly);
  FRIEND_TEST(Nav2BridgeTestFixture, BuildRoutePoiNamesWaypointsAndLandmarks);
  FRIEND_TEST(Nav2BridgeTestFixture, BuildRoutePoiNamesLandmarksEmptyBackwardCompat);
  FRIEND_TEST(Nav2BridgeTestFixture, BuildRoutePoiNamesEmpty);
  FRIEND_TEST(Nav2BridgeTestFixture, BuildRoutePoiNamesDuplicateNames);
  FRIEND_TEST(Nav2BridgeTestFixture, IsPauseEligibleRouteModeActiveWithPauseTag);
  FRIEND_TEST(Nav2BridgeTestFixture, IsPauseEligibleRouteModeActiveWithoutPauseTag);
  FRIEND_TEST(Nav2BridgeTestFixture, IsPauseEligibleRouteModeNonActivePoi);
  FRIEND_TEST(Nav2BridgeTestFixture, IsPauseEligibleGoalMode);
  FRIEND_TEST(Nav2BridgeTestFixture, IsPauseEligibleIdleMode);
  FRIEND_TEST(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInIdleMode);
  FRIEND_TEST(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInGoalMode);
  FRIEND_TEST(Nav2BridgeTestFixture, IsActiveRoutePoiTrueInRouteModeListedPoi);
  FRIEND_TEST(Nav2BridgeTestFixture, IsActiveRoutePoiFalseInRouteModeUnlistedPoi);
  FRIEND_TEST(Nav2BridgeTestFixture, ResetNavStateClearsRouteContext);
  FRIEND_TEST(Nav2BridgeTestFixture, AutoResumeTimeoutDefaultDisabled);
  FRIEND_TEST(Nav2BridgeTestFixture, AutoResumeTimeoutNegativeClampedToZero);
  FRIEND_TEST(Nav2BridgeTestFixture, AutoResumeTimeoutNonFiniteClampedToZero);
  FRIEND_TEST(Nav2BridgeTestFixture, CancelAutoResumeTimerIsIdempotent);
  FRIEND_TEST(Nav2BridgeTestFixture, ResetNavStateCancelsAutoResumeTimer);
  FRIEND_TEST(Nav2BridgeTestFixture, CmdVelTwistCallbackUpdatesZeroVelocityState);
  FRIEND_TEST(Nav2BridgeTestFixture, CmdVelTwistStampedCallbackUpdatesZeroVelocityState);
  FRIEND_TEST(Nav2BridgeTestFixture, CmdVelNonZeroClearsZeroVelocityState);
  FRIEND_TEST(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeExplicit);
  FRIEND_TEST(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeAutoByDistro);
  FRIEND_TEST(Nav2BridgeTestFixture, ResolveCmdVelMsgTypeUnknownFallback);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorTwistStampedParamCreatesStampedSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorTwistParamCreatesTwistSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorUnknownParamJazzyFallsBackToStampedSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorUnknownParamHumbleFallsBackToTwistSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorAutoParamJazzyCreatesStampedSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorAutoParamHumbleCreatesTwistSub);
  FRIEND_TEST(Nav2BridgeTestFixture, ConstructorAutoParamUnsetDistroCreatesStampedSub);
  FRIEND_TEST(Nav2BridgeTestFixture, IsWithinArrivalToleranceXyAndYawInside);
  FRIEND_TEST(Nav2BridgeTestFixture, IsWithinArrivalToleranceYawOff);
  FRIEND_TEST(Nav2BridgeTestFixture, IsWithinArrivalToleranceXyOff);
  FRIEND_TEST(Nav2BridgeTestFixture, IsWithinArrivalToleranceBoundaryXy);
  FRIEND_TEST(Nav2BridgeTestFixture, IsWithinArrivalToleranceBoundaryYaw);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionEnter);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionExitWithHysteresis);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionStaysInsideHysteresisBand);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionStaysOutside);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionEnterAtBoundary);
  FRIEND_TEST(Nav2BridgeTestFixture, ClassifyRadiusTransitionExitAtBoundary);
#endif
};

#endif  // MAPOI_SERVER__MAPOI_NAV_SERVER_HPP_
