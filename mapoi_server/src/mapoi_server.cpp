#include "mapoi_server/mapoi_server.hpp"

#include <cmath>
#include <cstdio>

#include "mapoi_server/initial_pose_resolver.hpp"
#include "mapoi_server/map_state_store.hpp"
#include "mapoi_server/system_tags.hpp"

MapoiServer::MapoiServer() : Node("mapoi_server") {
  // maps_path は REQUIRED (#163 で sample maps を mapoi_server から削除したため default 廃止)。
  this->declare_parameter<std::string>("maps_path", "");
  this->declare_parameter<std::string>("map_name", "turtlebot3_world");
  this->declare_parameter<std::string>("config_file", "mapoi_config.yaml");
  // #297: last-selected map 永続化用 state dir。空 (default) = 永続化無効 (従来挙動)。
  // opt-in にしている理由: default で ~/.ros 等へ暗黙に書くと、既存デプロイ・test 環境で
  // 前回 run の state が次回起動の挙動を変える汚染 (特に launch_test 間) が起きるため。
  this->declare_parameter<std::string>("state_path", "");

  // initial maps path
  maps_path_ = this->get_parameter("maps_path").as_string();
  map_name_ = this->get_parameter("map_name").as_string();
  config_file_ = this->get_parameter("config_file").as_string();
  state_path_ = this->get_parameter("state_path").as_string();
  // 空白のみの値も REQUIRED 違反として扱う (#170 Round 4 low)。
  if (maps_path_.find_first_not_of(" \t\r\n") == std::string::npos) {
    maps_path_.clear();
  }
  // state_path も空白のみは未設定 (= 永続化無効) に正規化する。
  if (state_path_.find_first_not_of(" \t\r\n") == std::string::npos) {
    state_path_.clear();
  }
  if (maps_path_.empty()) {
    RCLCPP_FATAL(this->get_logger(),
      "maps_path parameter is REQUIRED. mapoi_server は #163 から sample maps を "
      "提供しなくなったため、起動時に必ず maps_path を指定する必要があります "
      "(例: $(find-pkg-share mapoi_turtlebot3_example)/maps)。");
    throw std::runtime_error("maps_path parameter is required");
  }
  // maps_path の存在 + ディレクトリ性を起動時に検査。後段 (load_mapoi_config_file 等)
  // で不明瞭な YAML::BadFile になる前に明示的に fail させる (#163 / Cursor review medium)。
  std::error_code ec;
  bool path_exists = std::filesystem::exists(maps_path_, ec);
  if (ec) {
    // 権限不足 (EACCES) / IO エラー等で stat 自体が失敗した場合 (#170 Round 3 medium)。
    // 「存在しない」ではなく権限/IO 系として明示し、現場での復旧導線を分かりやすく。
    RCLCPP_FATAL(this->get_logger(),
      "maps_path '%s' could not be accessed: %s "
      "(権限不足 / IO エラーの可能性)。",
      maps_path_.c_str(), ec.message().c_str());
    throw std::runtime_error("maps_path access error");
  }
  if (!path_exists || !std::filesystem::is_directory(maps_path_, ec)) {
    RCLCPP_FATAL(this->get_logger(),
      "maps_path '%s' does not exist or is not a directory. "
      "正しい maps ディレクトリ path を指定してください "
      "(例: $(find-pkg-share mapoi_turtlebot3_example)/maps)。",
      maps_path_.c_str());
    throw std::runtime_error("maps_path is not a valid directory");
  }

  // #297: state file の有無で「真の初回起動」と「運用中の crash/再起動」を判別する。
  // 再起動時は起動パラメータ map_name ではなく last-selected map へ context を復元する
  // (再起動で map context が起動パラメータへ巻き戻ると、initial pose だけでなく
  // goal / route の POI 名解決も全て旧 map 基準で壊れるため)。
  const bool restart_detected = [this]() {
    if (state_path_.empty()) {
      return false;
    }
    const auto persisted_map = mapoi::read_last_map(state_path_);
    if (!persisted_map.has_value()) {
      return false;  // state file 無し = 初回起動
    }
    // 復元先が「単一 directory 名」かつ config が実在する時だけ map_name_ を差し替える。
    // 名前検証は path traversal 防止 (state file は破損・手動編集されうる外部入力。
    // separator や ".." を含む値を連結すると maps_path 外の config を load できてしまう、
    // PR #327 review medium 対応)。maps_path の付け替えや map ディレクトリ削除で state が
    // 古びているケースは起動パラメータへ fallback。
    // どちらでも「再起動」判定自体は維持し、起動時 publish は clear にする (下記)。
    const bool name_is_safe = mapoi::is_plain_directory_name(*persisted_map);
    const std::string persisted_config =
      maps_path_ + "/" + *persisted_map + "/" + config_file_;
    std::error_code state_ec;
    if (name_is_safe &&
        std::filesystem::is_regular_file(persisted_config, state_ec) && !state_ec) {
      if (*persisted_map != map_name_) {
        RCLCPP_INFO(this->get_logger(),
          "Restoring last-selected map '%s' from state file (parameter map_name '%s' を "
          "override、運用中再起動と判定、#297)。",
          persisted_map->c_str(), map_name_.c_str());
      }
      map_name_ = *persisted_map;
    } else {
      RCLCPP_WARN(this->get_logger(),
        "State file の last-selected map '%s' は不正な名前 (separator/'..' 含み) か "
        "maps_path 配下に存在しないため、起動パラメータの map_name '%s' を使用します (#297)。",
        persisted_map->c_str(), map_name_.c_str());
    }
    return true;
  }();

  load_tag_definitions();
  load_mapoi_config_file();

  // #297: 現在 map を state file に記録。初回起動でも書くことで、select_map 前に crash /
  // 再起動しても次回起動を「運用中再起動」と判別できる (same-map テレポート防止)。
  // opt-in した永続化が書けない = #297 保護が効かない構成なので、起動時の失敗は
  // maps_path (#163) と同様 fail fast (FATAL + throw) にする。publisher 生成より前に
  // 行い、fail する構成では DDS 副作用 (initialpose_poi の publish) を一切残さない。
  if (!state_path_.empty()) {
    std::string state_error;
    if (!mapoi::write_last_map(state_path_, map_name_, state_error)) {
      RCLCPP_FATAL(this->get_logger(),
        "state_path '%s' に last-selected map を書き込めません: %s "
        "(書き込み可能なディレクトリを指定するか、永続化不要なら state_path を空にしてください)。",
        state_path_.c_str(), state_error.c_str());
      throw std::runtime_error("state_path is not writable");
    }
  }

  // Publish config_path_ (transient_local QoS で latched、subscriber も transient_local に揃え
  // たので定期 publish は廃止、起動時 / select_map / reload_map_info で明示 publish する仕様に
  // 移行 (#135))。
  config_path_publisher_ = this->create_publisher<std_msgs::msg::String>(
    "mapoi/config_path", rclcpp::QoS(1).transient_local());
  publish_config_path();  // 起動時に latched 値として publish

  // Publish initial pose POI name (#144): mapoi_nav2_bridge がこれを受けて /initialpose を流す。
  // 起動時は初期 map の候補を publish し、select_map / reload では stale 防止の clear message を流す。
  // operator map switch 完了後の publish は mapoi_nav2_bridge が担当する。
  // 後起動 subscriber でも受信できるよう transient_local。
  initialpose_poi_publisher_ = this->create_publisher<mapoi_interfaces::msg::InitialPoseRequest>(
    "mapoi/initialpose_poi", rclcpp::QoS(1).transient_local());

  // 起動時の最初の map に対する initial pose POI を publish (#144)。
  // 順序保証: load_mapoi_config_file() (line 21) → publish_initial_poi_name() の順に実行。
  // pois_list_ は load 後に最新化されているので、compute_initial_poi_name の lookup は安全。
  // mapoi/initialpose_poi は transient_local QoS なので mapoi_nav2_bridge 後起動でも latched 値を
  // 受信できる。mapoi_nav2_bridge cb 側は新たに get_pois_info を fetch して名前 lookup するので、
  // mapoi_server 内の pois_list_ 更新と mapoi_nav2_bridge 側の cb 処理は順序非依存 (#149 review 補足)。
  // 起動時は requested_name = empty で default (POI list 先頭) を採用。
  //
  // #297: 運用中再起動 (state file あり) では POI ではなく clear を publish する。
  // server 再起動は新 DDS publisher の出現なので、この起動時 publish は後起動 subscriber
  // だけでなく**稼働中の localization bridge にも新規サンプルとして届き** (active push)、
  // 非空 POI を流すと走行中ロボットの自己位置が起動 map 先頭 POI へテレポートする
  // (map 切替なしの same-map 再起動でも同じ)。clear なら subscriber は無視する規約 (#154)
  // なので安全に latched 値だけを上書きできる。初回起動 (state file 無し) は従来どおり。
  if (restart_detected) {
    publish_initialpose_clear();
  } else {
    publish_initial_poi_name("");
  }

  get_pois_info_service_ = this->create_service<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info",
    std::bind(&MapoiServer::get_pois_info_service, this, std::placeholders::_1, std::placeholders::_2));

  get_route_pois_service_ = this->create_service<mapoi_interfaces::srv::GetRoutePois>("mapoi/get_route_pois",
    std::bind(&MapoiServer::get_route_pois_service, this, std::placeholders::_1, std::placeholders::_2));

  get_maps_info_service_ = this->create_service<mapoi_interfaces::srv::GetMapsInfo>("mapoi/get_maps_info",
    std::bind(&MapoiServer::get_maps_info_service, this, std::placeholders::_1, std::placeholders::_2));

  get_routes_info_service_ = this->create_service<mapoi_interfaces::srv::GetRoutesInfo>("mapoi/get_routes_info",
    std::bind(&MapoiServer::get_routes_info_service, this, std::placeholders::_1, std::placeholders::_2));

  select_map_service_ = this->create_service<mapoi_interfaces::srv::SelectMap>("mapoi/select_map",
    std::bind(&MapoiServer::select_map_service, this, std::placeholders::_1, std::placeholders::_2));

  // #211: mapoi/initialpose_poi の publish を mapoi_server 1 writer に集約するための service。
  // 外部 requester (mapoi_nav2_bridge / WebUI / RViz panel) はこの service 経由で publish を依頼し、
  // mapoi_server が唯一の writer として initialpose_poi_publisher_ から publish する。
  // initialpose_poi_publisher_ は上で構築済みなので、callback 実行時 (spin 開始後) は必ず存在する。
  request_initial_pose_service_ = this->create_service<mapoi_interfaces::srv::RequestInitialPose>(
    "mapoi/request_initial_pose",
    std::bind(&MapoiServer::request_initial_pose_service, this, std::placeholders::_1, std::placeholders::_2));

  reload_map_info_service_ = this->create_service<std_srvs::srv::Trigger>("mapoi/reload_map_info",
    std::bind(&MapoiServer::reload_map_info_service, this, std::placeholders::_1, std::placeholders::_2));

  get_tag_definitions_srv_ = this->create_service<mapoi_interfaces::srv::GetTagDefinitions>("mapoi/get_tag_definitions",
    std::bind(&MapoiServer::get_tag_definitions_service, this, std::placeholders::_1, std::placeholders::_2));

  RCLCPP_INFO(this->get_logger(), "Ready to serve. The current map_name is %s", config_path_.c_str());
}

