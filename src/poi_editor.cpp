#include "mapoi_rviz_plugins/poi_editor.hpp"
#include "mapoi_rviz_plugins/config_path_update_policy.hpp"
#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"
#include <class_loader/class_loader.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

// QFileDialog / QStandardPaths は FileComboBox の移設 (#397 step 9 → poi_editor_table.cpp)
// に伴い不要化したため削除。
#include <QTimer>
// undo_stack_->clear() の呼び出しに完全型が要る (hpp は前方宣言のみ、#407)。
#include <QUndoStack>

#include "ui_poi_editor.h"

using namespace std::chrono_literals;

namespace mapoi_rviz_plugins
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("mapoi_rviz_plugins.poi_editor");

// PoiTable column index 定数は poi_editor_helpers.hpp に集約 (#158 で導入、#346 の
// TU 分割に伴い 2 TU から参照されるため header へ移動)。
using detail::kColName;
using detail::kColPose;
using detail::kColTolerance;
using detail::kColTags;
using detail::kColDescription;
using detail::kColCount;

PoiEditorPanel::PoiEditorPanel(QWidget* parent) : Panel(parent),  ui_(new Ui::PoiEditorUi())
{
  ui_->setupUi(this);
}

PoiEditorPanel::~PoiEditorPanel() = default;

void PoiEditorPanel::onInitialize()
{
  node_ = this->getDisplayContext()->getRosNodeAbstraction().lock()->get_raw_node();

  // Create shared service node and persistent clients
  service_node_ = rclcpp::Node::make_shared("poieditor_service_client");
  select_map_client_ = service_node_->create_client<mapoi_interfaces::srv::SelectMap>("mapoi/select_map");
  get_pois_info_client_ = service_node_->create_client<mapoi_interfaces::srv::GetPoisInfo>("mapoi/get_pois_info");
  get_maps_info_client_ = service_node_->create_client<mapoi_interfaces::srv::GetMapsInfo>("mapoi/get_maps_info");
  get_tag_defs_client_ = service_node_->create_client<mapoi_interfaces::srv::GetTagDefinitions>("mapoi/get_tag_definitions");
  reload_map_info_client_ = service_node_->create_client<std_srvs::srv::Trigger>("mapoi/reload_map_info");

  connect(ui_->MapComboBox, SIGNAL(activated(int)), this, SLOT(MapComboBox()));
  connect(ui_->ResetButton, SIGNAL(clicked()), this, SLOT(ResetButton()));
  connect(ui_->PoiTable, SIGNAL(cellChanged(int, int)), this, SLOT(TableChanged(int, int)));
  connect(ui_->PoiTable->verticalHeader(), SIGNAL(sectionMoved(int, int, int)), this, SLOT(RowMoved(int, int, int)));
  connect(ui_->NewButton, SIGNAL(clicked()), this, SLOT(NewButton()));
  connect(ui_->CopyButton, SIGNAL(clicked()), this, SLOT(CopyButton()));
  connect(ui_->DeleteButton, SIGNAL(clicked()), this, SLOT(DeleteButton()));
  connect(ui_->FileComboBox, SIGNAL(activated(int)), this, SLOT(FileComboBox()));
  connect(ui_->SaveButton, SIGNAL(clicked()), this, SLOT(SaveButton()));
  connect(ui_->TagFilterComboBox, SIGNAL(activated(int)), this, SLOT(TagFilterChanged(int)));
  connect(ui_->TagHelperComboBox, SIGNAL(activated(int)), this, SLOT(TagHelperSelected(int)));
  // POI 名フィルタ (#405): テキスト変更のたびに setRowHidden で絞り込む。
  // textChanged は文字入力・clearButton 押下の両方で発火する。フィルタ本体は
  // NameFilterEdit の現在値を直接読むため、シグナル引数は使わず直接 connect する。
  connect(ui_->NameFilterEdit, &QLineEdit::textChanged,
    this, &PoiEditorPanel::ApplyNameFilter);

  // Undo/Redo 基盤 (#407): QUndoStack 生成・ショートカット (Ctrl+Z / Ctrl+Shift+Z、
  // WidgetWithChildrenShortcut) 配線・cleanChanged↔dirty 連携をまとめて行う。テーブル系
  // slot の connect (上の cellChanged / sectionMoved / New/Copy/Delete) より後、初回の
  // UpdatePoiTable (shadow 初期構築 + stack clear を行う) より前に一度だけ呼ぶ。定義は
  // poi_editor_table.cpp。
  SetupUndoRedo();

  poi_pose_sub_ = node_->create_subscription<geometry_msgs::msg::PoseStamped>(
    "mapoi_rviz_pose", 10, std::bind(&PoiEditorPanel::PoiPoseCallback, this, std::placeholders::_1));

  parentWidget()->setVisible(true);

  // MapComboBox
  if (!get_maps_info_client_->wait_for_service(3s)) {
    RCLCPP_ERROR(LOGGER, "mapoi/get_maps_info service not available after 3s timeout.");
    return;
  }
  auto request = std::make_shared<mapoi_interfaces::srv::GetMapsInfo::Request>();
  auto result = get_maps_info_client_->async_send_request(request);
  // #404: hang した service で UI スレッドが無期限ブロックしないよう timeout を付ける
  if (rclcpp::spin_until_future_complete(service_node_, result, 5s) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(LOGGER, "Failed to call service mapoi/get_maps_info (timeout or error)");
    return;
  }
  auto map_info = result.get();
  current_map_ = map_info->map_name;
  map_name_list_ = map_info->maps_list;
  ui_->MapComboBox->clear();
  for (const auto & map : map_name_list_) {
    ui_->MapComboBox->addItem(QString::fromStdString(map));
  }
  InitConfigs(map_info->map_name);
  LoadTagDefinitions();

  config_path_sub_ = node_->create_subscription<std_msgs::msg::String>(
    "mapoi/config_path", rclcpp::QoS(1).transient_local(),
    std::bind(&PoiEditorPanel::ConfigPathCallback, this, std::placeholders::_1));

  // Display Settings group: mapoi_rviz2_publisher の表示系 parameter を Panel から制御 (#99)
  // - route_display_mode (all / selected / none): RadioButton
  // - poi_label_format (index / name / both / none): RadioButton
  // - show_tolerance_sector (bool): CheckBox (#179: 旧 arrow_size_ratio UI を置換、
  //   PR #178 で publisher 側 parameter は廃止済だったが Panel UI が取り残されていた regression を修正)
  rviz2_pub_param_client_ =
    std::make_shared<rclcpp::AsyncParametersClient>(service_node_, "/mapoi_rviz2_publisher");

  // Display Settings 副パネル (ウィジェット構築 + connect) は別 TU へ分離 (#397 step 5)。
  // 順序制約: rviz2_pub_param_client_ 生成 → UI 構築/connect → 初期 Sync の順を保つ。
  SetupDisplaySettingsUi();

  // 初期同期: publisher から現在 parameter 値を読んで UI に反映
  SyncDisplaySettingsFromPublisher();
}

