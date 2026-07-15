// PoiEditorPanel::SaveButton の定義 (#397 step 6 で poi_editor.cpp から別 TU へ分離)。
// クラス構造・宣言 (poi_editor.hpp) は変更なし。yaml-cpp / QMessageBox は poi_editor.hpp
// 経由で入る。<fstream> / <QTimer> は poi_editor.hpp に無いので明示 include する。
#include "mapoi_rviz_plugins/poi_editor.hpp"
#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"

#include <fstream>
#include <sstream>

#include <QTimer>
// undo_stack_->setClean() の呼び出しに完全型が要る (hpp は前方宣言のみ、#407)。
#include <QUndoStack>

// generated UI header: PoiEditorPanel::ui_ (`Ui::PoiEditorUi*`) は poi_editor.hpp では
// 前方宣言のみなので、`ui_->PoiTable` 等の完全型アクセスにはこの include が要る
// (poi_editor_validation.cpp と同じ)。
#include "ui_poi_editor.h"

using namespace std::chrono_literals;

namespace mapoi_rviz_plugins
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("mapoi_rviz_plugins.poi_editor");

// pure helpers (try_parse_finite_double / split_and_trim) は poi_editor_helpers.hpp に
// 切り出した (#158 で test 容易化のため)。
using detail::try_parse_finite_double;
using detail::split_and_trim;

// PoiTable column index 定数は poi_editor_helpers.hpp に集約 (#158 で導入)。
using detail::kColName;
using detail::kColPose;
using detail::kColTolerance;
using detail::kColTags;
using detail::kColDescription;