mapoi_interfaces::msg::PointOfInterest MapoiServer::yaml_to_poi_msg(const YAML::Node & poi)
{
  mapoi_interfaces::msg::PointOfInterest msg;
  msg.name = poi["name"].as<std::string>("");
  msg.pose.position.x = poi["pose"]["x"].as<double>(0.0);
  msg.pose.position.y = poi["pose"]["y"].as<double>(0.0);
  auto yaw = poi["pose"]["yaw"].as<double>(0.0);
  msg.pose.orientation.z = std::sin(yaw / 2.0);
  msg.pose.orientation.w = std::cos(yaw / 2.0);
  // tolerance struct (Nav2 align、xy / yaw 同時に指定可能)。v0.3.0 で旧 `radius` フィールドから
  // 破壊変更で移行 (#87)。tolerance.xy は POI radius (進入判定距離) としても使われる。
  // 仕様 (#138): xy / yaw 共に >= 0.001、0 / 負値は禁止 (実用上「無反応 POI」を防ぐ)。
  // 違反時は安全側 default に補正 + WARN ログ:
  //   - tolerance struct 不在 / non-map / xy or yaw 欠落 / < 0.001: WARN + default (xy=0.5, yaw=π/4)
  //   - default は sample yaml と整合 (45° = π/4)
  constexpr double TOL_MIN = 0.001;
  constexpr double DEFAULT_TOL_XY = 0.5;
  const double DEFAULT_TOL_YAW = M_PI / 4.0;  // = 45°、sample yaml 統一値と整合
  msg.tolerance.xy = DEFAULT_TOL_XY;
  msg.tolerance.yaw = DEFAULT_TOL_YAW;
  if (!poi["tolerance"]) {
    RCLCPP_WARN(this->get_logger(),
      "POI '%s': 'tolerance' field missing; using default (xy=%.3f, yaw=%.3f rad). "
      "Old 'radius' field is no longer supported (#87).",
      msg.name.c_str(), DEFAULT_TOL_XY, DEFAULT_TOL_YAW);
  } else if (!poi["tolerance"].IsMap()) {
    RCLCPP_WARN(this->get_logger(),
      "POI '%s': 'tolerance' must be a mapping {xy, yaw}; using default (xy=%.3f, yaw=%.3f rad).",
      msg.name.c_str(), DEFAULT_TOL_XY, DEFAULT_TOL_YAW);
  } else {
    const auto & tol = poi["tolerance"];
    // NaN / +-Inf も clamp 対象 (Codex review #139 medium 対応)。yaml-cpp は ".nan" / ".inf"
    // を double に解釈するため、有限性チェックを明示的に入れる。
    if (tol["xy"]) {
      const double v = tol["xy"].as<double>(DEFAULT_TOL_XY);
      if (!std::isfinite(v) || v < TOL_MIN) {
        RCLCPP_WARN(this->get_logger(),
          "POI '%s': 'tolerance.xy' (%.6f) is non-finite or < %.3f m; clamping to %.3f m.",
          msg.name.c_str(), v, TOL_MIN, TOL_MIN);
        msg.tolerance.xy = TOL_MIN;
      } else {
        msg.tolerance.xy = v;
      }
    } else {
      RCLCPP_WARN(this->get_logger(),
        "POI '%s': 'tolerance.xy' missing; using default (%.3f).",
        msg.name.c_str(), DEFAULT_TOL_XY);
    }
    if (tol["yaw"]) {
      const double v = tol["yaw"].as<double>(DEFAULT_TOL_YAW);
      if (!std::isfinite(v) || v < TOL_MIN) {
        RCLCPP_WARN(this->get_logger(),
          "POI '%s': 'tolerance.yaw' (%.6f rad) is non-finite or < %.3f rad; clamping to %.3f rad.",
          msg.name.c_str(), v, TOL_MIN, TOL_MIN);
        msg.tolerance.yaw = TOL_MIN;
      } else {
        msg.tolerance.yaw = v;
      }
    } else {
      RCLCPP_WARN(this->get_logger(),
        "POI '%s': 'tolerance.yaw' missing; using default (%.3f rad ≒ 45 deg).",
        msg.name.c_str(), DEFAULT_TOL_YAW);
    }
  }
  msg.tags = poi["tags"].as<std::vector<std::string>>(std::vector<std::string>{});
  msg.description = poi["description"].as<std::string>("");
  return msg;
}