void PoiEditorPanel::onEnable()
{
  show();
  parentWidget()->show();
}

void PoiEditorPanel::onDisable()
{
  hide();
  parentWidget()->hide();
}

void PoiEditorPanel::MapComboBox()
{
  auto request_sm = std::make_shared<mapoi_interfaces::srv::SelectMap::Request>();
  request_sm->map_name = map_name_list_[ui_->MapComboBox->currentIndex()];

  if (!select_map_client_->wait_for_service(3s)) {
    RCLCPP_ERROR(LOGGER, "mapoi/select_map service not available after 3s timeout.");
    return;
  }

  auto result_sm = select_map_client_->async_send_request(request_sm);
  // #404: timeout (説明は poi_editor.cpp onInitialize)
  if (rclcpp::spin_until_future_complete(service_node_, result_sm, 5s) != rclcpp::FutureReturnCode::SUCCESS ||
      !result_sm.get()->success) {
    RCLCPP_ERROR(LOGGER, "Failed to call service mapoi/select_map (timeout or error)");
    return;
  }
  PoiEditorPanel::UpdatePoiTable();
}

// ResetButton / TableChanged / NewButton / CopyButton / DeleteButton / RowMoved /
// FileComboBox の定義は poi_editor_table.cpp へ切り出した (#397 step 9)。
// 同 TU には Undo/Redo 基盤 (QUndoStack / QUndoCommand 群 / shadow model / 適用ヘルパー)
// も実装している (#407。step 9 を先行分割せず Undo/Redo と統合したのは #397 step 9 の記述どおり)。

