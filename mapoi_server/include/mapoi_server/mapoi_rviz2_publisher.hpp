#ifndef MAPOI_SERVER__MAPOI_RVIZ2_PUBLISHER_HPP_
#define MAPOI_SERVER__MAPOI_RVIZ2_PUBLISHER_HPP_

#include <vector>
#include <set>
#include <map>
#include <array>
#include <string>
#include <sstream>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <filesystem>

#include <rclcpp/rclcpp.hpp>
#include <visualization_msgs/msg/marker_array.hpp>
#include <visualization_msgs/msg/marker.hpp>
#include <geometry_msgs/msg/pose.hpp>
#include <std_msgs/msg/string.hpp>

#include <yaml-cpp/yaml.h>
#include "mapoi_interfaces/srv/get_pois_info.hpp"
#include "mapoi_interfaces/srv/get_routes_info.hpp"
#include "mapoi_interfaces/srv/get_route_pois.hpp"
#include "mapoi_interfaces/msg/point_of_interest.hpp"

class MapoiRviz2Publisher : public rclcpp::Node
{
public:
  MapoiRviz2Publisher();

private:
  /**
   * @brief 初期化待ち合わせ用タイマーコールバック
   */
  void start_sequence();

  /**
   * @brief マーカー配信周期実行用コールバック
   */
  void timer_callback();

  /**
   * @brief POI情報取得サービスのレスポンス処理
   * @param future サービスの結果
   */
  void on_poi_received(rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future);

  /**
   * @brief get_pois_info service を呼び出して pois_list_ を更新する共通処理
   */
  void request_pois_list();

  /**
   * @brief mapoi/config_path topic の変化検出時に POI list を再取得する
   */
  void on_config_path_changed(const std_msgs::msg::String::SharedPtr msg);

  /**
   * @brief get_routes_info service を呼び出し、各 route の waypoint を順次 fetch する。
   * 各 fan-out は generation 番号で識別し、後続 fan-out が始まったら旧世代の callback は捨てる
   * (stale callback による partial state / route 混在防止)。fetch 完了 (全 route 集約済み) になった
   * 時点で all_routes_ に lock 下で swap する (timer_callback への部分公開を防ぐ)。
   */
  void request_routes_info();
  void on_routes_info_received(
    size_t my_generation,
    rclcpp::Client<mapoi_interfaces::srv::GetRoutesInfo>::SharedFuture future);

  // --- Member Variables ---

  // マーカーID管理用
  int id_buf_;

  // Publishers
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr marker_pois_pub_;
  rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr marker_routes_pub_;

  // Clients
  rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedPtr poi_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetRoutesInfo>::SharedPtr routes_info_client_;
  rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedPtr route_pois_client_;

  // Subscribers
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr highlight_goal_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr highlight_route_sub_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr config_path_sub_;
  void on_highlight_goal_received(const std_msgs::msg::String::SharedPtr msg);
  void on_highlight_route_received(const std_msgs::msg::String::SharedPtr msg);

  // Config path + mtime guard。
  // path だけ覚えると周期 publish に対して常に skip する一方、WebUI/Panel Save (内容のみ変更で path 不変)
  // も skip してしまう。mtime も併せて見ることで map switch (path 変更) と Save (mtime 変更) の両方を検出。
  // single-thread executor 前提で書き込みは callback context のみ。MultiThreadedExecutor 移行時は mutex が必要。
  std::string last_config_path_;
  std::filesystem::file_time_type last_config_mtime_{};

  // Timers
  rclcpp::TimerBase::SharedPtr init_timer_;
  rclcpp::TimerBase::SharedPtr timer_;

  // Data storage
  std::mutex data_mutex_;
  std::vector<mapoi_interfaces::msg::PointOfInterest> pois_list_;
  std::set<std::string> highlighted_goal_names_;
  std::map<std::string, int> highlighted_route_names_;
  std::vector<std::string> highlighted_route_ordered_;

  // route 名 → waypoint POI 列。get_routes_info で名前一覧を取得後、
  // 各 route について get_route_pois を非同期に呼び、全 route 揃った時点で lock 下で swap される。
  std::map<std::string, std::vector<mapoi_interfaces::msg::PointOfInterest>> all_routes_;
  // route fetch fan-out の世代番号。on_routes_info_received / per-route callback の lambda が
  // capture した値と現値が異なれば stale (= 後続 fan-out が走った) として捨てる。
  // single-thread executor 前提で書き込みは callback context のみ。
  size_t routes_fetch_generation_ = 0;
  // 前回 publish 時の route marker max id (DELETEALL 判定用、id_buf_ と独立)
  int route_id_buf_ = 0;

  // --- 差分ゲート (#402) ---
  // 1Hz の timer_callback で毎 tick フル再構築 (cos/sin/頂点列生成) するのが無駄なため、
  // データ (pois_list_ / all_routes_ / highlight / 描画 parameter) が変わった tick だけ
  // marker を再構築し、変化なしの tick は cache の header.stamp だけ更新して再 publish する。
  // 再 publish 自体は継続する: 購読側 QoS が volatile なので、後着 RViz へ marker を届けるには
  // 1Hz 再送を保つ必要がある (issue #402 の要件)。
  // dirty フラグはデータ書き込み点 (subscription callback / service callback) で立て、
  // timer_callback がフル構築した tick で消費 (クリア) する。data_mutex_ 下で読み書きする。
  bool marker_data_dirty_ = true;  // 初期 true で初回 tick は必ずフル構築する
  // 直近のフル構築結果 (dirty tick でここに保存、clean tick は stamp のみ更新して再 publish)
  visualization_msgs::msg::MarkerArray cached_ma_pois_;
  visualization_msgs::msg::MarkerArray cached_ma_routes_;

  // 描画 parameter の前回値 (timer_callback 冒頭で現値と比較し、変化していたら dirty を立てる)。
  // parameter callback 機構は追加せず、get_parameter (現状も毎 tick 呼んでいる) の結果で検出する。
  bool last_show_sector_ = true;               // show_tolerance_sector の default
  std::string last_poi_label_format_ = "index";  // poi_label_format の default
  std::string last_route_display_mode_ = "selected";  // route_display_mode の default
};

#endif  // MAPOI_SERVER__MAPOI_RVIZ2_PUBLISHER_HPP_