void MapoiServer::publish_config_path()
{
  auto msg = std_msgs::msg::String();
  msg.data = config_path_;
  config_path_publisher_->publish(msg);
}

void MapoiServer::load_mapoi_config_file()
{
  config_path_ = maps_path_ + "/" + map_name_ + "/" + config_file_;
  RCLCPP_INFO(this->get_logger(), "Loading mapoi config file: %s", config_path_.c_str());
  try {
    YAML::Node mapoi_config = YAML::LoadFile(config_path_);
    pois_list_ = mapoi_config["poi"] ? mapoi_config["poi"] : YAML::Node(YAML::NodeType::Sequence);
    routes_list_ = mapoi_config["route"] ? mapoi_config["route"] : YAML::Node(YAML::NodeType::Sequence);
    nav2_map_list_ = mapoi_config["map"] ? mapoi_config["map"] : YAML::Node(YAML::NodeType::Sequence);

    // Rebuild tag list: system tags + user tags from config
    tag_definitions_.clear();
    for (const auto & tag : system_tags_) {
      tag_definitions_.push_back(tag);
    }
    if (mapoi_config["custom_tags"]) {
      for (const auto & tag : mapoi_config["custom_tags"]) {
        mapoi_interfaces::msg::TagDefinition td;
        td.name = tag["name"].as<std::string>();
        td.description = tag["description"].as<std::string>("");
        td.is_system = false;
        tag_definitions_.push_back(td);
      }
    }
    RCLCPP_INFO(this->get_logger(), "Tag definitions: %zu system + %zu user",
                system_tags_.size(), tag_definitions_.size() - system_tags_.size());
  } catch (const YAML::BadFile & e) {
    RCLCPP_ERROR(this->get_logger(), "Failed to load mapoi config file: %s", e.what());
    rclcpp::shutdown();
  }
}