// SaveButton() の定義は poi_editor_save.cpp へ切り出した (#397 step 6)。

// Subscription Callback
void PoiEditorPanel::PoiPoseCallback(geometry_msgs::msg::PoseStamped::SharedPtr msg)
{
  // frame_id 検証 (#430): MapoiPoseTool は Fixed Frame で pose を publish するが、POI pose は
  // "map" frame 前提で保存される (bridge 側は frame_id="map" の goal として発行するため)。
  // Fixed Frame が "map" 以外だと raw な x/y/theta をそのまま反映してしまい、ロボットを誤誘導する
  // 座標が保存される。判定自体は純関数 (detail::is_map_frame) に切り出し、msg からの読み取りだけ
  // ここ (ROS executor スレッド) で行う。ダイアログ表示・テーブル更新は他 callback (#399/#406) と
  // 同じ規約で queued lambda 内 (UI スレッド) にまとめる。
  const bool frame_ok = detail::is_map_frame(msg->header.frame_id);
  const auto frame_id = msg->header.frame_id;
  auto p = msg->pose;
  auto txt = tr("%1, %2, %3").arg(p.position.x).arg(p.position.y).arg(detail::calc_yaw(p));
  QMetaObject::invokeMethod(this, [this, frame_ok, frame_id, txt]() {
    if (!frame_ok) {
      // 反映を拒否し警告のみ表示する (修正案2)。テーブルには一切触れない。
      QMessageBox::warning(this, tr("Frame Mismatch"),
        tr("Set Mapoi Pose ignored: the Fixed Frame is '%1', but POI poses are stored in "
           "the 'map' frame. Set the RViz Fixed Frame to 'map' and try again.")
          .arg(QString::fromStdString(frame_id)));
      return;
    }
    int current_row = ui_->PoiTable->currentRow();
    if (current_row >= 0) {
      ui_->PoiTable->setItem(current_row, kColPose, new QTableWidgetItem(txt));
    }
  }, Qt::QueuedConnection);
}

