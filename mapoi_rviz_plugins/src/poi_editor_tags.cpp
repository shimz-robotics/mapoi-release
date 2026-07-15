// PoiEditorPanel::TagFilterChanged / PopulateTagFilter / LoadTagDefinitions /
// TagHelperSelected / ApplyNameFilter / ApplyNameFilterToRow の定義。
// (#397 step 7 で poi_editor.cpp から別 TU へ分離、#405 で名前フィルタを追加)
#include "mapoi_rviz_plugins/poi_editor.hpp"
#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"

// undo_stack_->clear() の呼び出しに完全型が要る (hpp は前方宣言のみ、#407)。
#include <QUndoStack>

// generated UI header: PoiEditorPanel::ui_ (`Ui::PoiEditorUi*`) は poi_editor.hpp では
// 前方宣言のみなので、`ui_->TagFilterComboBox` 等の完全型アクセスにはこの include が要る
// (poi_editor_save.cpp / poi_editor_validation.cpp と同じ)。
#include "ui_poi_editor.h"

// std::chrono_literals (3s) は LoadTagDefinitions の wait_for_service で使用。
using namespace std::chrono_literals;

namespace mapoi_rviz_plugins
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("mapoi_rviz_plugins.poi_editor");

// PoiTable column index 定数は poi_editor_helpers.hpp に集約 (#158 で導入)。
using detail::kColName;
using detail::kColPose;
using detail::kColTolerance;
using detail::kColTags;
using detail::kColDescription;

// Tag Filter
void PoiEditorPanel::TagFilterChanged(int index)
{
  // 未保存編集ガード (#428)。current index / 適用済み index / dirty から純関数で判定する
  // (#399 の decide_config_reload_guard と同型)。callback はこの判定を no-op / 確認ダイアログ /
  // フィルタ適用へ写像するだけ。フィルタ適用は setRowCount(0) の行入れ替え + undo_stack_->clear()
  // で未保存編集を復元不能に破棄するため、config_path 経路と同様にガードが要る。
  const auto decision = detail::decide_tag_filter_change(
    index, applied_tag_filter_index_, table_dirty_);

  if (decision == detail::TagFilterChangeDecision::kNoop) {
    // 適用済みと同じ index の再選択 (activated は選び直しでも発火する) は何もしない。
    return;
  }
  if (decision == detail::TagFilterChangeDecision::kAskUser) {
    // 未保存編集がある状態でのフィルタ変更。破棄可否をユーザーに確認する。
    const auto answer = QMessageBox::question(
      this, tr("Unsaved Edits"),
      tr("There are unsaved edits. Discard them and change the tag filter?"),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
      // 「編集を継続」: combo の選択を適用済み index に戻して何もしない。setCurrentIndex は
      // activated を発火しないが currentIndexChanged は飛ぶため blockSignals で確実に抑制し、
      // シグナル再発火 (この slot の再入や将来配線される slot) を防ぐ。
      const bool blocked = ui_->TagFilterComboBox->blockSignals(true);
      ui_->TagFilterComboBox->setCurrentIndex(applied_tag_filter_index_);
      ui_->TagFilterComboBox->blockSignals(blocked);
      return;
    }
    // 「破棄して変更」: 従来通りフィルタ適用へ進む (以降で undo stack を clear する)。
  }

  if (index <= 0) {
    // "All" 選択: 全再構築 (PopulateTagFilter が applied_tag_filter_index_ を 0 にリセットする)。
    UpdatePoiTable();
    return;
  }

  std::string selected_tag = ui_->TagFilterComboBox->currentText().toStdString();

  is_table_color_ = false;
  ui_->PoiTable->setRowCount(0);

  int row = 0;
  for (const auto& p : all_pois_) {
    bool has_tag = false;
    for (const auto& tag : p.tags) {
      if (tag == selected_tag) {
        has_tag = true;
        break;
      }
    }
    if (has_tag) {
      ui_->PoiTable->insertRow(row);
      // 表示精度: pose / tolerance とも小数点以下 2 桁固定。長すぎる尾桁を避ける (#159 round 5 user)。
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
      row++;
    }
  }
  is_table_color_ = true;
  UpdatePoiCount();
  // タグフィルタで行を再構築した後、名前フィルタを再適用して絞り込み状態を維持する (#405)。
  // タグフィルタはまずタグ一致行を setRowCount(0) で再構築し、その後 名前フィルタが
  // setRowHidden で絞り込む。つまり「タグ AND 名前」の併用絞り込みになる。
  ApplyNameFilter();

  // Undo/Redo (#407): タグフィルタの適用は setRowCount(0) で行を丸ごと入れ替えるため、
  // 行 index を握る既存履歴が崩れる。UpdatePoiTable の全再構築と同じ理由で undo stack を
  // clear し、shadow model をフィルタ後のテーブルで取り直す (index <= 0 の解除パスは
  // UpdatePoiTable を呼ぶのでそちらで clear + 取り直し済み)。
  if (undo_stack_) {
    undo_stack_->clear();
  }
  RebuildShadowModel();

  // 適用完了。以後この index の再選択は decide_tag_filter_change の kNoop で弾く (#428)。
  applied_tag_filter_index_ = index;
}

