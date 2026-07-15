#include "mapoi_server/mapoi_rviz2_publisher.hpp"

using namespace std::chrono_literals;
using std::placeholders::_1;
using std::placeholders::_2;


MapoiRviz2Publisher::MapoiRviz2Publisher() : Node("mapoi_rviz2_publisher") {
  id_buf_ = 0;

  // POI tolerance visualization (#136 / #179) を表示するか。
  // 描画 layer (false で全 POI 抑制):
  //   - xy 判定円 outline (細い実線、薄め): tolerance.xy = ロボット進入判定の境界
  //   - yaw 制約: 0 < tolerance.yaw < π は扇形 (wedge)、>= π (yaw 不問 = 全方位) は
  //     塗りつぶし円 (disc) で重ね描き (#267)
  //   - pause overlay (点線 dot): pause tag POI の xy 円沿いに dot pattern
  // Editor 中心の使い方や RViz が情報過多な時に false にする想定。default true。
  this->declare_parameter<bool>("show_tolerance_sector", true);

  // POI label の表示形式: "index" (POI Editor 行番号、1-based) / "name" / "both" (= "<index>: <name>") / "none"。
  // default "index" は WebUI 上の行と RViz label を直接対応させ、文字長を抑えて重なりを減らす。
  // ("off" は ros2 param CLI で bool として解釈されるため "none" を採用)
  this->declare_parameter<std::string>("poi_label_format", "index");

  // Route marker の表示形式: "all" (全 route 表示、active は強調) / "selected" (active のみ) / "none"。
  // default "selected": RViz 起動時の clutter を抑え、user が選択した route だけを示す。
  // 全 route を見たい場合は ros2 param set /mapoi_rviz2_publisher route_display_mode all で切替。
  this->declare_parameter<std::string>("route_display_mode", "selected");

  marker_pois_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("mapoi/markers/pois", 10);
  marker_routes_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("mapoi/markers/routes", 10);

  this->poi_client_ = this->create_client<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info");
  this->routes_info_client_ = this->create_client<mapoi_interfaces::srv::GetRoutesInfo>("mapoi/get_routes_info");
  this->route_pois_client_ = this->create_client<mapoi_interfaces::srv::GetRoutePois>("mapoi/get_route_pois");

  highlight_goal_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/highlight/goal", 10,
    std::bind(&MapoiRviz2Publisher::on_highlight_goal_received, this, _1));
  highlight_route_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/highlight/route", 10,
    std::bind(&MapoiRviz2Publisher::on_highlight_route_received, this, _1));

  // mapoi/config_path 変化検出で POI list を再取得 (WebUI / Panel 並び替え保存 / map switch 対応)。
  // QoS は mapoi_nav2_bridge と同じ transient_local。後起動でも latched 値を受信できる。
  config_path_sub_ = this->create_subscription<std_msgs::msg::String>(
    "mapoi/config_path", rclcpp::QoS(1).transient_local(),
    std::bind(&MapoiRviz2Publisher::on_config_path_changed, this, _1));

  // 初期化シーケンスをデッドロック回避のため少し遅延させて開始
  this->init_timer_ = this->create_wall_timer(100ms, std::bind(&MapoiRviz2Publisher::start_sequence, this));

  timer_ = this->create_wall_timer(1s, std::bind(&MapoiRviz2Publisher::timer_callback, this));
}


void MapoiRviz2Publisher::start_sequence()
{
  this->init_timer_->cancel();

  if (!poi_client_->wait_for_service(1s)) {
    if (!rclcpp::ok()) {
      RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for services.");
      return;
    }
    RCLCPP_INFO(this->get_logger(), "Services not available, retrying...");
    this->init_timer_->reset();
    return;
  }

  RCLCPP_INFO(this->get_logger(), "Requesting POI Info...");
  request_pois_list();
  request_routes_info();
}

void MapoiRviz2Publisher::request_pois_list()
{
  auto request = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();
  poi_client_->async_send_request(
    request, std::bind(&MapoiRviz2Publisher::on_poi_received, this, _1));
}

void MapoiRviz2Publisher::request_routes_info()
{
  // get_routes_info → 各 route について get_route_pois の 2 段 fetch。
  // 世代番号 (routes_fetch_generation_) を increment して capture することで、後続 fan-out が始まった
  // 時点で旧世代の callback を stale 判定し all_routes_ への書き込みを drop する
  // (Codex round 1 high 対策: stale callback による旧 map / 新 map の route 混在防止)。
  // service 未起動なら skip (config_path 通知の度に再試行されるので自然回復)。
  if (!routes_info_client_->service_is_ready()) {
    RCLCPP_WARN(this->get_logger(), "mapoi/get_routes_info service not ready, skipping route fetch.");
    return;
  }
  const size_t my_gen = ++routes_fetch_generation_;
  auto request = std::make_shared<mapoi_interfaces::srv::GetRoutesInfo::Request>();
  routes_info_client_->async_send_request(
    request,
    [this, my_gen](rclcpp::Client<mapoi_interfaces::srv::GetRoutesInfo>::SharedFuture f) {
      on_routes_info_received(my_gen, f);
    });
}