void MapoiServer::get_pois_info_service(const std::shared_ptr<mapoi_interfaces::srv::GetPoisInfo::Request> request,
          std::shared_ptr<mapoi_interfaces::srv::GetPoisInfo::Response> response)
{
  (void)request;
  if (!pois_list_ || !pois_list_.IsSequence()) {
    return;
  }
  for (const auto & poi : pois_list_) {
    response->pois_list.push_back(yaml_to_poi_msg(poi));
  }
  RCLCPP_DEBUG(this->get_logger(), "sending back response '%zu'", response->pois_list.size());
}

void MapoiServer::get_route_pois_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetRoutePois::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetRoutePois::Response> response)
{
  RCLCPP_DEBUG(this->get_logger(), "Incoming get_route_pois request: %s", request->route_name.c_str());

  // Helper to look up a POI by name in pois_list_ and append the converted msg
  // into the given vector (#143).
  auto append_poi_by_name = [this](const std::string & name,
                                    std::vector<mapoi_interfaces::msg::PointOfInterest> & target) {
    if (!pois_list_ || !pois_list_.IsSequence()) return;
    for (const auto &poi : pois_list_) {
      if (poi["name"].as<std::string>("") == name) {
        target.push_back(yaml_to_poi_msg(poi));
        return;
      }
    }
    RCLCPP_WARN(this->get_logger(),
      "Route POI '%s' not found in pois_list_; skipping.", name.c_str());
  };

  bool route_found = false;
  if (routes_list_ && routes_list_.IsSequence()) {
    for (const auto &route : routes_list_) {
      if (route["name"].as<std::string>("") != request->route_name) continue;
      route_found = true;

      // Ordered waypoints (sent to Nav2 FollowWaypoints).
      if (route["waypoints"] && route["waypoints"].IsSequence()) {
        for (const auto &wp : route["waypoints"]) {
          append_poi_by_name(wp.as<std::string>(), response->pois_list);
        }
      }

      // route.landmarks (#143): NOT navigated, but radius-monitored while this
      // route is active. Order is informational only.
      // Codex review #147 low: 参照先 POI が `landmark` tag を持つかも検証し、
      // 持たない POI を route.landmarks に書いた場合は WARN ログ (typo / spec 誤解の検知)。
      if (route["landmarks"] && route["landmarks"].IsSequence()) {
        for (const auto &lm : route["landmarks"]) {
          const std::string lm_name = lm.as<std::string>();
          const size_t before = response->landmark_pois.size();
          append_poi_by_name(lm_name, response->landmark_pois);
          if (response->landmark_pois.size() > before) {
            const auto & lm_poi = response->landmark_pois.back();
            bool has_landmark_tag = false;
            for (const auto & t : lm_poi.tags) {
              if (t == "landmark") { has_landmark_tag = true; break; }
            }
            if (!has_landmark_tag) {
              RCLCPP_WARN(this->get_logger(),
                "Route '%s' references '%s' as landmark but POI does not have 'landmark' tag.",
                request->route_name.c_str(), lm_name.c_str());
            }
          }
        }
      }

      break;
    }
  }

  // #342: route_name の typo と「route は存在するが POI 0 件」を呼び出し側が区別できるよう
  // success/error_message を返す (SelectMap パターン準拠)。POI 0 件自体は route はある
  // ので success=true のまま (呼び出し側の空 reject 経路は現行維持)。
  // routes 未定義 (config に route 節が無い / 未ロード) は typo と原因が違うため
  // error_message を分ける (Cursor review PR #361 medium)。route キー欠如は load 時に
  // 空 sequence へ正規化される (load_mapoi_config) ため size()==0 も同扱い。
  if (!route_found) {
    response->success = false;
    if (!routes_list_ || !routes_list_.IsSequence() || routes_list_.size() == 0) {
      response->error_message = "Route '" + request->route_name +
        "' not found: map '" + map_name_ + "' has no routes";
    } else {
      response->error_message = "Route '" + request->route_name +
        "' not found in map '" + map_name_ + "'";
    }
    RCLCPP_WARN(this->get_logger(), "get_route_pois: %s", response->error_message.c_str());
    return;
  }
  response->success = true;

  // debug log
  std::string wp_names;
  bool first = true;
  for (const auto &poi : response->pois_list) {
    if (!first) wp_names += ", ";
    first = false;
    wp_names += poi.name;
  }
  std::string lm_names;
  first = true;
  for (const auto &poi : response->landmark_pois) {
    if (!first) lm_names += ", ";
    first = false;
    lm_names += poi.name;
  }
  RCLCPP_DEBUG(this->get_logger(),
    "Collected route '%s': waypoints=[%s], landmarks=[%s]",
    request->route_name.c_str(), wp_names.c_str(), lm_names.c_str());
}

