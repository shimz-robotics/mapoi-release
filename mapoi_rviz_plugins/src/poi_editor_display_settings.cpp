// PoiEditorPanel::SetupDisplaySettingsUi / SyncDisplaySettingsFromPublisher /
// RevertDisplaySettingsUiFromCache の定義 (#397 step 5 で poi_editor.cpp から別 TU へ分離)。
// extract-method に伴い SetupDisplaySettingsUi の宣言を poi_editor.hpp へ追加した
// (他の宣言は変更なし)。QRadioButton / QCheckBox は poi_editor.hpp
// 経由で入る。以下の Qt layout ヘッダと <QSignalBlocker> は poi_editor.hpp に無いので
// 明示 include する (これらは全て本 TU 内でのみ使用)。ui_ (`Ui::PoiEditorUi*`) への
// 完全型アクセスは本 TU には無いため ui_poi_editor.h は不要。
#include "mapoi_rviz_plugins/poi_editor.hpp"

#include <QGroupBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QSignalBlocker>

// std::chrono_literals (50ms / 100ms / 200ms / 500ms) は set/get_parameters の
// wait_for_service / spin_until_future_complete で使用。
using namespace std::chrono_literals;

namespace mapoi_rviz_plugins
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("mapoi_rviz_plugins.poi_editor");