void MapoiRviz2Publisher::on_routes_info_received(
  size_t my_generation,
  rclcpp::Client<mapoi_interfaces::srv::GetRoutesInfo>::SharedFuture future)
{
  // stale check: 後続 fan-out が走っていたら旧世代の応答を捨てる
  if (my_generation != routes_fetch_generation_) {
    return;
  }
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "Failed to get Routes Info.");
    return;
  }
  RCLCPP_INFO(this->get_logger(), "Received %zu route names (gen=%zu).",
    result->routes_list.size(), my_generation);

  // 空 route list は all_routes_ を即時 clear (削除された route の取り残し防止)
  if (result->routes_list.empty()) {
    std::lock_guard<std::mutex> lock(data_mutex_);
    all_routes_.clear();
    marker_data_dirty_ = true;  // #402: route 全消え → 次 tick で再構築 (DELETEALL 経路含む)
    return;
  }

  // pending map に集約してから lock 下で all_routes_ に swap (timer_callback への部分公開を防ぐ)。
  // shared_ptr で per-route lambda 間で共有、最後の callback で swap する。
  using RouteMap = std::map<std::string, std::vector<mapoi_interfaces::msg::PointOfInterest>>;
  auto pending = std::make_shared<RouteMap>();
  auto remaining = std::make_shared<size_t>(result->routes_list.size());

  for (const auto & route_name : result->routes_list) {
    auto req = std::make_shared<mapoi_interfaces::srv::GetRoutePois::Request>();
    req->route_name = route_name;
    route_pois_client_->async_send_request(
      req,
      [this, route_name, my_generation, pending, remaining]
      (rclcpp::Client<mapoi_interfaces::srv::GetRoutePois>::SharedFuture f) {
        // stale check (per-route): 後続 fan-out が走っていたら結果を捨てる
        if (my_generation != routes_fetch_generation_) {
          return;
        }
        auto r = f.get();
        if (r && r->success) {
          (*pending)[route_name] = r->pois_list;
          RCLCPP_INFO(this->get_logger(), "Route '%s': %zu waypoints (gen=%zu).",
            route_name.c_str(), r->pois_list.size(), my_generation);
        } else if (r) {
          // #342: route が見つからない (typo 等)。描画対象から skip する。
          RCLCPP_WARN(this->get_logger(), "Route '%s' not found: %s (gen=%zu).",
            route_name.c_str(), r->error_message.c_str(), my_generation);
        } else {
          RCLCPP_ERROR(this->get_logger(), "Failed to get route pois for '%s' (gen=%zu).",
            route_name.c_str(), my_generation);
        }
        // 全 route 集約済みになったら lock 下で all_routes_ に swap
        if (--(*remaining) == 0) {
          std::lock_guard<std::mutex> lock(data_mutex_);
          all_routes_ = std::move(*pending);
          marker_data_dirty_ = true;  // #402: route 構成確定 → 次 tick で再構築
        }
      });
  }
}

void MapoiRviz2Publisher::on_config_path_changed(const std_msgs::msg::String::SharedPtr msg)
{
  // mapoi_server は config path 文字列を event 駆動で publish する (周期 publish は #135 で廃止し、
  // subscriber を transient_local に揃えた。起動時 / select_map / reload_map_info の 3 箇所)。
  // このうち reload_map_info (WebUI/Panel Save) は path 不変・内容のみ変更なので、path だけで dedup
  // すると map switch (path 変更) は拾えるが Save を取りこぼす。YAML の mtime も併せて比較し両 case を検出する。
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
  // stat 失敗時は dedup 不能とみなし fetch を試みる (起動 race などで一時的に発生し得る)

  RCLCPP_INFO(this->get_logger(), "Map config changed: %s — refreshing POI list.", current_path.c_str());

  // config 変更を検出したら fan-out 実行可否に関わらず routes_fetch_generation_ を進めて
  // 旧 fan-out の pending callback を全て stale 化する (Codex round 2 high 対策: service 未 ready で
  // 早期 return する経路で gen が進まないと、旧 config の callback が my_gen == current_gen を満たして
  // 旧 route set を swap してしまう窓が残る)。
  // request_routes_info() 側でも increment するため fan-out 成功時は double increment になるが、
  // 単調増加のため意味的には正しく、無害。
  ++routes_fetch_generation_;

  // 起動直後は service が未起動の場合がある。POI / routes_info / route_pois 全てが ready の時のみ
  // guard を更新して fan-out 開始する (Codex round 1 medium 対策: route 系 service だけ未 ready の
  // 状態で guard が進むと、同じ path+mtime が dedup で skip され route fetch の自然 retry が無くなる)。
  // 一つでも未 ready なら guard 据え置き → 次回 publish で再試行。
  if (!poi_client_->service_is_ready() ||
      !routes_info_client_->service_is_ready() ||
      !route_pois_client_->service_is_ready()) {
    RCLCPP_WARN(this->get_logger(),
      "Some service not ready (poi=%d, routes_info=%d, route_pois=%d), skipping refresh.",
      poi_client_->service_is_ready(),
      routes_info_client_->service_is_ready(),
      route_pois_client_->service_is_ready());
    return;
  }

  last_config_path_ = current_path;
  last_config_mtime_ = current_mtime;
  request_pois_list();
  request_routes_info();  // POI と同様、map switch / Save で route 構成も変わる可能性あり
}

void MapoiRviz2Publisher::on_poi_received(rclcpp::Client<mapoi_interfaces::srv::GetPoisInfo>::SharedFuture future)
{
  auto result = future.get();
  if (!result) {
    RCLCPP_ERROR(this->get_logger(), "Failed to get Pois Info.");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(data_mutex_);
    pois_list_ = result->pois_list;
    marker_data_dirty_ = true;  // #402: POI 更新 → 次 tick でフル再構築
  }
  RCLCPP_INFO(this->get_logger(), "Received %zu POIs.", pois_list_.size());
}