void MapoiServer::get_maps_info_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetMapsInfo::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetMapsInfo::Response> response)
{
  (void)request;
  RCLCPP_DEBUG(this->get_logger(), "Incoming get_maps_info request");

  std::vector<std::string> maps_list;
  for (const auto & entry : std::filesystem::directory_iterator(maps_path_)) {
    if (entry.is_directory()) {
      maps_list.push_back(entry.path().filename().string());
    }
  }

  response->maps_list = maps_list;
  response->map_name = map_name_;

  RCLCPP_DEBUG(this->get_logger(), "sending back response '%zu' maps", response->maps_list.size());
}

void MapoiServer::get_routes_info_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetRoutesInfo::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetRoutesInfo::Response> response)
{
  (void)request;
  RCLCPP_DEBUG(this->get_logger(), "Incoming get_routes_info request");

  std::vector<std::string> routes_list;
  if (routes_list_ && routes_list_.IsSequence()) {
    for (const auto &route : routes_list_) {
      routes_list.push_back(route["name"].as<std::string>(""));
    }
  }

  response->routes_list = routes_list;
  RCLCPP_DEBUG(this->get_logger(), "sending back response '%zu' routes", response->routes_list.size());
}

void MapoiServer::select_map_service(const std::shared_ptr<mapoi_interfaces::srv::SelectMap::Request> request,
          std::shared_ptr<mapoi_interfaces::srv::SelectMap::Response> response)
{
  auto map_name_new = request->map_name;
  RCLCPP_INFO(this->get_logger(), "Incoming select_map request: %s", map_name_new.c_str());
  if (map_name_new.empty()) {
    response->success = false;
    response->error_message = "map_name is empty";
    return;
  }
  // #328: map_name は maps_path 直下の「単一 path segment」のみ受け付ける
  // (is_plain_directory_name の契約: separator 入り・"."・".." 単体は reject、
  // "..hidden" のように ".." を含むだけの単一 segment は許可)。separator や ".." 単体を
  // そのまま連結すると、maps_path 外の任意 config YAML の load (下の config_path_new) と、
  // response->nav2_map_urls 経由で Nav2 map_server の LoadMap 先も maps_path 外を
  // 指せてしまう (state file 側は PR #327 で対応済みの同種 path traversal。こちらは
  // operator UI からの service 入力経路)。
  if (!mapoi::is_plain_directory_name(map_name_new)) {
    response->success = false;
    response->error_message =
      "Invalid map_name (must be a single path segment): " + map_name_new;
    RCLCPP_WARN(this->get_logger(),
      "select_map: rejected map_name '%s' (単一 path segment のみ受け付け: "
      "separator 入り / '.' / '..' は不可、#328)。",
      map_name_new.c_str());
    return;
  }
  const std::string config_path_new = maps_path_ + "/" + map_name_new + "/" + config_file_;
  std::error_code ec;
  if (!std::filesystem::exists(config_path_new, ec) ||
      !std::filesystem::is_regular_file(config_path_new, ec) || ec) {
    response->success = false;
    response->error_message = "Config file not found: " + config_path_new;
    return;
  }

  map_name_ = map_name_new;
  load_mapoi_config_file();
  // #297: operator の map 選択を永続化 (crash/再起動時の context 復元用)。
  persist_last_selected_map();
  response->config_path = config_path_;
  response->resolved_initial_poi_name = resolve_initial_poi_name(request->initial_poi_name);

  if (nav2_map_list_ && nav2_map_list_.IsSequence()) {
    for (const auto & map : nav2_map_list_) {
      const std::string node_name = map["node_name"].as<std::string>("");
      const std::string map_file = map["map_file"].as<std::string>("");
      if (node_name.empty() || map_file.empty()) {
        RCLCPP_WARN(this->get_logger(),
          "Map entry for '%s' is missing node_name or map_file; skipping.",
          map_name_new.c_str());
        continue;
      }
      response->nav2_node_names.push_back(node_name);
      response->nav2_map_urls.push_back(maps_path_ + "/" + map_name_new + "/" + map_file);
    }
  } else {
    RCLCPP_WARN(this->get_logger(),
      "No Nav2 map entries defined in config for '%s'. Editor context selection remains valid.",
      map_name_new.c_str());
  }

  // mapoi/config_path を再 publish (transient_local で latched 値更新)。subscriber は新 path を
  // 受け取って table / ComboBox を再 fetch する (#135)。
  publish_config_path();

  // select_map は Nav2-free の context 更新入口。ここでは AMCL に initial pose を流さず、
  // stale な latched initial pose だけを clear する。operator mode では mapoi_nav2_bridge が
  // Nav2 LoadMap 成功後に response->resolved_initial_poi_name を mapoi/initialpose_poi へ publish する。
  publish_initialpose_clear();

  RCLCPP_INFO(this->get_logger(), "Selected map context: %s", map_name_.c_str());
  response->success = true;
}