void PoiEditorPanel::SetupDisplaySettingsUi()
{
  // onInitialize から一度だけ呼ばれる前提。誤って再呼び出しされた場合にウィジェットと
  // connect が二重化しないよう早期 return で守る (PR #422 review。メンバは hpp で
  // nullptr 初期化されており、構築済みなら非 null)。
  if (route_radio_all_ != nullptr) {
    return;
  }
  auto * display_group = new QGroupBox("Display Settings", this);
  auto * display_form = new QFormLayout(display_group);
  display_form->setContentsMargins(8, 8, 8, 8);

  // Route display mode
  auto * route_h = new QHBoxLayout();
  route_radio_all_ = new QRadioButton("all", this);
  route_radio_selected_ = new QRadioButton("selected", this);
  route_radio_none_ = new QRadioButton("none", this);
  route_radio_selected_->setChecked(true);  // default in mapoi_rviz2_publisher
  auto * route_group = new QButtonGroup(this);
  route_group->setExclusive(true);
  route_group->addButton(route_radio_all_);
  route_group->addButton(route_radio_selected_);
  route_group->addButton(route_radio_none_);
  route_h->addWidget(route_radio_all_);
  route_h->addWidget(route_radio_selected_);
  route_h->addWidget(route_radio_none_);
  route_h->addStretch();
  display_form->addRow("Route:", route_h);

  // POI label format
  auto * label_h = new QHBoxLayout();
  label_radio_index_ = new QRadioButton("index", this);
  label_radio_name_ = new QRadioButton("name", this);
  label_radio_both_ = new QRadioButton("both", this);
  label_radio_none_ = new QRadioButton("none", this);
  label_radio_index_->setChecked(true);  // default in mapoi_rviz2_publisher
  auto * label_group = new QButtonGroup(this);
  label_group->setExclusive(true);
  label_group->addButton(label_radio_index_);
  label_group->addButton(label_radio_name_);
  label_group->addButton(label_radio_both_);
  label_group->addButton(label_radio_none_);
  label_h->addWidget(label_radio_index_);
  label_h->addWidget(label_radio_name_);
  label_h->addWidget(label_radio_both_);
  label_h->addWidget(label_radio_none_);
  label_h->addStretch();
  display_form->addRow("POI label:", label_h);

  // Tolerance visualization toggle (#179: 旧 arrow_size_ratio UI を置換)
  tolerance_sector_check_ = new QCheckBox(this);
  tolerance_sector_check_->setChecked(true);  // default in mapoi_rviz2_publisher
  display_form->addRow("Tolerance:", tolerance_sector_check_);

  // Display Settings group を verticalLayout に append (Save 行の下)
  if (auto * panel_layout = qobject_cast<QVBoxLayout *>(this->layout())) {
    panel_layout->addWidget(display_group);
  } else {
    // poi_editor.ui のルート layout が QVBoxLayout でなくなった場合のみ到達する。
    // その場合 display_group は UI に載らない (parent=this なので leak はしない)。
    // 黙って消えると原因究明が難しいため明示ログを残す (PR #422 review low)。
    RCLCPP_ERROR(LOGGER, "Display Settings group could not be attached: root layout is not QVBoxLayout");
  }

  // Connect signals → set_parameters service call.
  // 失敗 (service 未起動 / spin timeout / SetParametersResult unsuccessful) いずれの場合も:
  //   1. SyncDisplaySettingsFromPublisher() で publisher 真値で UI 更新を試みる (sync 成功なら
  //      UI = publisher で cache も更新される)
  //   2. それも失敗 (= service 完全 down) なら RevertDisplaySettingsUiFromCache() で
  //      最後に publisher と一致が確認できた値 (cached_*) に UI を戻す
  // これで UI/publisher state ズレ問題は service 状態に関わらず必ず収束 (Codex round 1-3 中)。
  auto fail_recovery = [this](const std::string & name, const std::string & reason) {
    RCLCPP_WARN(LOGGER, "SetParameters failed for %s (%s), reverting UI", name.c_str(), reason.c_str());
    SyncDisplaySettingsFromPublisher();
    RevertDisplaySettingsUiFromCache();
  };
  auto send_param = [this, fail_recovery](const std::string & name, const rclcpp::ParameterValue & value) {
    // service_is_ready() は cached state を返すだけで、AsyncParametersClient が
    // 一度も spin されていないと stale で false を返す。wait_for_service で短い spin を挟む。
    if (!rviz2_pub_param_client_->wait_for_service(100ms)) {
      fail_recovery(name, "service not ready after 100ms wait");
      return;
    }
    auto fut = rviz2_pub_param_client_->set_parameters({rclcpp::Parameter(name, value)});
    auto rc = rclcpp::spin_until_future_complete(service_node_, fut, 500ms);
    if (rc != rclcpp::FutureReturnCode::SUCCESS) {
      fail_recovery(name, "spin timeout");
      return;
    }
    auto results = fut.get();
    if (results.empty() || !results[0].successful) {
      fail_recovery(name, results.empty() ? "no result" : results[0].reason);
      return;
    }
    // 成功: cache 更新 (publisher が受理した値 = 確定値、次回失敗時の revert 元として記憶)
    if (name == "route_display_mode") {
      cached_route_mode_ = value.get<std::string>();
    } else if (name == "poi_label_format") {
      cached_label_fmt_ = value.get<std::string>();
    } else if (name == "show_tolerance_sector") {
      cached_tolerance_sector_ = value.get<bool>();
    }
  };
  auto send_string_param = [send_param](const std::string & name, const std::string & value) {
    send_param(name, rclcpp::ParameterValue(value));
  };
  auto send_bool_param = [send_param](const std::string & name, bool value) {
    send_param(name, rclcpp::ParameterValue(value));
  };

  // route_display_mode: toggled(checked=true) のみ反応 (false 側 toggle で重複 send しない)
  connect(route_radio_all_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("route_display_mode", "all"); });
  connect(route_radio_selected_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("route_display_mode", "selected"); });
  connect(route_radio_none_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("route_display_mode", "none"); });

  // poi_label_format
  connect(label_radio_index_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("poi_label_format", "index"); });
  connect(label_radio_name_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("poi_label_format", "name"); });
  connect(label_radio_both_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("poi_label_format", "both"); });
  connect(label_radio_none_, &QRadioButton::toggled,
    [send_string_param](bool checked) { if (checked) send_string_param("poi_label_format", "none"); });

  // show_tolerance_sector (#179)
  connect(tolerance_sector_check_, &QCheckBox::toggled,
    [send_bool_param](bool checked) { send_bool_param("show_tolerance_sector", checked); });
}