void MapoiRviz2Publisher::on_highlight_goal_received(const std_msgs::msg::String::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(data_mutex_);
  highlighted_goal_names_.clear();
  if (!msg->data.empty()) {
    highlighted_goal_names_.insert(msg->data);
  }
  marker_data_dirty_ = true;  // #402: goal highlight 変更 → POI 色が変わるので再構築
}

void MapoiRviz2Publisher::on_highlight_route_received(const std_msgs::msg::String::SharedPtr msg)
{
  std::lock_guard<std::mutex> lock(data_mutex_);
  highlighted_route_names_.clear();
  highlighted_route_ordered_.clear();
  if (!msg->data.empty()) {
    std::istringstream ss(msg->data);
    std::string token;
    int order = 1;
    while (std::getline(ss, token, ',')) {
      if (!token.empty()) {
        highlighted_route_names_[token] = order++;
        highlighted_route_ordered_.push_back(token);
      }
    }
  }
  marker_data_dirty_ = true;  // #402: route highlight 変更 → route 線/方向矢印が変わるので再構築
}

void MapoiRviz2Publisher::timer_callback(){
  std::lock_guard<std::mutex> lock(data_mutex_);

  // --- 差分ゲート (#402) ---
  // 1Hz でフル再構築 (cos/sin/頂点列生成) するのは無駄なので、データ (POI/route/highlight/
  // 描画 parameter) が変わった tick だけ marker を再構築する。それ以外の tick は前回構築を
  // cache しておき、header.stamp だけ現在時刻に差し替えて再 publish する。
  // 再 publish を続ける理由: 購読側 QoS が volatile なので、後起動の RViz へ marker を届けるには
  // marker 自体を 1Hz で送り続ける必要がある (stamp だけ更新すれば形状再計算は不要)。
  // 描画 parameter (show_tolerance_sector / poi_label_format / route_display_mode) の変化は
  // subscription を持たないため、ここで前回値と比較して dirty を立てる (get_parameter は元々
  // 毎 tick 呼んでおりコスト増はない)。
  const bool show_sector = this->get_parameter("show_tolerance_sector").as_bool();
  const std::string label_format = this->get_parameter("poi_label_format").as_string();
  const std::string route_mode = this->get_parameter("route_display_mode").as_string();
  if (show_sector != last_show_sector_ ||
      label_format != last_poi_label_format_ ||
      route_mode != last_route_display_mode_) {
    marker_data_dirty_ = true;
    last_show_sector_ = show_sector;
    last_poi_label_format_ = label_format;
    last_route_display_mode_ = route_mode;
  }

  // 購読者ゼロスキップ (#402): pois / routes をそれぞれ独立に判定する。購読者ゼロの間は構築も
  // publish もしないが、dirty フラグは「消費しない」(= クリアしない)。購読者が復帰した最初の
  // tick で dirty が立っていれば必ずフル構築される。dirty=false のまま復帰した場合も cache は
  // データ不変を意味するので、cache を stamp 更新して publish すれば正しい (下の clean 経路)。
  const bool pois_has_sub = marker_pois_pub_->get_subscription_count() > 0;
  const bool routes_has_sub = marker_routes_pub_->get_subscription_count() > 0;

  // 両 topic とも購読者ゼロなら dirty の有無に関わらず即 return する (RViz 未接続時の
  // 毎 tick コストを default marker 構築含めゼロにする)。dirty はここでも消費しないため、
  // 購読者復帰後の最初の tick で必ずフル構築される。
  if (!pois_has_sub && !routes_has_sub) {
    return;
  }

  // clean tick (dirty=false): 幾何再計算せず cache の stamp だけ更新して再 publish する。
  // marker 数は不変なので DELETEALL 分岐 (id_buf_ / route_id_buf_) は通らない
  // (id_buf_ / route_id_buf_ は dirty tick でのみ更新されるため整合する)。
  if (!marker_data_dirty_) {
    const auto now = rclcpp::Clock().now();
    if (pois_has_sub) {
      for (auto & m : cached_ma_pois_.markers) m.header.stamp = now;
      marker_pois_pub_->publish(cached_ma_pois_);
    }
    if (routes_has_sub) {
      for (auto & m : cached_ma_routes_.markers) m.header.stamp = now;
      marker_routes_pub_->publish(cached_ma_routes_);
    }
    return;
  }

  // --- dirty tick: 従来通りフル構築する ---
  // publish markers on rviz
  visualization_msgs::msg::Marker default_arrow_marker;
  default_arrow_marker.header.frame_id = "map";
  default_arrow_marker.header.stamp = rclcpp::Clock().now();
  default_arrow_marker.action = visualization_msgs::msg::Marker::ADD;
  default_arrow_marker.type = visualization_msgs::msg::Marker::ARROW;
  // 矢印は扇形 (#136) の方向補助なので細く短く固定 (POI 中心線として yaw のみ示す)。
  // 比率 (length:shaft:head) = 0.15:0.04:0.04。radius 連動の倍率は廃止 (arrow_size_ratio 廃止)。
  default_arrow_marker.scale.x = 0.15; default_arrow_marker.scale.y = 0.04; default_arrow_marker.scale.z = 0.04;
  default_arrow_marker.color.r = 0.0; default_arrow_marker.color.g = 1.0; default_arrow_marker.color.b = 0.0; default_arrow_marker.color.a = 0.7;
  default_arrow_marker.lifetime.sec = 2.0;

  // tolerance visualization (#136 / #179) の表示制御 (show_sector) と POI label format
  // (label_format: "index" / "name" / "both" / "none") は timer_callback 冒頭で取得済み
  // (差分ゲートの parameter 変化検出と共有、#402)。
  // - show_sector: false で全 POI の円 + 扇形 + pause overlay を抑制。yaw sector だけでなく
  //   POI tolerance layer 全体を制御する。
  // - label_format: 空文字列 ("none") を返した場合は label を生成しない。
  auto build_label = [&label_format](size_t index_one_based, const std::string & name) -> std::string {
    if (label_format == "none") return "";
    if (label_format == "name") return name;
    if (label_format == "both") return std::to_string(index_one_based) + ": " + name;
    // default "index" (および未知の値)
    return std::to_string(index_one_based);
  };

  visualization_msgs::msg::Marker default_text_marker = default_arrow_marker;
  default_text_marker.type = visualization_msgs::msg::Marker::TEXT_VIEW_FACING;
  default_text_marker.scale.x = 0.2; default_text_marker.scale.y = 0.2; default_text_marker.scale.z = 0.2;
  default_text_marker.color.r = 0.0; default_text_marker.color.g = 0.0; default_text_marker.color.b = 0.0; default_text_marker.color.a = 1.0;

  visualization_msgs::msg::MarkerArray ma_pois;
  int id = 0;

  // 扇形 (sector) の床面 z (map / costmap よりわずかに上、0.01m)
  constexpr double SECTOR_Z = 0.01;

  // Pose の orientation (quaternion) から 2D yaw を取り出す。
  // tf2 を引かずに済むよう atan2(2(wz+xy), 1-2(y^2+z^2)) を直書き。
  // 前提: POI は 2D (roll/pitch=0)、quaternion は正規化済 (mapoi_server / Editor 側で保証)。
  // 3D POI 対応や未正規化入力が必要になれば tf2 経由に書き換える。
  auto get_yaw_from_pose = [](const geometry_msgs::msg::Pose & p) -> double {
    const double w = p.orientation.w;
    const double x = p.orientation.x;
    const double y = p.orientation.y;
    const double z = p.orientation.z;
    return std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
  };

  // xy 判定円 outline を細い実線で描画する helper (#179)。
  // - radius = tolerance.xy = ロボット進入判定の境界 (yaw 不問)
  // - 扇形 (yaw 制約) と重ね描きする前提で、薄めの alpha + 細い実線で控えめに
  // - LINE_STRIP の頂点列で 36 角形 (10 度刻み) を構成
  auto poi_marker_ns = [](const std::string & layer, const std::string & poi_name) {
    return layer + "/" + poi_name;
  };

  auto add_radius_circle = [&](const geometry_msgs::msg::Pose & pose, double radius,
                               float r, float g, float b, float a,
                               int marker_id,
                               const std::string & marker_ns,
                               visualization_msgs::msg::MarkerArray & target) {
    if (radius <= 0.0) return;
    constexpr int N_SEG = 36;
    visualization_msgs::msg::Marker m;
    m.header.frame_id = "map";
    m.header.stamp = rclcpp::Clock().now();
    m.action = visualization_msgs::msg::Marker::ADD;
    m.type = visualization_msgs::msg::Marker::LINE_STRIP;
    m.id = marker_id;
    m.ns = marker_ns;
    m.pose.orientation.w = 1.0;
    m.scale.x = 0.02;  // 細い実線 (扇形 stroke と同じ太さ)
    m.color.r = r; m.color.g = g; m.color.b = b; m.color.a = a;
    m.lifetime.sec = 2;
    m.points.reserve(N_SEG + 1);
    for (int i = 0; i <= N_SEG; ++i) {
      const double angle = 2.0 * M_PI * i / N_SEG;
      geometry_msgs::msg::Point p;
      p.x = pose.position.x + radius * std::cos(angle);
      p.y = pose.position.y + radius * std::sin(angle);
      p.z = SECTOR_Z;
      m.points.push_back(p);
    }
    target.markers.push_back(m);
  };

  // 扇形 (sector) marker を描画する helper (#136 / #179)。
  // - radius = tolerance.xy、扇角 = 2 * yaw_tolerance、中心線 = pose.yaw
  // - 0 < yaw_tolerance < π の時のみ描画。yaw 不問 (それ以外の値) は呼び出し側で
  //   add_radius_circle に分岐させる (#179: 円 + 扇形重ね描き)
  // - fill_alpha > 0 → TRIANGLE_LIST で塗り (waypoint 用)
  // - stroke_alpha > 0 → LINE_STRIP で境界線 (中心 → 弧 → 中心、yaw 範囲を半径線で強調)
  // 戻り値: target.markers に push した marker 数 (id 消費数、yaw 不問入力は 0)
  auto add_sector = [&](const geometry_msgs::msg::Pose & pose,
                        double radius, double yaw_tolerance,
                        float r, float g, float b,
                        float fill_alpha, float stroke_alpha,
                        int marker_id_base,
                        const std::string & marker_ns,
                        visualization_msgs::msg::MarkerArray & target) -> int {
    if (radius <= 0.0) return 0;
    if (yaw_tolerance <= 0.0 || yaw_tolerance >= M_PI) return 0;

    const double half_angle = yaw_tolerance;
    const double yaw_center = get_yaw_from_pose(pose);
    const double total_angle = 2.0 * half_angle;
    // 弧の頂点数: 0.1 rad 刻み相当 (約 5.7 度)、最低 8 (扇形でも視認可能)
    const int n_seg = std::max(8, static_cast<int>(std::ceil(total_angle / 0.1)));

    const double start_angle = yaw_center - half_angle;
    std::vector<geometry_msgs::msg::Point> arc_pts;
    arc_pts.reserve(n_seg + 1);
    for (int i = 0; i <= n_seg; ++i) {
      const double a = start_angle + total_angle * i / n_seg;
      geometry_msgs::msg::Point p;
      p.x = pose.position.x + radius * std::cos(a);
      p.y = pose.position.y + radius * std::sin(a);
      p.z = SECTOR_Z;
      arc_pts.push_back(p);
    }

    geometry_msgs::msg::Point center;
    center.x = pose.position.x;
    center.y = pose.position.y;
    center.z = SECTOR_Z;

    int n_added = 0;

    if (fill_alpha > 0.0f) {
      visualization_msgs::msg::Marker m;
      m.header.frame_id = "map";
      m.header.stamp = rclcpp::Clock().now();
      m.action = visualization_msgs::msg::Marker::ADD;
      m.type = visualization_msgs::msg::Marker::TRIANGLE_LIST;
      m.id = marker_id_base + n_added;
      m.ns = marker_ns;
      m.pose.orientation.w = 1.0;
      m.scale.x = m.scale.y = m.scale.z = 1.0;
      m.color.r = r; m.color.g = g; m.color.b = b; m.color.a = fill_alpha;
      m.lifetime.sec = 2;
      m.points.reserve(static_cast<size_t>(n_seg) * 3);
      for (int i = 0; i < n_seg; ++i) {
        m.points.push_back(center);
        m.points.push_back(arc_pts[i]);
        m.points.push_back(arc_pts[i + 1]);
      }
      target.markers.push_back(m);
      n_added += 1;
    }

    if (stroke_alpha > 0.0f) {
      visualization_msgs::msg::Marker m;
      m.header.frame_id = "map";
      m.header.stamp = rclcpp::Clock().now();
      m.action = visualization_msgs::msg::Marker::ADD;
      m.type = visualization_msgs::msg::Marker::LINE_STRIP;
      m.id = marker_id_base + n_added;
      m.ns = marker_ns;
      m.pose.orientation.w = 1.0;
      m.scale.x = 0.02;  // line width (m)、xy 円 outline と同じ太さ
      m.color.r = r; m.color.g = g; m.color.b = b; m.color.a = stroke_alpha;
      m.lifetime.sec = 2;
      // 扇形の境界: 中心 → 弧端 → 弧上 → 弧端 → 中心 (半径線で yaw 範囲を強調)
      m.points.reserve(arc_pts.size() + 2);
      m.points.push_back(center);
      for (const auto & p : arc_pts) m.points.push_back(p);
      m.points.push_back(center);
      target.markers.push_back(m);
      n_added += 1;
    }

    return n_added;
  };

  // yaw 不問 (tolerance.yaw >= π = 全方位) を塗りつぶし円 (disc) で描画する helper (#267)。
  // add_sector は yaw 不問を return 0 でスキップするため、その場合に円 outline だけだと
  // 「描画されていない?」と誤認される (WebUI と同じ feedback)。扇形 (wedge) が広がりきって
  // 円になったもの、という連続的な見た目で「全方位 OK」を明示する。TRIANGLE_LIST で全周を塗る。
  auto add_filled_disc = [&](const geometry_msgs::msg::Pose & pose, double radius,
                             float r, float g, float b, float fill_alpha,
                             int marker_id,
                             const std::string & marker_ns,
                             visualization_msgs::msg::MarkerArray & target) {
    if (radius <= 0.0 || fill_alpha <= 0.0f) return;
    constexpr int N_SEG = 36;
    visualization_msgs::msg::Marker m;
    m.header.frame_id = "map";
    m.header.stamp = rclcpp::Clock().now();
    m.action = visualization_msgs::msg::Marker::ADD;
    m.type = visualization_msgs::msg::Marker::TRIANGLE_LIST;
    m.id = marker_id;
    m.ns = marker_ns;
    m.pose.orientation.w = 1.0;
    m.scale.x = m.scale.y = m.scale.z = 1.0;
    m.color.r = r; m.color.g = g; m.color.b = b; m.color.a = fill_alpha;
    m.lifetime.sec = 2;
    geometry_msgs::msg::Point center;
    center.x = pose.position.x;
    center.y = pose.position.y;
    center.z = SECTOR_Z;
    m.points.reserve(static_cast<size_t>(N_SEG) * 3);
    for (int i = 0; i < N_SEG; ++i) {
      const double a0 = 2.0 * M_PI * i / N_SEG;
      const double a1 = 2.0 * M_PI * (i + 1) / N_SEG;
      geometry_msgs::msg::Point p0;
      p0.x = pose.position.x + radius * std::cos(a0);
      p0.y = pose.position.y + radius * std::sin(a0);
      p0.z = SECTOR_Z;
      geometry_msgs::msg::Point p1;
      p1.x = pose.position.x + radius * std::cos(a1);
      p1.y = pose.position.y + radius * std::sin(a1);
      p1.z = SECTOR_Z;
      m.points.push_back(center);
      m.points.push_back(p0);
      m.points.push_back(p1);
    }
    target.markers.push_back(m);
  };

  // pause overlay を xy 円沿いに点線 (dot 形式) で重ね描き (#179)。
  // LINE_LIST で短い segment + 長い gap で dot 表現:
  //   旧 add_dashed_outline (#136) は segment 長 0.05m + 1:1 比率の dash で「点と感じない、
  //   潰れて見える」user feedback (#178 PR コメント) を受け、dot 長を短くし on:off 比率を 1:4 に拡大。
  // pause 発火条件は xy 円内 (#84 hysteresis) なので xy 境界 (= add_radius_circle と同じ円) に重畳する。
  // 主 glyph より僅かに上 (z + 0.001) に置いて隠れ防止。
  auto add_dotted_outline = [&](const geometry_msgs::msg::Pose & pose, double radius,
                                float r, float g, float b, float a,
                                int marker_id,
                                const std::string & marker_ns,
                                visualization_msgs::msg::MarkerArray & target) {
    if (radius <= 0.0) return;

    // dot 中心間隔 0.10m、dot 長 0.02m → cycle 比 20% on (1:4 on:off)。
    // WebUI 側 (dashArray '2, 6') は 25% on (1:3 on:off) で厳密には僅差あるが、
    // どちらも sparse dot pattern として視覚的に同方向で整合 (#179 cursor review round 2)。
    // typical radius (0.1-2m) で dot として識別可能な粒度。
    constexpr double DOT_STEP = 0.10;
    constexpr double DOT_LENGTH = 0.02;
    const double total_arc_len = 2.0 * M_PI * radius;
    const int n_dots = std::max(12, static_cast<int>(std::ceil(total_arc_len / DOT_STEP)));
    // dot 1 個分の弧角度。極小半径 (Tolerance min 0.001m など) で DOT_LENGTH/radius が
    // セル角を超えると dot が連結して dash 化するため、cell の 40% を cap として保つ
    // (1:4 比率を維持できなくなる場合でも dot 識別性 ≧ on:off 分離を優先)。
    const double cell_angle = (2.0 * M_PI) / n_dots;
    const double dot_angle_span = std::min(DOT_LENGTH / radius, 0.4 * cell_angle);

    visualization_msgs::msg::Marker m;
    m.header.frame_id = "map";
    m.header.stamp = rclcpp::Clock().now();
    m.action = visualization_msgs::msg::Marker::ADD;
    m.type = visualization_msgs::msg::Marker::LINE_LIST;
    m.id = marker_id;
    m.ns = marker_ns;
    m.pose.orientation.w = 1.0;
    m.scale.x = 0.04;  // dot 線幅 (旧 dash の 0.06m から短縮、dot として丸みを出す)
    m.color.r = r; m.color.g = g; m.color.b = b; m.color.a = a;
    m.lifetime.sec = 2;

    m.points.reserve(static_cast<size_t>(n_dots) * 2);
    for (int i = 0; i < n_dots; ++i) {
      const double a1 = (2.0 * M_PI) * i / n_dots;
      const double a2 = a1 + dot_angle_span;
      geometry_msgs::msg::Point p1, p2;
      p1.x = pose.position.x + radius * std::cos(a1);
      p1.y = pose.position.y + radius * std::sin(a1);
      p1.z = SECTOR_Z + 0.001;
      p2.x = pose.position.x + radius * std::cos(a2);
      p2.y = pose.position.y + radius * std::sin(a2);
      p2.z = SECTOR_Z + 0.001;
      m.points.push_back(p1);
      m.points.push_back(p2);
    }
    target.markers.push_back(m);
  };

  const auto has_tag = [](const mapoi_interfaces::msg::PointOfInterest & poi,
                          const std::string & target) {
    for (const auto & tag : poi.tags) {
      if (tag == target) return true;
    }
    return false;
  };

  auto apply_poi_color = [&](visualization_msgs::msg::Marker & marker,
                             const mapoi_interfaces::msg::PointOfInterest & poi) {
    if (highlighted_goal_names_.count(poi.name) > 0) {
      marker.color.r = 1.0; marker.color.g = 0.6; marker.color.b = 0.0; marker.color.a = 0.7;
    } else if (has_tag(poi, "waypoint")) {
      marker.color.r = 0.0; marker.color.g = 1.0; marker.color.b = 0.0; marker.color.a = 0.7;
    } else if (has_tag(poi, "landmark")) {
      marker.color.r = 0.5; marker.color.g = 0.5; marker.color.b = 0.5; marker.color.a = 0.7;
    } else {
      marker.color.r = 0.1; marker.color.g = 0.45; marker.color.b = 1.0; marker.color.a = 0.7;
    }
  };

  auto poi_color = [&](const mapoi_interfaces::msg::PointOfInterest & poi) {
    std::array<float, 3> color = {0.1f, 0.45f, 1.0f};
    if (has_tag(poi, "waypoint")) {
      color = {0.0f, 1.0f, 0.0f};
    } else if (has_tag(poi, "landmark")) {
      color = {0.5f, 0.5f, 0.5f};
    }
    return color;
  };

  // 購読者ゼロスキップ (#402): POI marker の購読者がいなければ幾何構築も publish も行わない。
  // marker_data_dirty_ はこの分岐では消費しないため、購読者復帰後の最初の tick で必ず
  // フル構築される (下の dirty クリアは pois/routes 双方の publish を経てから行う)。
  if (pois_has_sub) {
  size_t poi_index_one_based = 0;
  for (const auto & poi : pois_list_) {
    poi_index_one_based += 1;  // POI Editor (mapoi_config.yaml の poi: 順) 行番号、1-based、tag フィルタ非依存
    geometry_msgs::msg::Pose pose = poi.pose;

    // POI tolerance を 円 (xy 判定領域) + 扇形 (yaw 制約) で重ね描き (#136 / #179、全 POI 共通)。
    // 主 glyph は waypoint > landmark > custom/default の優先順:
    //   - waypoint: 塗り扇形 (緑)
    //   - landmark: 薄塗り扇形 (灰)
    //   - custom/default: 薄塗り扇形 (青)
    // 円 outline は判定 semantics (xy 境界) を控えめに常時表示し、扇形は yaw 制約あり時のみ
    // 重ね描きする。pause tag があれば xy 円沿いに dot pattern overlay を追加。
    if (show_sector && poi.tolerance.xy > 0.0) {
      const auto color = poi_color(poi);
      const float sr = color[0];
      const float sg = color[1];
      const float sb = color[2];
      const float fill_a = has_tag(poi, "waypoint") ? 0.4f : 0.15f;
      const float stroke_a = 0.7f;

      // 円 outline (xy 判定領域、#179): 常時描画。半透明 + 細実線で控えめに「進入判定境界」を示す。
      add_radius_circle(pose, poi.tolerance.xy, sr, sg, sb, 0.4f, id,
                        poi_marker_ns("tolerance_xy", poi.name), ma_pois);
      id += 1;

      // 扇形 (yaw 制約、#136): 0 < yaw < π の時は扇形 (wedge)、yaw >= π (yaw 不問 = 全方位)
      // の時は塗りつぶし円 (disc) で重ね描きする (#267)。yaw 不問を円 outline だけにすると
      // 「描画されていない?」と誤認されるため disc で「全方位 OK」を明示する (WebUI と整合)。
      const bool has_yaw_constraint =
        (poi.tolerance.yaw > 0.0 && poi.tolerance.yaw < M_PI);
      if (has_yaw_constraint) {
        id += add_sector(pose, poi.tolerance.xy, poi.tolerance.yaw,
                         sr, sg, sb,
                         fill_a, stroke_a,
                         id, poi_marker_ns("tolerance_yaw", poi.name), ma_pois);
      } else if (poi.tolerance.yaw >= M_PI) {
        add_filled_disc(pose, poi.tolerance.xy, sr, sg, sb, fill_a,
                        id, poi_marker_ns("tolerance_yaw", poi.name), ma_pois);
        id += 1;
      }

      if (has_tag(poi, "pause")) {
        // pause overlay は xy 円沿いに dot pattern (#179)。
        // pause 発火条件 (xy 円内) と境界が一致するため自然な視覚的根拠になる。
        add_dotted_outline(pose, poi.tolerance.xy,
                           sr, sg, sb, 0.9f,
                           id, poi_marker_ns("status_paused", poi.name), ma_pois);
        id += 1;
      }
    }

    const std::string arrow_ns = poi_marker_ns("arrow", poi.name);
    visualization_msgs::msg::Marker m_arrow = default_arrow_marker;
    m_arrow.pose = pose;
    m_arrow.pose.position.z = 0.1;
    m_arrow.ns = arrow_ns;
    apply_poi_color(m_arrow, poi);
    m_arrow.id = id;
    ma_pois.markers.push_back(m_arrow);
    id += 1;

    const std::string label_text = build_label(poi_index_one_based, poi.name);
    if (!label_text.empty()) {
      visualization_msgs::msg::Marker m_text = default_text_marker;
      m_text.text = label_text;
      m_text.pose = pose;
      m_text.pose.position.z = 0.1;
      m_text.ns = arrow_ns;
      m_text.id = id;
      ma_pois.markers.push_back(m_text);
      id += 1;
    }
  }

  // delete remaining incorrect marker
  if(id_buf_ > id) {
    visualization_msgs::msg::Marker m_del;
    m_del.action = visualization_msgs::msg::Marker::DELETEALL;
    visualization_msgs::msg::MarkerArray ma_del;
    ma_del.markers.push_back(m_del);
    marker_pois_pub_->publish(ma_del);
  }
  id_buf_ = id;

  marker_pois_pub_->publish(ma_pois);
  // clean tick で stamp 更新して再送するため構築結果を cache する (#402)。
  cached_ma_pois_ = std::move(ma_pois);
  }  // if (pois_has_sub)

  // Route markers (mapoi/markers/routes topic)。
  // route_display_mode parameter で表示制御:
  //   "all"      : 全 route 表示、active route (highlighted_route_names_ に含む) は太線+不透明で強調
  //   "selected" : active route のみ表示
  //   "none"     : 表示しない (DELETEALL で既存も消す)
  // POI marker (waypoint=green / landmark=gray / default=blue) と被らない palette を使う。
  static constexpr std::array<std::array<float, 3>, 6> ROUTE_PALETTE = {{
    {1.0f, 0.5f, 0.0f},   // orange
    {1.0f, 0.0f, 1.0f},   // magenta
    {0.0f, 1.0f, 1.0f},   // cyan
    {1.0f, 1.0f, 0.0f},   // yellow
    {0.7f, 0.3f, 1.0f},   // purple
    {0.0f, 0.7f, 0.3f},   // teal
  }};
  auto pick_route_color = [](const std::string & name) {
    std::hash<std::string> h;
    return ROUTE_PALETTE[h(name) % ROUTE_PALETTE.size()];
  };

  // route_mode は timer_callback 冒頭で取得済み (差分ゲートの parameter 変化検出と共有、#402)。
  // 購読者ゼロスキップ (#402): route marker の購読者がいなければ構築も publish も行わない。
  // marker_data_dirty_ はこの分岐では消費しない (dirty クリアは pois/routes 双方の後で行う)。
  if (routes_has_sub) {
  visualization_msgs::msg::MarkerArray ma_routes;
  int route_id = 0;

  // ルート経由点間の矢印マーカー生成
  if (route_mode != "none" && highlighted_route_ordered_.size() >= 2) {
    // POI名→poseのルックアップマップを構築
    std::map<std::string, geometry_msgs::msg::Pose> poi_pose_map;
    for (const auto & poi : pois_list_) {
      poi_pose_map[poi.name] = poi.pose;
    }

    for (size_t i = 0; i + 1 < highlighted_route_ordered_.size(); ++i) {
      auto it_start = poi_pose_map.find(highlighted_route_ordered_[i]);
      auto it_end = poi_pose_map.find(highlighted_route_ordered_[i + 1]);
      if (it_start == poi_pose_map.end() || it_end == poi_pose_map.end()) {
        continue;
      }

      visualization_msgs::msg::Marker m_arrow;
      m_arrow.header.frame_id = "map";
      m_arrow.header.stamp = rclcpp::Clock().now();
      m_arrow.action = visualization_msgs::msg::Marker::ADD;
      m_arrow.type = visualization_msgs::msg::Marker::ARROW;
      m_arrow.ns = "highlight_direction";
      m_arrow.scale.x = 0.05;  // shaft diameter
      m_arrow.scale.y = 0.1;   // head diameter
      m_arrow.scale.z = 0.1;   // head length
      m_arrow.color.r = 0.6; m_arrow.color.g = 0.2; m_arrow.color.b = 1.0; m_arrow.color.a = 0.7;
      m_arrow.lifetime.sec = 2.0;

      geometry_msgs::msg::Point p_start;
      p_start.x = it_start->second.position.x;
      p_start.y = it_start->second.position.y;
      p_start.z = 0.15;
      geometry_msgs::msg::Point p_end;
      p_end.x = it_end->second.position.x;
      p_end.y = it_end->second.position.y;
      p_end.z = 0.15;

      m_arrow.points.push_back(p_start);
      m_arrow.points.push_back(p_end);
      m_arrow.id = route_id++;
      ma_routes.markers.push_back(m_arrow);
    }
  }

  if (route_mode != "none") {
    constexpr double ROUTE_LINE_Z = 0.05;  // map / costmap よりわずかに上
    for (const auto & [name, waypoints] : all_routes_) {
      if (waypoints.size() < 2) continue;  // 1 点だけの route は polyline にできない
      const bool is_active = highlighted_route_names_.count(name) > 0;
      if (route_mode == "selected" && !is_active) continue;

      const auto color = pick_route_color(name);
      visualization_msgs::msg::Marker m;
      m.header.frame_id = "map";
      m.header.stamp = rclcpp::Clock().now();
      m.action = visualization_msgs::msg::Marker::ADD;
      m.type = visualization_msgs::msg::Marker::LINE_STRIP;
      m.ns = "route/" + name;
      m.id = route_id++;
      m.lifetime.sec = 2.0;
      m.pose.orientation.w = 1.0;  // identity (LINE_STRIP の point は world 座標)
      m.scale.x = is_active ? 0.08 : 0.04;  // active を太く (line width)
      m.color.r = color[0]; m.color.g = color[1]; m.color.b = color[2];
      m.color.a = is_active ? 0.9f : 0.4f;  // active を不透明、他は薄く

      m.points.reserve(waypoints.size());
      for (const auto & wp : waypoints) {
        geometry_msgs::msg::Point p;
        p.x = wp.pose.position.x;
        p.y = wp.pose.position.y;
        p.z = ROUTE_LINE_Z;
        m.points.push_back(p);
      }
      ma_routes.markers.push_back(m);
    }
  }

  if (route_id_buf_ > route_id) {
    visualization_msgs::msg::Marker m_del;
    m_del.action = visualization_msgs::msg::Marker::DELETEALL;
    visualization_msgs::msg::MarkerArray ma_del;
    ma_del.markers.push_back(m_del);
    marker_routes_pub_->publish(ma_del);
  }
  route_id_buf_ = route_id;
  marker_routes_pub_->publish(ma_routes);
  // clean tick で stamp 更新して再送するため構築結果を cache する (#402)。
  cached_ma_routes_ = std::move(ma_routes);
  }  // if (routes_has_sub)

  // 差分ゲートの dirty クリア (#402): dirty は pois/routes 共通の単一フラグなので、両方が
  // 購読者ありで揃って構築・cache 更新できたときだけ落とす。片側が購読者ゼロで skip した場合は
  // その側の cache が古いままなので、購読者復帰後に再構築させるため dirty を残す
  // (代償として購読者ありの側は復帰まで毎 tick 再構築されるが、購読者ゼロ運用は一時的とみなす)。
  // 両側とも購読者ゼロなら次 tick もこの dirty 経路を通り、購読者復帰時に確実にフル構築される。
  if (pois_has_sub && routes_has_sub) {
    marker_data_dirty_ = false;
  }
}

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<MapoiRviz2Publisher>());
  rclcpp::shutdown();
  return 0;
}