void PoiEditorPanel::ConfigPathCallback(std_msgs::msg::String::SharedPtr msg)
{
  // Resolve symlinks to get the real (src/) path instead of install/ path
  std::filesystem::path p(msg->data);
  std::string resolved_path = msg->data;
  try {
    resolved_path = std::filesystem::canonical(p).string();
  } catch (const std::filesystem::filesystem_error& e) {
    RCLCPP_WARN(LOGGER, "Could not resolve canonical path: %s", e.what());
  }

  std::filesystem::path resolved_p(resolved_path);
  std::string map_name = resolved_p.parent_path().filename().string();

  // 同じ map で path も同じ場合 (= save 後の reload_map_info で再 publish される flow)
  // でも POI 内容が変わっている可能性があるため UpdatePoiTable で table を再 fetch する (#135)。
  // map 切替 / 初回受信時は MapComboBox 同期 + FileComboBox 再構築も必要なので InitConfigs を呼ぶ。
  // 自分自身が save 直後 (suppress_config_callback_update_) は 1.5 秒の SAVED! feedback を
  // 守るため UpdatePoiTable を skip する。旧挙動と同じく queued lambda 実行時の flag を見る。
  //
  // メンバ更新 (current_map_ / config_path_) と action 判定は queued lambda 内 (UI スレッド)
  // で行う (#399, PR #414 review high)。ROS executor スレッドでの読み書きを無くして UI スレッド
  // 所有の規約 (PR #412 と同じ) に揃えるのに加え、lambda は Qt イベントキューの FIFO で
  // イベント順に適用されるため、確認ダイアログのネストイベントループ中に後続イベントの
  // lambda が走ってもメンバは常に最新へ進む (Yes 時の最新メンバ再構築とセットで機能する)。
  QMetaObject::invokeMethod(this, [this, resolved_path, map_name]() {
    const auto base_action = detail::decide_poi_editor_config_path_action(
      current_map_, map_name, config_path_);
    config_path_ = resolved_path;
    current_map_ = map_name;
    const auto suppressed = detail::apply_poi_editor_content_update_suppression(
      base_action, suppress_config_callback_update_);

    // 内容 diff ガード (#403): ゲート適用順 suppression → dedup → dirty guard。
    // suppression が Noop にした場合、dedup は Noop 入力をそのまま素通しし (last_seen も
    // 更新されない)、guard は kProceed → dispatch は何もしない。suppression (Save 直後の
    // SAVED! 保護) と dedup (内容不変の抑制) は目的が異なるが、この合成で両立する。
    // dedup が Noop にした場合 = 内容不変の再 publish。dirty でも確認ダイアログを出さない
    // (変化が無いのに編集破棄を問わない)。
    std::error_code mtime_ec;
    const auto mtime = std::filesystem::last_write_time(resolved_path, mtime_ec);
    const auto action = detail::apply_config_content_dedup(
      suppressed, !mtime_ec,
      resolved_path, mtime,
      last_seen_config_path_, last_seen_config_mtime_);

    // dedup で Noop にならなかった場合 = イベントを処理する場合に last_seen_* を更新する。
    // 更新タイミングは dirty guard (AskUser) の前: No でも last_seen を進めることで、同一内容
    // の再 publish で再度ダイアログを出さない。実変更 (mtime 変化) なら再度尋ねる。
    if (action != detail::ConfigPathUpdateAction::Noop) {
      last_seen_config_path_ = resolved_path;
      if (!mtime_ec) {
        last_seen_config_mtime_ = mtime;
      }
    }

    // 未保存編集ガード (#399)。suppression 適用後の action・dirty・dialog 表示中フラグから
    // 再構築の可否を純関数で判定する。callback はこの判定を QMessageBox 表示へ写像するだけ。
    const auto guard = detail::decide_config_reload_guard(
      action, table_dirty_, config_dialog_open_);

    if (guard == detail::ConfigReloadGuardDecision::kDrop) {
      // dialog 表示中に届いたイベントは破棄する (QMessageBox のネストイベントループ中に
      // 後続 queued lambda が積み重なって dialog が重なるのを防ぐ)。ユーザーが「再読込」を
      // 選べばその時点の最新状態が fetch されるため drop で欠損しない。
      return;
    }
    if (guard == detail::ConfigReloadGuardDecision::kAskUser) {
      // 未保存編集がある状態で外部変更を検出。破棄可否をユーザーに確認する。
      // dialog 表示中に届く config_path イベントは config_dialog_open_ で drop させる。
      config_dialog_open_ = true;
      const auto answer = QMessageBox::question(
        this, tr("External Change Detected"),
        tr("The configuration was changed externally. Reload?\n"
           "(Unsaved edits will be discarded.)"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      config_dialog_open_ = false;
      if (answer != QMessageBox::Yes) {
        // 「編集を継続」: 再構築を skip してテーブルを温存する。InitConfigs を呼ばないため
        // FileComboBox とテーブルは旧 map/path 側で「一貫して」残り、保存も旧 path へ正しく
        // 対応する (メンバ current_map_ / config_path_ だけが最新へ進む)。次の外部イベントでは
        // dirty が立ったままなので再度確認され、SaveButton では baseline 比較ガード
        // (should_confirm_overwrite) が外部変更を再検出して上書き確認を出す。
        return;
      }
      // 「再読込」: dialog 表示中に後続イベントが kDrop で破棄されていても、メンバは
      // 各イベントの lambda (FIFO) で常に最新へ更新済み。キャプチャした map_name/action で
      // 部分再構築すると MapComboBox / FileComboBox とテーブルが食い違うため (PR #414 review
      // high)、最新メンバから full reinit する (InitConfigs は FileComboBox 再構築 +
      // UpdatePoiTable まで行い、dirty / baseline もそこでリセットされる)。
      InitConfigs(current_map_);
      return;
    }

    // kProceed (dirty でない / Noop): 従来通りキャプチャ値で dispatch する (dialog を挟まない
    // ため staleness は生じない。連続イベントも FIFO で各 lambda が順に適用され最後が勝つ)。
    if (action == detail::ConfigPathUpdateAction::ReinitializeMap) {
      InitConfigs(map_name);
    } else if (action == detail::ConfigPathUpdateAction::RefreshCurrentMap) {
      UpdatePoiTable();
    }
  }, Qt::QueuedConnection);
}

// TagFilterChanged / PopulateTagFilter / LoadTagDefinitions / TagHelperSelected の定義は
// poi_editor_tags.cpp へ切り出した (#397 step 7)。

void PoiEditorPanel::UpdatePoiCount()
{
  const int total = ui_->PoiTable->rowCount();
  int visible = 0;
  for (int row = 0; row < total; ++row) {
    if (!ui_->PoiTable->isRowHidden(row)) {
      ++visible;
    }
  }
  // 名前フィルタ (#405) で非表示行がある間は「可視/全体」を併記し、テーブルの見た目と
  // 件数表示のズレを避ける (PR #420 review。タグフィルタは行自体を再構築するため
  // rowCount() に反映済みで、従来表記のまま)。
  if (visible != total) {
    ui_->PoiCountLabel->setText(tr("POIs: %1/%2").arg(visible).arg(total));
  } else {
    ui_->PoiCountLabel->setText(tr("POIs: %1").arg(total));
  }
}

// ValidatePois() の定義は poi_editor_validation.cpp へ切り出した (#346)。

// Functions
void PoiEditorPanel::InitConfigs(std::string map_name)
{
  // MapComboBox
  int i = 0;
  for(const auto & map : map_name_list_){
    if(map == map_name){
      ui_->MapComboBox->setCurrentIndex(i);
      break;
    }
    i++;
  }

  // FileComboBox
  ui_->FileComboBox->clear();
  if (!config_path_.empty()) {
    ui_->FileComboBox->addItem(QString::fromStdString(config_path_));
  }
  ui_->FileComboBox->addItem("the other");

  // PoiTable, SaveButton, and CheckBox
  PoiEditorPanel::UpdatePoiTable();
}

void PoiEditorPanel::UpdatePoiTable()
{
  auto request_gtp = std::make_shared<mapoi_interfaces::srv::GetPoisInfo::Request>();

  if (!get_pois_info_client_->wait_for_service(3s)) {
    RCLCPP_ERROR(LOGGER, "mapoi/get_pois_info service not available after 3s timeout.");
    return;
  }

  auto result_gtp = get_pois_info_client_->async_send_request(request_gtp);
  // #404: timeout (説明は poi_editor.cpp onInitialize)
  if (rclcpp::spin_until_future_complete(service_node_, result_gtp, 5s) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(LOGGER, "Failed to call service mapoi/get_pois_info (timeout or error)");
    return;
  }
  auto poi_info = result_gtp.get();
  auto pois = poi_info->pois_list;
  auto numRows = pois.size();

  // Store all POIs for tag filtering
  all_pois_.assign(pois.begin(), pois.end());
  PopulateTagFilter();

  ui_->PoiTable->clear();
  // Drag による reorder で動いた verticalHeader の visual order は、
  // 同じ rowCount で setRowCount し直しても reset されない (Qt 既知挙動)。
  // 一度 0 行にしてから再生成することで visual = logical を強制する。
  ui_->PoiTable->setRowCount(0);
  ui_->PoiTable->setRowCount(numRows);
  // column 構造 (#158): name / pose / tolerance / tags / description
  ui_->PoiTable->setColumnCount(kColCount);
  ui_->PoiTable->setHorizontalHeaderLabels(
    QStringList() << tr("name") << tr("pose (x, y, yaw)")
                  << tr("tolerance (xy, yaw)") << tr("tags") << tr("description"));
  ui_->PoiTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  ui_->PoiTable->verticalHeader()->setSectionsMovable(true);
  // ソートは有効化しない (undo/shadow/save が論理 index 前提) (#433)

  is_table_color_ = false;
  for (size_t row = 0; row < numRows; row++){
    const auto & p = pois[row];
    // 表示精度: pose / tolerance とも小数点以下 2 桁固定。
    ui_->PoiTable->setItem(row, kColName, new QTableWidgetItem(QString::fromStdString(p.name)));
    ui_->PoiTable->setItem(row, kColPose, new QTableWidgetItem(
      tr("%1, %2, %3")
        .arg(QString::number(p.pose.position.x, 'f', 2))
        .arg(QString::number(p.pose.position.y, 'f', 2))
        .arg(QString::number(detail::calc_yaw(p.pose), 'f', 2))));
    ui_->PoiTable->setItem(row, kColTolerance, new QTableWidgetItem(
      tr("%1, %2")
        .arg(QString::number(p.tolerance.xy, 'f', 2))
        .arg(QString::number(p.tolerance.yaw, 'f', 2))));
    ui_->PoiTable->setItem(row, kColTags, new QTableWidgetItem(QString::fromStdString(detail::join(p.tags, ", "))));
    ui_->PoiTable->setItem(row, kColDescription, new QTableWidgetItem(QString::fromStdString(p.description)));
  }
  is_table_color_ = true;
  ui_->SaveButton->setText("save");
  // 空 list (numRows == 0) では setItem 由来の cellChanged 副作用が発火せず、SAVED 時の
  // green stylesheet が残ったままになる症状 (issue #77 症状 2) を明示リセットで防ぐ。
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: white; color: black;}");
  UpdatePoiCount();
  // 全再構築後、名前フィルタを再適用して絞り込み状態を維持する (#405)。
  // setRowHidden は行を削除しないため rowCount() = 全 POI 数は変わらず、
  // SaveButton の全行ループは非表示行も含めて正しく保存される。
  ApplyNameFilter();

  // Undo/Redo (#407): 全再構築で行 index の前提が総入れ替えになるため、それ以前の
  // 履歴 (行 index を握るコマンド) を残すと undo で存在しない行を触りかねない。安全側に
  // 割り切って undo stack を clear し、shadow model を再構築後のテーブルで取り直す。
  // (名前フィルタ #405 の setRowHidden は行を削除しないため履歴と共存可 = clear 不要。
  //  タグフィルタ TagFilterChanged 側でも同様に clear + 取り直しを行う。)
  if (undo_stack_) {
    undo_stack_->clear();  // ここで clean=true になり cleanChanged→table_dirty_=false も走る
  }
  RebuildShadowModel();

  // 全再構築 = サーバ状態と一致した時点なので dirty をクリアする (#399)。
  // stack clear の cleanChanged でも false になるが、undo_stack_ 未生成の初回や
  // 既に clean だった場合 (cleanChanged 不発) に備えて明示 set は不変のまま残す。
  table_dirty_ = false;

  // 外部変更検出 (SaveButton) の baseline スナップショットを取り直す (#399)。
  // テーブルの元データに対応する config ファイルは FileComboBox->itemText(0) (= config_path_
  // 由来。SaveButton の LoadFile が読む path と同じ)。std::ifstream で全読みして {path, content}
  // を保存する。読めない場合は両方 clear = 比較不能 (should_confirm_overwrite がガードを無効化
  // = 従来挙動になる)。
  baseline_path_.clear();
  baseline_content_.clear();
  if (ui_->FileComboBox->count() > 0) {
    const std::string base_path = ui_->FileComboBox->itemText(0).toStdString();
    std::ifstream base_ifs(base_path, std::ios::binary);
    if (base_ifs) {
      std::stringstream ss;
      ss << base_ifs.rdbuf();
      if (base_ifs.good() || base_ifs.eof()) {
        baseline_path_ = base_path;
        baseline_content_ = ss.str();
      }
    }
    if (baseline_path_.empty() && !config_path_.empty()) {
      // config を把握しているのに baseline を読めなかった場合のみ WARN する (config 未取得で
      // itemText(0) が "the other" の場合はガード無効が正常なのでログしない)。
      // ガード無効化 (= 上書き確認が出ない) の切り分け用 (PR #414 review low)。
      RCLCPP_WARN(LOGGER, "Failed to read baseline config for overwrite guard: %s",
                  base_path.c_str());
    }
  }
}

// calcYaw / join / SplitSentence の定義は poi_editor_helpers.hpp に移動 (#397 step 8)。
// detail::calc_yaw / detail::join / detail::split_sentence として自由関数化。

}  // mapoi_rviz_plugins

CLASS_LOADER_REGISTER_CLASS(mapoi_rviz_plugins::PoiEditorPanel, rviz_common::Panel)