void PoiEditorPanel::SaveButton()
{
  // Block save when tag filter is active (not "All")
  if (ui_->TagFilterComboBox->currentIndex() > 0) {
    QMessageBox::warning(this, tr("Filter Active"),
      tr("Cannot save while tag filter is active.\nPlease select \"All\" first to show all POIs."));
    return;
  }

  // Validate before saving
  if (!ValidatePois()) {
    return;
  }

  // 外部変更検出 (#399)。保存先ファイルが baseline (最後にテーブルへ読み込んだ / 保存した
  // 時点の内容) から外部で変更されていたら上書き確認を挟む。WebUI の expected_version
  // 楽観ロック相当のクライアント側実装 (config_version の厳密共有は不要で内容比較で足りる)。
  //
  // 比較は保存先 path (currentText) を基準にする。PR #413 review (low: itemText(0)/currentText
  // 非対称) を踏まえ、FileComboBox「the other」で別ファイルを保存先に選んだ場合は baseline_path_
  // と一致せず、should_confirm_overwrite が false を返して比較しない (誤った基準ファイルとの
  // 比較を避ける)。baseline を読めていない場合もガード無効 = 従来挙動。
  const std::string save_target_path = ui_->FileComboBox->currentText().toStdString();
  if (save_target_path == baseline_path_ && !baseline_content_.empty()) {
    std::string current_disk_content;
    std::ifstream cur_ifs(save_target_path, std::ios::binary);
    if (cur_ifs) {
      std::stringstream ss;
      ss << cur_ifs.rdbuf();
      current_disk_content = ss.str();
    }
    if (detail::should_confirm_overwrite(
          save_target_path, baseline_path_, baseline_content_, current_disk_content)) {
      const auto answer = QMessageBox::question(
        this, tr("External Change Detected"),
        tr("The destination configuration file was changed externally. Overwrite?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (answer != QMessageBox::Yes) {
        return;  // キャンセル: 上書きしない
      }
    }
  }

  int numRows = ui_->PoiTable->rowCount();

  // #429: 保存対象 YAML が読めない (placeholder "file to save" のまま到達 / 外部で削除・破損)
  // 場合に YAML::BadFile / ParserException が未捕捉のまま漏れると RViz ごと落ちるため、
  // 他の Save Error 系ダイアログ (open for writing / write failure) と同じパターンで捕捉する。
  YAML::Node map_info;
  try {
    map_info = YAML::LoadFile(ui_->FileComboBox->itemText(0).toStdString());
  } catch (const std::exception & e) {
    RCLCPP_ERROR(LOGGER, "Failed to load map YAML file: %s", e.what());
    QMessageBox::critical(this, tr("Save Error"),
      tr("Failed to load the map YAML file:\n%1").arg(QString::fromStdString(e.what())));
    return;
  }
  std::vector<YAML::Node> pois_list;

  for (int row = 0; row < numRows; row++) {
    int logical_row = ui_->PoiTable->verticalHeader()->logicalIndex(row);
    YAML::Node poi;
    // 全 cell の null check (#159 round 2 ヘビー medium): 未生成セルが混じると segfault する。
    // ValidatePois と同様に「nullptr → 空文字」で扱い、validation で reject されるはずだが、
    // ここでも防御として null を空に変換する。
    auto cell_text = [&](int col) -> std::string {
      auto * item = ui_->PoiTable->item(logical_row, col);
      return item ? item->text().toStdString() : "";
    };
    poi["name"] = cell_text(kColName);
    // pose も tolerance と同じ厳密 parser (try_parse_finite_double) で 1abc / nan / inf を拒否
    // (#159 round 1 medium 対応で std::stod から切り替え)。表記揺れ trim は split_and_trim で吸収。
    auto poses_str = cell_text(kColPose);
    auto pose_parts = split_and_trim(poses_str, ',');
    double x_val = 0.0, y_val = 0.0, yaw_val_pose = 0.0;
    if (pose_parts.size() != 3
        || !try_parse_finite_double(pose_parts[0], x_val)
        || !try_parse_finite_double(pose_parts[1], y_val)
        || !try_parse_finite_double(pose_parts[2], yaw_val_pose)) {
      RCLCPP_ERROR(LOGGER, "Failed to parse pose at row %d (post-validation race?)", row);
      QMessageBox::critical(this, tr("Save Error"),
        tr("Failed to parse pose at row %1.").arg(row + 1));
      return;
    }
    poi["pose"]["x"] = x_val;
    poi["pose"]["y"] = y_val;
    poi["pose"]["yaw"] = yaw_val_pose;
    // tolerance を 1 column "xy m, yaw rad" に統合 (#158): split → xy_val (m), yaw_val (rad)
    // を抽出して tolerance.xy / tolerance.yaw に書き戻す。yaw は rad で UI と yaml を統一
    // (旧 deg 入力は #138 の暫定仕様。pose.yaw が rad 表示なので tolerance.yaw も rad に揃える)。
    auto tolerance_str = cell_text(kColTolerance);
    auto tolerance_parts = split_and_trim(tolerance_str, ',');
    double xy_val = 0.0;
    double yaw_val = 0.0;
    if (tolerance_parts.size() != 2
        || !try_parse_finite_double(tolerance_parts[0], xy_val)
        || !try_parse_finite_double(tolerance_parts[1], yaw_val)) {
      RCLCPP_ERROR(LOGGER, "Failed to parse tolerance at row %d (post-validation race?)", row);
      QMessageBox::critical(this, tr("Save Error"),
        tr("Failed to parse tolerance at row %1.").arg(row + 1));
      return;
    }
    poi["tolerance"]["xy"] = xy_val;
    poi["tolerance"]["yaw"] = yaw_val;
    auto tags_str = cell_text(kColTags);
    poi["tags"] = detail::split_sentence(tags_str, ", ");
    poi["description"] = cell_text(kColDescription);
    pois_list.push_back(poi);
  }

  map_info["poi"] = pois_list;
  YAML::Emitter out;
  out << map_info;

  std::string save_path = ui_->FileComboBox->currentText().toStdString();
  std::ofstream file(save_path);
  if (!file.is_open()) {
    RCLCPP_ERROR(LOGGER, "Failed to open file for writing: %s", save_path.c_str());
    QMessageBox::critical(this, tr("Save Error"),
      tr("Failed to open file for writing:\n%1").arg(QString::fromStdString(save_path)));
    return;
  }
  file << out.c_str();
  file.close();
  if (!file.good()) {
    RCLCPP_ERROR(LOGGER, "Error occurred while writing file: %s", save_path.c_str());
    QMessageBox::critical(this, tr("Save Error"),
      tr("Error occurred while writing file:\n%1").arg(QString::fromStdString(save_path)));
    return;
  }

  ui_->SaveButton->setText("SAVED!");
  ui_->SaveButton->setStyleSheet("QPushButton {background-color: green; color: black;}");

  // 保存成功: 未保存編集ガードの状態を更新する (#399)。
  // - dirty clear は書き込み直後に行う (1.5 秒後の UpdatePoiTable を待たず、その間に届く
  //   config_path 再着信で ConfigPathCallback が確認ダイアログを出さないようにする)。
  // - baseline も書き込んだ内容 (out.c_str()) と保存先 path で更新する。1.5 秒後の
  //   UpdatePoiTable が itemText(0) から取り直す baseline と実質同じだが、それを待たずに
  //   直後の save 連打や外部変更検出が正しい基準を持てるようにここでも更新する。
  table_dirty_ = false;
  baseline_path_ = save_path;
  baseline_content_ = out.c_str();
  // 着色基準 (#445) も保存内容 (= 現在のテーブル内容をミラーする shadow_) へ同期する。
  // 1.5 秒後の UpdatePoiTable 再構築 (RebuildShadowModel) を待たずに、保存直後の編集が
  // 「保存値との差分」で正しく着色されるようにする (baseline_content_ の即時更新と同旨)。
  // 緑着色の解除も下の undo_stack_->clear() の cleanChanged(true) 経由に頼らず明示する
  // (QUndoStack::clear は既に clean な stack では cleanChanged を emit しないため、
  // 経路依存を残さない — PR #447 review)。二重実行は冪等で無害。
  clean_texts_ = shadow_;
  ClearAllEditMarks();

  // Undo/Redo (#407): 保存成功 = 履歴境界として undo stack を clear する (PR #426 review)。
  // setClean で履歴を残しても、1.5 秒後の UpdatePoiTable 全再構築で stack は消えるため
  // 「保存直後の 1.5 秒だけ undo でき、直後の再構築で突然保存状態へ戻される」錯乱窓に
  // しかならない。WebUI (poi-history.js) は保存後も履歴を保つが、RViz 側には保存後の
  // 自動再構築があるため意図的に分岐する。clear は clean 化を伴い cleanChanged→
  // table_dirty_=false も走る (上の明示 set と整合)。undo_stack_ 未生成は無いが防御的に check。
  if (undo_stack_) {
    undo_stack_->clear();
  }

  // 保存後の reload_map_info 失敗はログだけだと「保存成功と認識したままサーバ側が古い」状態に
  // 気づけない (PR #424 review medium)。保存自体は完了している旨とあわせて明示する。
  auto warn_reload_failed = [this]() {
    QMessageBox::warning(this, tr("Reload Failed"),
      tr("The configuration was saved, but the server failed to reload it.\n"
         "Other clients may not see the change until the server reloads."));
  };
  if (!reload_map_info_client_->wait_for_service(3s)) {
    RCLCPP_ERROR(LOGGER, "mapoi/reload_map_info service not available after 3s timeout.");
    warn_reload_failed();
    return;
  }
  auto request_reload_map_info = std::make_shared<std_srvs::srv::Trigger::Request>();
  auto result_reload_map_info = reload_map_info_client_->async_send_request(request_reload_map_info);
  // #404: timeout (説明は poi_editor.cpp)。保存自体は完了済み。timeout / 非 SUCCESS 時は
  // suppression・QTimer (UpdatePoiTable 遅延再構築) フローに進まず return する。
  // SAVED! 表示は既に完了しているが、それは既存の wait_for_service 失敗 return と同じ位置づけ。
  if (rclcpp::spin_until_future_complete(service_node_, result_reload_map_info, 5s) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_ERROR(LOGGER, "Failed to call service mapoi/reload_map_info (timeout or error)");
    warn_reload_failed();
    return;
  }

  // Drag による reorder は Qt の visual order だけ変えるため、Save 後も verticalHeader の
  // 番号は元の logical order のまま (例: [3, 1, 2] のように見える)。
  // saved YAML を fetch し直して table を再構築することで visual = logical を揃え、
  // 番号が 1, 2, 3, ... に並ぶようにする。drag 中の background highlight (Qt::green) も解除。
  //
  // delay を入れて SAVED + green を 1.5 秒見せてから rebuild する (issue #77)。
  // 即時 rebuild だと UpdatePoiTable() が text/style を即 reset するため、Save 成功 feedback が
  // ほぼ見えない (reload service が高速なほど顕著)。QTimer::singleShot で Qt event loop に戻して
  // SAVED + green の paint を保証してから 1.5 秒後に rebuild。
  //
  // 同じ 1.5 秒の間に再 Save 連打されると pending rebuild と新 Save flow が race するため、
  // SaveButton を disable して連打防止。timer 完了時に setEnabled(true) で復帰。
  // 注: 1.5 秒間に Reset / MapCombo 等の他 UpdatePoiTable 呼び出しが入ると順序競合あり得るが、
  // 短い窓 + Save 直後の操作頻度低さから許容。必要なら future PR で member QTimer + cancel
  // pattern に拡張可能。
  ui_->SaveButton->setEnabled(false);
  // SAVED! の green feedback を 1.5 秒見せる間、外部 (mapoi/config_path 再 publish 由来) からの
  // UpdatePoiTable trigger を抑制する。1.5 秒後に rebuild + flag リセットで通常 flow に戻る。
  suppress_config_callback_update_ = true;
  QTimer::singleShot(1500, this, [this]() {
    UpdatePoiTable();
    ui_->SaveButton->setEnabled(true);
    suppress_config_callback_update_ = false;
  });
}

}  // namespace mapoi_rviz_plugins
