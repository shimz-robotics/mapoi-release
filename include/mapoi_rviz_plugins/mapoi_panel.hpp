#pragma once

#ifndef Q_MOC_RUN
#include <rclcpp/rclcpp.hpp>
#include <filesystem>
#endif

#include <QTimer>
#include <rviz_common/panel.hpp>
#include <rviz_common/display_context.hpp>
#include <geometry_msgs/msg/pose.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/int8.hpp>
#include <std_msgs/msg/string.hpp>
#include "mapoi_interfaces/msg/point_of_interest.hpp"
#include "mapoi_interfaces/msg/poi_event.hpp"
#include "mapoi_interfaces/srv/request_initial_pose.hpp"
#include "mapoi_interfaces/msg/navigation_backend_status.hpp"
#include "mapoi_interfaces/msg/localization_backend_status.hpp"
#include "mapoi_interfaces/srv/get_pois_info.hpp"
#include "mapoi_interfaces/srv/get_maps_info.hpp"
#include "mapoi_interfaces/srv/get_route_pois.hpp"
#include "mapoi_interfaces/srv/get_routes_info.hpp"

namespace Ui {
class ScUI;
}

namespace mapoi_rviz_plugins
{

class MapoiPanel: public rviz_common::Panel
{
  Q_OBJECT
public:
  MapoiPanel(QWidget* parent = nullptr);
    ~MapoiPanel() override;

  void onInitialize() override;
  void onEnable();
  void onDisable();

private Q_SLOTS:
  void MapComboBox();
  void Nav2GoalComboBox();
  void MapoiRouteComboBox();

  void LocalizationButton();
  void RunGoalButton();
  void RunRouteButton();
  void PauseButton();
  void ResumeButton();
  void StopButton();

protected:
  Ui::ScUI* ui_;
  int goal_combobox_ind_;
  std::vector<mapoi_interfaces::msg::PointOfInterest> pois_;
  std::vector<std::string> map_name_list_;
  std::string current_map_;

  rclcpp::Node::SharedPtr node_;

  // Shared service node and persistent clients
  rclcpp::Node::SharedPtr service_node_;
  rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr get_pois_info_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetMapsInfo>::SharedPtr get_maps_info_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetRoutesInfo>::SharedPtr get_routes_info_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedPtr get_route_pois_client_;

  // #211: LocalizationButton クリック時に直接 publish せず、唯一の writer である mapoi_server へ
  // request_initial_pose service 経由で {map_name, poi_name} の publish を依頼する。bridge が
  // POI resolve / `/initialpose` 配信 / retry を担当する点は不変 (#209)。直接 `/initialpose` には
  // publish しない。client は他の service client と同じく service_node_ 上に作る。
  rclcpp::Client<mapoi_interfaces::srv::RequestInitialPose>::SharedPtr request_initial_pose_client_;
  // mapoi/nav/goal_pose_poi (std_msgs/String) publisher: RunGoalButton クリック時に POI 名を
  // publish し、navigation bridge (mapoi_nav2_bridge / 自作 bridge) が POI resolve / Nav2 action
  // 起動 / status 配信を担当する (#209 review #3)。直接 `goal_pose` には publish しない。
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_goal_pose_poi_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_cancel_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_switch_map_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_pause_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_resume_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_route_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_highlight_goal_pub_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr mapoi_highlight_route_pub_;

  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config_path_sub_;
  void ConfigPathCallback(std_msgs::msg::String::SharedPtr msg);

  // 内容 diff ガード (#403)。直前に処理した config_path イベントの path と mtime を保持し、
  // path も mtime も変わっていない再 publish (= 内容変化なし) を Noop に落とす。
  // UI (Qt メイン) スレッド上でのみ読み書きする (queued lambda 内、PR #412/#414 の規約と同じ)。
  std::string last_seen_config_path_;
  std::filesystem::file_time_type last_seen_config_mtime_{};

  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr nav_status_sub_;
  void NavStatusCallback(std_msgs::msg::String::SharedPtr msg);
  std::string current_nav_mode_;
  std::string current_nav_target_;

  // #398: mapoi/nav/command_rejected 購読。latched な NavStatusLabel とは別 label (CommandRejectedLabel)
  // に一時表示することで、権威的な状態スナップショットと揮発的なイベント通知を分離する。
  // nav_mode を問わず全 reject で publish される (#354 設計) ので走行中の誤操作にも気づける。
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr command_rejected_sub_;
  void CommandRejectedCallback(std_msgs::msg::String::SharedPtr msg);
  QTimer* reject_clear_timer_ {nullptr};
  // #401: CommandRejectedLabel への一時通知を集約するヘルパー。#398 の CommandRejectedLabel と
  // reject_clear_timer_ (singleShot 5 秒) を共用し、initial pose 拒否 / route 取得失敗など
  // ボタン押下起点の操作失敗も同じ揮発表示に載せる (latched な NavStatusLabel は汚さない)。
  // UI (Qt メイン) スレッドからのみ呼ぶこと (呼び出し元は全て Qt スロット / queued lambda 内)。
  // is_error=false で情報通知 (緑)。既定はエラー (赤)。
  void ShowTransientNotice(const QString & text, bool is_error = true);