void PoiEditorPanel::PopulateTagFilter()
{
  std::set<std::string> unique_tags;
  for (const auto& p : all_pois_) {
    for (const auto& tag : p.tags) {
      if (!tag.empty()) {
        unique_tags.insert(tag);
      }
    }
  }

  ui_->TagFilterComboBox->clear();
  ui_->TagFilterComboBox->addItem("All");
  for (const auto& tag : unique_tags) {
    ui_->TagFilterComboBox->addItem(QString::fromStdString(tag));
  }
  // combo を "All" (index 0) に再構築した = フィルタ無し状態に戻ったので追跡を同期する (#428)。
  // clear()/addItem() は activated を発火しないため TagFilterChanged は呼ばれず、
  // UpdatePoiTable (全再構築 = 全 POI 表示) と applied index が食い違わないようここで揃える。
  applied_tag_filter_index_ = 0;
}

void PoiEditorPanel::LoadTagDefinitions()
{
  if (!get_tag_defs_client_->wait_for_service(3s)) {
    RCLCPP_WARN(LOGGER, "mapoi/get_tag_definitions service not available, TagHelper will be empty.");
    return;
  }

  auto request = std::make_shared<mapoi_interfaces::srv::GetTagDefinitions::Request>();
  auto result = get_tag_defs_client_->async_send_request(request);
  // #404: timeout (説明は poi_editor.cpp)。非 SUCCESS 時は result.get() せず return。
  if (rclcpp::spin_until_future_complete(service_node_, result, 5s) != rclcpp::FutureReturnCode::SUCCESS) {
    RCLCPP_WARN(LOGGER, "Failed to call service mapoi/get_tag_definitions (timeout or error), TagHelper will be empty.");
    return;
  }
  auto response = result.get();

  known_tag_names_.clear();
  known_tag_is_system_.clear();
  for (const auto & def : response->definitions) {
    known_tag_names_.push_back(def.name);
    known_tag_is_system_.push_back(def.is_system);
  }

  // Build TagHelperComboBox
  ui_->TagHelperComboBox->clear();
  ui_->TagHelperComboBox->addItem("+ Add tag...");
  for (size_t i = 0; i < known_tag_names_.size(); i++) {
    QString label;
    if (known_tag_is_system_[i]) {
      label = QString("[S] %1").arg(QString::fromStdString(known_tag_names_[i]));
    } else {
      label = QString::fromStdString(known_tag_names_[i]);
    }
    ui_->TagHelperComboBox->addItem(label);
  }
}

