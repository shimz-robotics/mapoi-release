// PoiEditorPanel::ValidatePois から切り出した純ロジックの unit test (#346)。
// pose/tolerance セルの判定と tag 排他判定は Qt (QMessageBox/QStringList) 非依存の
// 純関数として poi_editor_helpers.hpp に切り出してあり、poi_editor_validation.cpp の
// ValidatePois() はこれらの判定結果を元の文言 (英語 warning 文) に組み立てるだけ。
#include <gtest/gtest.h>

#include "mapoi_rviz_plugins/poi_editor_helpers.hpp"

using mapoi_rviz_plugins::detail::PoseCellStatus;
using mapoi_rviz_plugins::detail::ToleranceFieldStatus;
using mapoi_rviz_plugins::detail::check_tag_exclusivity;
using mapoi_rviz_plugins::detail::validate_pose_cell;
using mapoi_rviz_plugins::detail::validate_tolerance_cell;

// --- validate_pose_cell ---

TEST(ValidatePoseCell, ValidThreeValues)
{
  const auto result = validate_pose_cell("1.0, 2.0, 0.5");
  EXPECT_EQ(result.status, PoseCellStatus::kOk);
}

TEST(ValidatePoseCell, WrongFieldCountReturnsRawString)
{
  const auto result = validate_pose_cell("1.0, 2.0");
  EXPECT_EQ(result.status, PoseCellStatus::kWrongFieldCount);
  EXPECT_EQ(result.raw, "1.0, 2.0");
}

TEST(ValidatePoseCell, InvalidValueReturnsFirstFailingElement)
{
  // 2 番目の要素が不正な場合、最初に失敗した要素だけを返す (元実装の break と同じ挙動)。
  const auto result = validate_pose_cell("1.0, abc, 0.5");
  EXPECT_EQ(result.status, PoseCellStatus::kInvalidValue);
  EXPECT_EQ(result.invalid_value, "abc");
}

// --- validate_tolerance_cell ---

TEST(ValidateToleranceCell, ValidValuesBothOk)
{
  const auto result = validate_tolerance_cell("0.5, 0.7854");
  EXPECT_TRUE(result.format_ok);
  EXPECT_EQ(result.xy_status, ToleranceFieldStatus::kOk);
  EXPECT_DOUBLE_EQ(result.xy_value, 0.5);
  EXPECT_EQ(result.yaw_status, ToleranceFieldStatus::kOk);
  EXPECT_DOUBLE_EQ(result.yaw_value, 0.7854);
}

TEST(ValidateToleranceCell, WrongFieldCount)
{
  const auto result = validate_tolerance_cell("0.5");
  EXPECT_FALSE(result.format_ok);
}

TEST(ValidateToleranceCell, XyBelowMinimum)
{
  const auto result = validate_tolerance_cell("0.0001, 0.5");
  EXPECT_TRUE(result.format_ok);
  EXPECT_EQ(result.xy_status, ToleranceFieldStatus::kBelowMinimum);
}

TEST(ValidateToleranceCell, YawParseError)
{
  const auto result = validate_tolerance_cell("0.5, notanumber");
  EXPECT_TRUE(result.format_ok);
  EXPECT_EQ(result.yaw_status, ToleranceFieldStatus::kParseError);
}

TEST(ValidateToleranceCell, YawExceeds2PiLikelyDegreeMistake)
{
  // #159: 45 (deg のつもり) を rad として誤入力した典型例。2π (≒6.283) 超は reject。
  const auto result = validate_tolerance_cell("0.5, 45");
  EXPECT_TRUE(result.format_ok);
  EXPECT_EQ(result.yaw_status, ToleranceFieldStatus::kYawExceeds2Pi);
}

TEST(ValidateToleranceCell, YawAtUpperBoundIsAccepted)
{
  // 2π ちょうどは "超過" ではないので kOk (元実装の `> 2*M_PI` と同じ境界)。
  const auto result = validate_tolerance_cell("0.5, 6.283185307179586");
  EXPECT_EQ(result.yaw_status, ToleranceFieldStatus::kOk);
}

// --- check_tag_exclusivity ---

TEST(CheckTagExclusivity, NoConflictForPlainWaypoint)
{
  const auto result = check_tag_exclusivity({"waypoint"});
  EXPECT_FALSE(result.waypoint_landmark_conflict);
  EXPECT_FALSE(result.pause_landmark_conflict);
}

TEST(CheckTagExclusivity, WaypointAndLandmarkConflict)
{
  const auto result = check_tag_exclusivity({"waypoint", "landmark"});
  EXPECT_TRUE(result.waypoint_landmark_conflict);
  EXPECT_FALSE(result.pause_landmark_conflict);
}

TEST(CheckTagExclusivity, PauseAndLandmarkConflict)
{
  const auto result = check_tag_exclusivity({"pause", "landmark"});
  EXPECT_FALSE(result.waypoint_landmark_conflict);
  EXPECT_TRUE(result.pause_landmark_conflict);
}

TEST(CheckTagExclusivity, AllThreeTagsReportsBothConflicts)
{
  const auto result = check_tag_exclusivity({"waypoint", "pause", "landmark"});
  EXPECT_TRUE(result.waypoint_landmark_conflict);
  EXPECT_TRUE(result.pause_landmark_conflict);
}