void MapoiServer::request_initial_pose_service(
    const std::shared_ptr<mapoi_interfaces::srv::RequestInitialPose::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::RequestInitialPose::Response> response)
{
  // #211: 外部 requester の依頼を受けて mapoi/initialpose_poi に publish する。mapoi_server が
  // 唯一の writer になることで transient_local の latched cache が単一化され、clear (poi_name 空)
  // が後起動 subscriber に対しても真に last-write-wins で効く (= クロス writer の stale 競合が
  // 構造的に消える)。本 callback は publish に徹し、POI resolve / landmark 排他は従来どおり
  // subscriber (mapoi_amcl_localization_bridge) 側で処理する (#149 round 10 の map 非検証も維持)。
  // #299: map_name が非空かつ現在 map と不一致な要求は publish せず reject する。map switch の
  // 過渡窓 (A→B 連打で server context は既に B、bridge は A の callback を実行中) に stale な
  // {旧 map, POI} が latched publish されるのを入口で遮断する。server は自分の現在 map を
  // 原子的に知っているため、#149 round 10 の制約 (bridge が config_path 未受信のせいで正当
  // request を誤棄却する) には抵触しない。空 map_name は map 不明の requester として従来どおり透過。
  if (!request->map_name.empty() && request->map_name != map_name_) {
    response->success = false;
    response->error_message = "map_name '" + request->map_name +
      "' does not match current map '" + map_name_ + "'";
    RCLCPP_WARN(this->get_logger(),
      "request_initial_pose: rejected request for map '%s' (current map is '%s', #299).",
      request->map_name.c_str(), map_name_.c_str());
    return;
  }
  auto msg = mapoi_interfaces::msg::InitialPoseRequest();
  msg.map_name = request->map_name;
  msg.poi_name = request->poi_name;
  initialpose_poi_publisher_->publish(msg);
  if (request->poi_name.empty()) {
    RCLCPP_INFO(this->get_logger(),
      "request_initial_pose: cleared latched mapoi/initialpose_poi for map '%s' (#211).",
      request->map_name.c_str());
  } else {
    RCLCPP_INFO(this->get_logger(),
      "request_initial_pose: published initial pose POI '%s' for map '%s' (#211).",
      request->poi_name.c_str(), request->map_name.c_str());
  }
  response->success = true;
}