void PoiEditorPanel::SyncDisplaySettingsFromPublisher()
{
  // CLI で先に変更されていた場合や SetParameters 失敗時に、UI を publisher の真値に寄せる。
  // 失敗時 (publisher 未起動 / timeout) は何もしない (UI は変えない、log は呼び出し側で出力)。
  // 成功時は cache (cached_*) も同時に publisher 真値で更新する。
  if (!rviz2_pub_param_client_->wait_for_service(50ms)) {
    return;  // 短い wait、ready でなければ skip
  }
  auto fut = rviz2_pub_param_client_->get_parameters(
    {"route_display_mode", "poi_label_format", "show_tolerance_sector"});
  if (rclcpp::spin_until_future_complete(service_node_, fut, 200ms) !=
      rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_WARN(LOGGER, "Sync of Display Settings parameters timed out");
    return;
  }
  auto params = fut.get();
  if (params.size() < 3) {
    return;
  }
  const std::string route_mode = params[0].as_string();
  const std::string label_fmt = params[1].as_string();
  const bool tolerance_sector = params[2].as_bool();

  // cache 更新 (publisher 真値が記憶される、次回 SetParameters 失敗時の revert 元として使用)
  cached_route_mode_ = route_mode;
  cached_label_fmt_ = label_fmt;
  cached_tolerance_sector_ = tolerance_sector;

  // QSignalBlocker で setChecked / setValue が SetParameters を再発火させないようにする
  QSignalBlocker b1(route_radio_all_);
  QSignalBlocker b2(route_radio_selected_);
  QSignalBlocker b3(route_radio_none_);
  QSignalBlocker b4(label_radio_index_);
  QSignalBlocker b5(label_radio_name_);
  QSignalBlocker b6(label_radio_both_);
  QSignalBlocker b7(label_radio_none_);
  QSignalBlocker b8(tolerance_sector_check_);

  if (route_mode == "all") route_radio_all_->setChecked(true);
  else if (route_mode == "none") route_radio_none_->setChecked(true);
  else route_radio_selected_->setChecked(true);  // default fallback

  if (label_fmt == "name") label_radio_name_->setChecked(true);
  else if (label_fmt == "both") label_radio_both_->setChecked(true);
  else if (label_fmt == "none") label_radio_none_->setChecked(true);
  else label_radio_index_->setChecked(true);  // default fallback

  tolerance_sector_check_->setChecked(tolerance_sector);
}

void PoiEditorPanel::RevertDisplaySettingsUiFromCache()
{
  // service 完全 down 等で SyncDisplaySettingsFromPublisher が no-op だった場合のフォールバック。
  // cached_* は最後に publisher と一致が確認できた値 (初期同期 or 直前の SetParameters 成功時)。
  // 通常は呼び出し側 (fail_recovery) で必ず Sync を試みた直後に呼ぶため、Sync 成功時は
  // cache が UI と一致していて revert は no-op になる。
  QSignalBlocker b1(route_radio_all_);
  QSignalBlocker b2(route_radio_selected_);
  QSignalBlocker b3(route_radio_none_);
  QSignalBlocker b4(label_radio_index_);
  QSignalBlocker b5(label_radio_name_);
  QSignalBlocker b6(label_radio_both_);
  QSignalBlocker b7(label_radio_none_);
  QSignalBlocker b8(tolerance_sector_check_);

  if (cached_route_mode_ == "all") route_radio_all_->setChecked(true);
  else if (cached_route_mode_ == "none") route_radio_none_->setChecked(true);
  else route_radio_selected_->setChecked(true);

  if (cached_label_fmt_ == "name") label_radio_name_->setChecked(true);
  else if (cached_label_fmt_ == "both") label_radio_both_->setChecked(true);
  else if (cached_label_fmt_ == "none") label_radio_none_->setChecked(true);
  else label_radio_index_->setChecked(true);

  tolerance_sector_check_->setChecked(cached_tolerance_sector_);
}

}  // namespace mapoi_rviz_plugins