void PoiEditorPanel::TagHelperSelected(int index)
{
  if (index <= 0) return;

  // Get tag name (strip "[S] " prefix if present)
  QString selected = ui_->TagHelperComboBox->itemText(index);
  if (selected.startsWith("[S] ")) {
    selected = selected.mid(4);
  }
  std::string tag_name = selected.toStdString();

  // Get current row's tags cell (column 5; tags は #138 で column 4→5 にシフト)
  int current_row = ui_->PoiTable->currentRow();
  if (current_row < 0) {
    ui_->TagHelperComboBox->setCurrentIndex(0);
    return;
  }

  auto* tags_item = ui_->PoiTable->item(current_row, kColTags);
  std::string current_tags = tags_item ? tags_item->text().toStdString() : "";

  // Check if tag already exists
  auto tag_list = detail::split_sentence(current_tags, ", ");
  for (const auto& t : tag_list) {
    if (t == tag_name) {
      ui_->TagHelperComboBox->setCurrentIndex(0);
      return;
    }
  }

  // Append tag (column 5 = tags、column 4 (tolerance.yaw) を上書きしないよう注意 — Codex review #139 high)
  std::string new_tags;
  if (current_tags.empty()) {
    new_tags = tag_name;
  } else {
    new_tags = current_tags + ", " + tag_name;
  }
  ui_->PoiTable->setItem(current_row, kColTags, new QTableWidgetItem(QString::fromStdString(new_tags)));

  // Reset combo to placeholder
  ui_->TagHelperComboBox->setCurrentIndex(0);
}

// POI 名フィルタ (#405)
// -----------------------------------------------------------------------
// 設計判断:
//   - setRowHidden を使うため行は削除せず rowCount() は不変。
//     SaveButton の全行ループは非表示行も含めて正しく保存される
//     (タグフィルタの setRowCount(0) 再構築とは異なり、保存ブロック不要)。
//   - 判定は logical row の item text に対して行う (verticalHeader の
//     visual 並び替えには依存しない)。
//   - タグフィルタとの併用: TagFilterChanged → ApplyNameFilter の順に呼ぶことで
//     「タグ AND 名前」の絞り込みになる。タグフィルタで非一致行は行ごと setRowCount(0) で
//     排除済みのため、名前フィルタは残行に対して追加絞り込みを行うだけでよい。
//   - TableChanged (セル編集): セル編集は行数を変えないため setRowHidden 状態は有効なまま。
//     kColName セルの編集確定時のみ、その行を ApplyNameFilterToRow で再評価する
//     (PR #420 review。一致しなくなった行は編集確定と同時に隠れる)。
//   - New/Copy の新規行はフィルタ非一致でもデフォルト可視のまま (作った行が見えないと
//     編集を継続できないため意図的)。件数表示は UpdatePoiCount の可視/全体 併記で整合する。
//   - 行ドラッグ (setSectionsMovable): setRowHidden は logical row 基準で visual 並びと
//     独立。非表示行を跨ぐドラッグは Qt 標準挙動に委ねる。
// -----------------------------------------------------------------------

void PoiEditorPanel::ApplyNameFilterToRow(int row)
{
  const QString filter_text = ui_->NameFilterEdit->text();
  if (filter_text.isEmpty()) {
    // 空文字 = フィルタ解除 (行は常に表示)
    ui_->PoiTable->setRowHidden(row, false);
    return;
  }
  const auto * name_item = ui_->PoiTable->item(row, kColName);
  const QString name_text = name_item ? name_item->text() : QString();
  // 部分一致・大文字小文字不区別 (WebUI の matchesPoiName と同じ意味論)
  ui_->PoiTable->setRowHidden(row, !name_text.contains(filter_text, Qt::CaseInsensitive));
}

void PoiEditorPanel::ApplyNameFilter()
{
  const int num_rows = ui_->PoiTable->rowCount();
  for (int row = 0; row < num_rows; ++row) {
    ApplyNameFilterToRow(row);
  }
  // 可視行数が変わるので件数表示 (可視/全体) を同期する (PR #420 review)。
  UpdatePoiCount();
}

}  // namespace mapoi_rviz_plugins