void MapoiServer::reload_map_info_service(
    const std::shared_ptr<std_srvs::srv::Trigger::Request> request,
    std::shared_ptr<std_srvs::srv::Trigger::Response> response)
{
  (void)request;
  load_mapoi_config_file();
  // mapoi/config_path を再 publish (transient_local で latched 値更新)。subscriber が
  // table / ComboBox を再 fetch することで save 後の内容更新が反映される (#135)。
  publish_config_path();
  // reload は POI 編集後の再読込 trigger。ここで `mapoi/initialpose_poi` を「採用候補あり」で
  // 再 publish すると、mapoi_nav2_bridge 側で /initialpose が再配信され **運用中の自己位置が
  // 巻き戻される** 回帰がある (Cursor review #149 round 4 medium 対応)。
  // 一方で latched message は古い POI 名のまま残り、reload 後に mapoi_nav2_bridge が再起動 / 再 connection
  // すると stale な POI 名が配信される問題が残っていた (#154)。
  // 対策: skip message (poi_name 空、subscriber 側で無視される規約) を明示 publish して
  // transient_local の latched 値を上書きする。運用中 subscriber には影響なく (空は無視)、
  // 後起動 subscriber も古い POI 名を拾わない。
  // 初期姿勢の (再) 設定は operator map switch または手動経路 (RViz / WebUI /
  // mapoi/initialpose_poi 直接 publish) を使うのが正しい運用。
  publish_initialpose_clear();
  response->success = true;
  response->message = config_path_;
  RCLCPP_INFO(this->get_logger(), "Reloaded mapoi config: %s", config_path_.c_str());
}

std::string MapoiServer::resolve_initial_poi_name(const std::string & requested_name)
{
  // requested_name は select_map.initial_poi_name (空 = default、POI list 先頭採用)。
  // shared state を持たず、呼び出し側が直接渡す形にして thread safety / lifecycle race を排除
  // (Cursor review #149 round 2 high 対応)。選定 logic は bridge 2 つと共通化済 (#150)。
  const std::string target = mapoi::select_initial_poi_name(pois_list_, requested_name);
  // ログ順序: target.empty() の WARN は下流に任せ、ここでは「requested 不採用かつ fallback 候補が
  // 存在する」ケースのみ WARN を出す (#149 round 5 low: target.empty() 時に紛らわしい両 WARN が
  // 出るのを避ける)。
  if (!requested_name.empty() && !target.empty() && target != requested_name) {
    RCLCPP_WARN(this->get_logger(),
      "Requested initial_poi_name '%s' was not adopted (not found / landmark / invalid pose); "
      "falling back to POI list first ('%s').",
      requested_name.c_str(), target.c_str());
  }
  if (target.empty()) {
    RCLCPP_WARN(this->get_logger(),
      "No POI available for initial pose in map '%s'.",
      map_name_.c_str());
    return "";
  }
  return target;
}