  // #451: NavStatusLabel / RouteProgressLabel / CommandRejectedLabel は空文字にせず、
  // idle 時は「<caption>: —」をグレーで常設表示する (空白行に見える問題の解消。ラベル
  // 常駐自体はレイアウトジャンプ防止の仕様で hide しない)。idle 表示の正本は Set*Idle
  // 関数で、constructor (setupUi 直後) とクリア経路の両方から呼ぶ。NavStatusLabel は
  // idle のグレーを既定色へ戻すため setText 直呼びせず必ず SetNavStatus() を通すこと。
  // いずれも UI スレッドからのみ呼ぶ。
  void SetNavStatus(const QString & text);
  void SetNavStatusIdle();
  void SetRouteProgressIdle();
  void SetNoticeIdle();

  // #406: mapoi/events 購読。ROUTE 走行中の通過 POI 進捗を RouteProgressLabel に表示する。
  // events は ROUTE 走行時のみ発火 (IDLE/GOAL では無音) なのでアイドル時の負荷はゼロ。
  // 総数は highlighted_route_poi_names_ (panel でルート選択時に取得) を参照するため、
  // 外部ノード起点の走行 (panel 未選択) では総数が不明になるケースがある。
  rclcpp::Subscription<mapoi_interfaces::msg::PoiEvent>::SharedPtr poi_event_sub_;
  void PoiEventCallback(mapoi_interfaces::msg::PoiEvent::SharedPtr msg);
  // 終了系 nav/status の受信後に遅延到着した events で進捗表示が復活するのを抑制する
  // (nav/status と events はトピック間で到達順序が保証されない)。navigating 受信で解除。
  // UI スレッド (queued lambda 内) でのみ読み書きする。
  bool route_progress_suppressed_ {false};

  // Navigation backend readiness subscribe (#198, #205 minimal contract):
  // bridge が `mapoi/nav/backend_status` を publish する場合は backend_ready で navigation
  // 操作ボタンと MapComboBox を一括 gate する。topic 不在 (旧 mapoi_nav_server build (#208 以前 backend_status contract 未実装、#204 で nav2_bridge へ rename)) では「全 enable」の
  // ままで後方互換を保つ。Minimal contract なので per-capability gate は持たない。
  rclcpp::Subscription<mapoi_interfaces::msg::NavigationBackendStatus>::SharedPtr backend_status_sub_;
  void BackendStatusCallback(mapoi_interfaces::msg::NavigationBackendStatus::SharedPtr msg);
  // backend_status 不在 (contract 未実装の bridge build / editor 構成) では callback が呼ばれず、初期値
  // (true = enable) のままで後方互換を保つ。Minimal contract なので per-capability gate は持たない。
  // 以下のメンバはすべて UI (Qt メイン) スレッド所有 (#400: queued lambda 内でのみ読み書きする)。
  bool last_navigation_backend_ready_ {true};
  // 一度でも navigation backend_status を受信したか。受信実績なし = 旧 mapoi_nav_server build (#208 以前 contract 未実装、#204 で rename)
  // / editor 構成として enable のまま (後方互換)。受信実績ありの publisher が liveliness lost
  // (= bridge 死亡) した場合のみ disable に倒す。
  bool nav_backend_status_received_ {false};
  // MANUAL_BY_TOPIC liveliness (#208) で publisher 生存を track。subscription event_callback
  // が `alive_count > 0` で更新する。`*_received_` と AND して、未受信は alive 判定をバイパス。
  bool nav_backend_alive_ {false};
  // backend_ready=false 時に bridge が設定する reason 文字列 (#400)。UI スレッド所有。
  std::string last_navigation_reason_;

  // Localization backend readiness subscribe (#209): mapoi_amcl_localization_bridge (or any
  // custom localization bridge) が publish する readiness で LocalizationButton を gate する。
  // navigation backend (#205) と独立した仕様。topic 不在 (bridge 単独不起動 / editor 構成) は
  // 後方互換のため「全 enable のまま」とする (BackendStatusCallback と同じ初期値方針)。
  rclcpp::Subscription<mapoi_interfaces::msg::LocalizationBackendStatus>::SharedPtr
    localization_backend_status_sub_;
  void LocalizationBackendStatusCallback(
    mapoi_interfaces::msg::LocalizationBackendStatus::SharedPtr msg);
  // 以下のメンバはすべて UI (Qt メイン) スレッド所有 (#400: queued lambda 内でのみ読み書きする)。
  bool last_localization_backend_ready_ {true};
  bool localization_backend_status_received_ {false};
  bool localization_backend_alive_ {false};
  // backend_ready=false 時に bridge が設定する reason 文字列 (#400)。UI スレッド所有。
  std::string last_localization_reason_;

  // navigation_ready / localization_ready の最新値を保持し、両 callback / liveliness event から
  // 共通の UpdateNavButtonsEnabled を呼び出す。LocalizationButton は localization、それ以外の
  // navigation 操作 UI は navigation で gate する (#209 で 2 軸に分離)。
  void UpdateNavButtonsEnabled();

  void RequestSetCmdVelMode(std::string cm);
  void SetMapComboBox(std::string map_name);
  void SetNav2GoalComboBox();
  void SetMapoiRouteComboBox();
  void PublishHighlightPois();

  std::vector<std::string> route_name_list_;
  int route_combobox_ind_;

  std::string highlighted_goal_name_;
  std::vector<std::string> highlighted_route_poi_names_;
};
}
