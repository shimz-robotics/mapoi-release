// PoiEditorPanel::ValidatePois の定義 (#346 で poi_editor.cpp から別 TU へ分離)。
// クラス構造・宣言 (poi_editor.hpp) は変更なし。UI 側 (QTableWidget 読み取り / QMessageBox
// 表示) は元のまま Qt 依存だが、pose/tolerance セルの判定と tag 排他判定は
// poi_editor_helpers.hpp の純関数 (validate_pose_cell / validate_tolerance_cell /
// check_tag_exclusivity、#158 の try_parse_finite_double / split_and_trim と同じ場所) に
// 委譲し、表示文言は元の文字列・書式指定子を一言一句変えずに組み立てる。
#include "mapoi_rviz_plugins/poi_editor.hpp"
#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"

#include <cmath>
#include <set>
#include <string>

#include <QMessageBox>

// generated UI header: PoiEditorPanel::ui_ (`Ui::PoiEditorUi*`) は poi_editor.hpp では
// 前方宣言のみなので、`ui_->PoiTable` 等の完全型アクセスにはこの include が要る
// (poi_editor.cpp と同じ)。
#include "ui_poi_editor.h"

namespace mapoi_rviz_plugins
{

// PoiTable column index 定数は poi_editor_helpers.hpp に集約 (PR #371 review medium:
// TU ごとの重複定義は列並び替え時の片側更新漏れで validation が黙って壊れるため)。
using detail::kColName;
using detail::kColPose;
using detail::kColTolerance;
using detail::kColTags;

bool PoiEditorPanel::ValidatePois()
{
  int numRows = ui_->PoiTable->rowCount();
  QStringList warnings;
  std::set<std::string> names_seen;

  for (int row = 0; row < numRows; row++) {
    int logical_row = ui_->PoiTable->verticalHeader()->logicalIndex(row);

    // Check name
    auto* name_item = ui_->PoiTable->item(logical_row, kColName);
    std::string name = name_item ? name_item->text().toStdString() : "";
    if (name.empty()) {
      warnings.append(tr("Row %1: name is empty").arg(row + 1));
    } else if (names_seen.count(name) > 0) {
      warnings.append(tr("Row %1: duplicate name \"%2\"").arg(row + 1).arg(QString::fromStdString(name)));
    }
    names_seen.insert(name);

    // Check pose format (expect "x, y, yaw" — 3 elements)。判定は純関数に委譲 (#346)。
    auto* pose_item = ui_->PoiTable->item(logical_row, kColPose);
    std::string pose_str = pose_item ? pose_item->text().toStdString() : "";
    const auto pose_result = detail::validate_pose_cell(pose_str);
    if (pose_result.status == detail::PoseCellStatus::kWrongFieldCount) {
      warnings.append(tr("Row %1: pose must be \"x, y, yaw\" (3 values, got \"%2\")")
                        .arg(row + 1).arg(QString::fromStdString(pose_result.raw)));
    } else if (pose_result.status == detail::PoseCellStatus::kInvalidValue) {
      warnings.append(tr("Row %1: pose contains invalid value \"%2\"")
                        .arg(row + 1).arg(QString::fromStdString(pose_result.invalid_value)));
    }

    // Check tolerance "xy m, yaw rad" (1 column 統合、#158)。判定は純関数に委譲 (#346)。
    // min: tolerance.xy >= 0.001 m / tolerance.yaw >= 0.001 rad ≒ 0.057° (#138 msg spec)。
    auto* tolerance_item = ui_->PoiTable->item(logical_row, kColTolerance);
    std::string tolerance_str = tolerance_item ? tolerance_item->text().toStdString() : "";
    const auto tol = detail::validate_tolerance_cell(tolerance_str);
    if (!tol.format_ok) {
      warnings.append(tr("Row %1: tolerance must be \"xy, yaw_rad\" (got \"%2\")")
                        .arg(row + 1).arg(QString::fromStdString(tolerance_str)));
    } else {
      if (tol.xy_status == detail::ToleranceFieldStatus::kParseError) {
        warnings.append(tr("Row %1: invalid tolerance.xy \"%2\"")
                          .arg(row + 1).arg(QString::fromStdString(tol.xy_raw)));
      } else if (tol.xy_status == detail::ToleranceFieldStatus::kBelowMinimum) {
        warnings.append(tr("Row %1: tolerance.xy must be >= 0.001 m (got %2)")
                          .arg(row + 1).arg(tol.xy_value));
      }
      if (tol.yaw_status == detail::ToleranceFieldStatus::kParseError) {
        warnings.append(tr("Row %1: invalid tolerance.yaw \"%2\"")
                          .arg(row + 1).arg(QString::fromStdString(tol.yaw_raw)));
      } else if (tol.yaw_status == detail::ToleranceFieldStatus::kBelowMinimum) {
        warnings.append(tr("Row %1: tolerance.yaw must be >= 0.001 rad (≒ 0.057°) (got %2 rad)")
                          .arg(row + 1).arg(tol.yaw_value));
      } else if (tol.yaw_status == detail::ToleranceFieldStatus::kYawExceeds2Pi) {
        // 旧 deg 入力 (例: 45) を rad として誤って入れた場合の防御 (#159 round 2 軽微 medium)。
        // 2π (= 360°) 超は実用上意味のない異常値 (typo 可能性大) なので reject。
        // 「rad 入力」の仕様 (#158) を明示し、deg として解釈した場合の同等値も提示する。
        // (Qt MessageBox は plain text なので markdown 強調は使わない、#159 round 4 low)
        warnings.append(tr("Row %1: tolerance.yaw is in rad units (#158); value %2 exceeds 2π (= 360°). "
                           "If you meant deg, %2° = %3 rad.")
                          .arg(row + 1).arg(tol.yaw_value).arg(tol.yaw_value * M_PI / 180.0));
      }
    }
  }

  if (!warnings.isEmpty()) {
    QMessageBox::warning(this, tr("Validation Errors"), warnings.join("\n"));
    return false;
  }

  // Hard validation: landmark の排他 (#85)。判定は純関数に委譲 (#346)。
  // goal+landmark は意味矛盾 (Nav2 goal にできない reference 点)、
  // initial_pose+landmark は到達不可な POI を起点に置く矛盾。
  QStringList exclusivity_warnings;
  for (int row = 0; row < numRows; row++) {
    int logical_row = ui_->PoiTable->verticalHeader()->logicalIndex(row);
    auto* tags_item = ui_->PoiTable->item(logical_row, kColTags);
    std::string tags_str = tags_item ? tags_item->text().toStdString() : "";
    if (tags_str.empty()) continue;

    auto tags = detail::split_sentence(tags_str, ", ");
    const auto excl = detail::check_tag_exclusivity(tags);
    if (excl.waypoint_landmark_conflict) {
      exclusivity_warnings.append(
        tr("Row %1: \"waypoint\" and \"landmark\" cannot be used together."
           " A landmark is only a reference POI and is not sent to Nav2.").arg(row + 1));
    }
    // landmark × pause 排他 (#143): landmark は到達不可な reference なので
    // pause (= 到達したときに止める semantics) と意味的に矛盾する。
    if (excl.pause_landmark_conflict) {
      exclusivity_warnings.append(
        tr("Row %1: \"pause\" and \"landmark\" cannot be used together."
           " A landmark is not a navigation target, so the robot cannot pause there.").arg(row + 1));
    }
    // (initial_pose × landmark 排他は #144 で initial_pose system tag を廃止したため不要に。)
  }
  if (!exclusivity_warnings.isEmpty()) {
    QMessageBox::warning(this, tr("Tag Exclusivity Errors"), exclusivity_warnings.join("\n"));
    return false;
  }

  // Soft validation: warn about undefined tags (non-blocking)
  QStringList tag_warnings;
  for (int row = 0; row < numRows; row++) {
    int logical_row = ui_->PoiTable->verticalHeader()->logicalIndex(row);
    auto* tags_item = ui_->PoiTable->item(logical_row, kColTags);
    std::string tags_str = tags_item ? tags_item->text().toStdString() : "";
    if (tags_str.empty()) continue;

    auto tags = detail::split_sentence(tags_str, ", ");
    for (const auto& tag : tags) {
      if (tag.empty()) continue;
      bool found = false;
      for (const auto& known : known_tag_names_) {
        if (known == tag) {
          found = true;
          break;
        }
      }
      if (!found) {
        tag_warnings.append(tr("Row %1: undefined tag \"%2\"").arg(row + 1).arg(QString::fromStdString(tag)));
      }
    }
  }

  if (!tag_warnings.isEmpty()) {
    QString msg = tr("The following undefined tags were found:\n\n%1\n\nSave anyway?").arg(tag_warnings.join("\n"));
    auto ret = QMessageBox::question(this, tr("Undefined Tags"), msg,
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) {
      return false;
    }
  }

  return true;
}

}  // namespace mapoi_rviz_plugins