void MapoiServer::publish_initial_poi_name(const std::string & requested_name)
{
  const std::string target = resolve_initial_poi_name(requested_name);
  if (target.empty()) {
    return;
  }
  auto msg = mapoi_interfaces::msg::InitialPoseRequest();
  msg.map_name = map_name_;
  msg.poi_name = target;
  initialpose_poi_publisher_->publish(msg);
  RCLCPP_INFO(this->get_logger(),
    "Published initial pose POI name '%s' for map '%s' (#144).",
    target.c_str(), map_name_.c_str());
}

void MapoiServer::publish_initialpose_clear()
{
  // initialpose_poi_publisher_ は constructor で先に生成済み (publish_initial_poi_name と同じ
  // invariant)。service callback は spin 開始後にしか呼ばれないため、reload_map_info_service
  // 経由でここに来た時点で publisher は必ず存在する (#174 review high 対応)。
  // poi_name 空 = 「採用候補なし、subscriber は無視」(InitialPoseRequest.msg コメント参照)。
  // transient_local depth=1 の latched 値をこの skip message で上書きすることで、
  // reload 直前の古い POI 名が後起動の subscriber に latched 配信される stale 問題を排除する。
  auto msg = mapoi_interfaces::msg::InitialPoseRequest();
  msg.map_name = map_name_;
  msg.poi_name = "";
  initialpose_poi_publisher_->publish(msg);
  RCLCPP_INFO(this->get_logger(),
    "Cleared latched mapoi/initialpose_poi for map '%s' (#154 stale guard on reload).",
    map_name_.c_str());
}

void MapoiServer::persist_last_selected_map()
{
  if (state_path_.empty()) {
    return;
  }
  std::string state_error;
  if (!mapoi::write_last_map(state_path_, map_name_, state_error)) {
    // 運用中は稼働継続を優先して WARN に留める (disk full 等の一過性障害で select_map を
    // 失敗させない)。書けない期間に crash すると復元先が 1 世代古くなるが、復元時は
    // clear publish なので自己位置テレポートは起きない (context が古いだけ)。
    RCLCPP_WARN(this->get_logger(),
      "Failed to persist last-selected map '%s' (#297): %s",
      map_name_.c_str(), state_error.c_str());
  }
}

void MapoiServer::load_tag_definitions()
{
  // system tag はコンパイル時定数 mapoi::kSystemTags から構築 (#191)。
  // user tag は load_mapoi_config_file() 内で custom_tags から merge される。
  system_tags_.clear();
  for (const auto & def : mapoi::kSystemTags) {
    mapoi_interfaces::msg::TagDefinition td;
    td.name = def.name;
    td.description = def.description;
    td.is_system = true;
    system_tags_.push_back(td);
  }
  RCLCPP_INFO(this->get_logger(), "Loaded %zu system tag definitions (compiled-in)",
              system_tags_.size());
}

void MapoiServer::get_tag_definitions_service(
    const std::shared_ptr<mapoi_interfaces::srv::GetTagDefinitions::Request> request,
    std::shared_ptr<mapoi_interfaces::srv::GetTagDefinitions::Response> response)
{
  (void)request;
  response->definitions = tag_definitions_;
  RCLCPP_DEBUG(this->get_logger(), "Sending %zu tag definitions", tag_definitions_.size());
}

#ifndef UNIT_TEST
int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  // try は ctor のみに限定する (#170 Round 4 high): spin 中 (callback 等) の例外まで
  // catch すると std::terminate ベースのデバッグ性が落ちるため。MapoiServer のコンストラ
  // クタは設定不備 (maps_path REQUIRED 違反 / 不在 dir 等) 時に std::runtime_error を
  // throw する (#163)。fatal log は ctor 内で出力済 + ここで what() を stderr に出して
  // clean shutdown + exit code 1。
  std::shared_ptr<MapoiServer> node;
  try {
    node = std::make_shared<MapoiServer>();
  } catch (const std::exception & e) {
    fprintf(stderr, "[mapoi_server] FATAL: %s\n", e.what());
    rclcpp::shutdown();
    return 1;
  }
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
#endif
