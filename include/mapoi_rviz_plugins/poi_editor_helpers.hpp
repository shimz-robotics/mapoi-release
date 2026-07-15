// Pure helper functions for PoiEditorPanel (#158 で header 化、unit test 容易化のため)。
// Qt 依存なし。stdlib のみ + geometry_msgs / tf2 (calc_yaw のみ)。
// NOTE: calc_yaw のために tf2/geometry_msgs 依存が加わったが、それ以外の関数は
//       引き続き stdlib のみ (テスト時は calc_yaw のテストだけ tf2 link が要る)。
#pragma once

#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <geometry_msgs/msg/pose.hpp>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Quaternion.h>

#include "mapoi_rviz_plugins/config_path_update_policy.hpp"

namespace mapoi_rviz_plugins::detail
{

// tolerance.xy / tolerance.yaw の最小値 (msg spec、poi-filter.js の TOLERANCE_MIN と同じ値、#138)。
constexpr double kToleranceMin = 0.001;

// PoiTable column index 定数 (#158 round 1 medium): magic number を排除して
// 「name → pose → tolerance → tags → description」順序を 1 箇所に集約。
// #346 の TU 分割で poi_editor.cpp / poi_editor_validation.cpp の両方から参照される
// ため header へ移動 (PR #371 review medium: TU ごとの重複定義は列並び替え時に
// 片方だけ更新されて validation が黙って壊れるリスクがあった)。
constexpr int kColName = 0;
constexpr int kColPose = 1;
constexpr int kColTolerance = 2;
constexpr int kColTags = 3;
constexpr int kColDescription = 4;
constexpr int kColCount = 5;

// PoiEditorPanel::PoiPoseCallback の frame_id 検証 (#430)。MapoiPoseTool は RViz2 の
// Fixed Frame で pose を publish するが、POI pose は mapoi_server 側で frame_id="map" 前提の
// goal として扱われる。Fixed Frame が "map" でない状態で反映すると、raw な x/y/theta が
// 別 frame の座標のまま "map" 座標として保存され、ロボットを誤誘導する (修正案2採用: 警告して
// 反映を拒否する。tf2 変換は依存追加になるため導入しない)。
// 先頭の '/' は古いノードが frame_id を "/map" 形式 (tf1 由来の絶対 frame 表記) で
// publish するケースを許容するため正規化してから比較する。
inline bool is_map_frame(const std::string & frame_id)
{
  std::string normalized = frame_id;
  if (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(0, 1);
  }
  return normalized == "map";
}

// 文字列を strict に double に parse し、有限性を確認する純関数 (Codex review #139 medium 対応)。
// std::stod は "1abc" を 1 として返し NaN/Inf も throw しないため、validation と save で
// 同じ parser を使うために helper にする。min check は呼び出し側の責任。
//
// success: parsed value を out に書き、true を返す。
// failure: out 不変、false を返す (parse failure / 末尾 garbage / NaN / +-Inf いずれも reject)。
inline bool try_parse_finite_double(const std::string & str, double & out)
{
  if (str.empty()) return false;
  try {
    size_t pos = 0;
    const double v = std::stod(str, &pos);
    while (pos < str.size() && std::isspace(static_cast<unsigned char>(str[pos]))) ++pos;
    if (pos != str.size()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

// `delimiter` で区切って trim ( leading/trailing whitespace + 改行を除去) した結果を返す。
// **中間の空要素は保持** (`"a,,b"` → `["a", "", "b"]`) するが、**末尾の空要素は std::getline
// 仕様で省略される** (`"0.5,"` → `["0.5"]`)。tolerance validation は「2 要素必須」check で
// 末尾区切りを拒否できる。先頭空要素は保持される (`",0.5"` → `["", "0.5"]`)。
// `0.5,45.0` `0.5, 45.0` `0.5 , 45.0` 等の表記揺れと改行混入 (`copy & paste 由来`) を許容する。
inline std::vector<std::string> split_and_trim(const std::string & input, char delimiter)
{
  std::vector<std::string> result;
  std::stringstream ss(input);
  std::string item;
  while (std::getline(ss, item, delimiter)) {
    const auto begin = item.find_first_not_of(" \t\r\n");
    const auto end = item.find_last_not_of(" \t\r\n");
    if (begin == std::string::npos) {
      result.push_back("");
    } else {
      result.push_back(item.substr(begin, end - begin + 1));
    }
  }
  return result;
}

// PoiEditorPanel::ValidatePois の pose セル ("x, y, yaw") 判定 (#346)。判定ロジックのみを
// 純関数化し、QMessageBox 向けの文言組み立ては呼び出し側 (poi_editor_validation.cpp) が
// 元の文字列そのまま (raw / invalid_value) を使って行う — Qt の `.arg(double)` 書式化に
// 依存する数値以外は、ここでは文字列のまま持ち回って表示文言を一切変えない。
enum class PoseCellStatus
{
  kOk,
  kWrongFieldCount,  // "x, y, yaw" の 3 要素に split できなかった
  kInvalidValue,     // いずれかの要素が有限 double として parse できなかった
};

struct PoseCellValidation
{
  PoseCellStatus status = PoseCellStatus::kWrongFieldCount;
  std::string raw;            // 元の pose_str (kWrongFieldCount のメッセージに使う)
  std::string invalid_value;  // parse 失敗した要素 (kInvalidValue のメッセージに使う)
};

inline PoseCellValidation validate_pose_cell(const std::string & pose_str)
{
  PoseCellValidation result;
  result.raw = pose_str;
  const auto parts = split_and_trim(pose_str, ',');
  if (parts.size() != 3) {
    result.status = PoseCellStatus::kWrongFieldCount;
    return result;
  }
  for (const auto & part : parts) {
    double v = 0.0;
    if (!try_parse_finite_double(part, v)) {
      result.status = PoseCellStatus::kInvalidValue;
      result.invalid_value = part;
      return result;
    }
  }
  result.status = PoseCellStatus::kOk;
  return result;
}

// PoiEditorPanel::ValidatePois の tolerance セル ("xy_m, yaw_rad") 判定 (#346)。
// xy / yaw は独立に評価する (両方エラーがあれば呼び出し側は両方の警告を積める)。
enum class ToleranceFieldStatus
{
  kOk,
  kParseError,     // 有限 double として parse できない
  kBelowMinimum,   // 0 以上だが kToleranceMin 未満
  kYawExceeds2Pi,  // yaw のみ: 2π 超過 (#159、deg 入力の取り違え防御)
};

struct ToleranceCellValidation
{
  bool format_ok = false;  // "xy, yaw" の 2 要素に split できたか
  std::string xy_raw;      // parts[0] (kParseError のメッセージに使う)
  std::string yaw_raw;     // parts[1] (kParseError のメッセージに使う)
  ToleranceFieldStatus xy_status = ToleranceFieldStatus::kOk;
  double xy_value = 0.0;   // xy_status==kOk|kBelowMinimum 時のみ意味を持つ
  ToleranceFieldStatus yaw_status = ToleranceFieldStatus::kOk;
  double yaw_value = 0.0;  // yaw_status==kOk|kBelowMinimum|kYawExceeds2Pi 時のみ意味を持つ
};

inline ToleranceCellValidation validate_tolerance_cell(const std::string & tolerance_str)
{
  ToleranceCellValidation result;
  const auto parts = split_and_trim(tolerance_str, ',');
  if (parts.size() != 2) {
    result.format_ok = false;
    return result;
  }
  result.format_ok = true;
  result.xy_raw = parts[0];
  result.yaw_raw = parts[1];

  double xy_val = 0.0;
  if (!try_parse_finite_double(parts[0], xy_val)) {
    result.xy_status = ToleranceFieldStatus::kParseError;
  } else {
    result.xy_value = xy_val;
    if (xy_val < kToleranceMin) {
      result.xy_status = ToleranceFieldStatus::kBelowMinimum;
    }
  }

  double yaw_val = 0.0;
  if (!try_parse_finite_double(parts[1], yaw_val)) {
    result.yaw_status = ToleranceFieldStatus::kParseError;
  } else {
    result.yaw_value = yaw_val;
    if (yaw_val < kToleranceMin) {
      result.yaw_status = ToleranceFieldStatus::kBelowMinimum;
    } else if (yaw_val > 2.0 * M_PI) {
      result.yaw_status = ToleranceFieldStatus::kYawExceeds2Pi;
    }
  }
  return result;
}

// PoiEditorPanel::ValidatePois のタグ排他判定 (#85 / #143、#346 で純関数化)。
// waypoint+landmark (landmark は Nav2 navigation 不可) と pause+landmark (landmark は
// 到達不可なので pause が成立しない) の 2 組を独立に判定する。
struct TagExclusivityResult
{
  bool waypoint_landmark_conflict = false;
  bool pause_landmark_conflict = false;
};

inline TagExclusivityResult check_tag_exclusivity(const std::vector<std::string> & tags)
{
  bool has_waypoint = false;
  bool has_landmark = false;
  bool has_pause = false;
  for (const auto & t : tags) {
    if (t == "waypoint") has_waypoint = true;
    else if (t == "landmark") has_landmark = true;
    else if (t == "pause") has_pause = true;
  }
  TagExclusivityResult result;
  result.waypoint_landmark_conflict = has_waypoint && has_landmark;
  result.pause_landmark_conflict = has_pause && has_landmark;
  return result;
}

// PoiEditorPanel::ConfigPathCallback の未保存編集ガード判定 (#399)。
// 外部で mapoi/config_path が再 publish された時に、テーブルを無条件で全再構築すると
// 未保存のセル編集が黙って消える。suppression 適用後の action・dirty フラグ・dialog 表示中
// フラグの 3 つから、UI が「そのまま再構築 / 確認ダイアログ / イベント破棄」のいずれを
// 取るべきかを判定する純関数。Qt 非依存 (callback 側が結果を QMessageBox 表示へ写像する)。
enum class ConfigReloadGuardDecision
{
  kProceed,   // 従来通り再構築 (Noop なら何もしない、それ以外は InitConfigs/UpdatePoiTable)
  kAskUser,   // 未保存編集ありのため確認ダイアログを出す
  kDrop,      // dialog 表示中に届いたイベントは破棄 (再入・dialog 積み重ね防止)
};

// action    : suppression 適用後の ConfigPathUpdateAction (Noop = suppress 中 or 更新不要)
// table_dirty: UI スレッド上のユーザー編集有無 (未保存編集ガードの主軸)
// dialog_open: 既に確認ダイアログを表示中か (QMessageBox のネストイベントループ中に
//              後続 queued lambda が走って dialog が積み重なるのを防ぐ)
inline ConfigReloadGuardDecision decide_config_reload_guard(
  ConfigPathUpdateAction action,
  bool table_dirty,
  bool dialog_open)
{
  // 再構築対象が無い (Noop = suppress 中 or 同一 map で更新不要) なら dirty でも何もしない。
  // Noop は table を触らないので未保存編集は消えない。
  if (action == ConfigPathUpdateAction::Noop) {
    return ConfigReloadGuardDecision::kProceed;
  }
  // dialog 表示中に届いたイベントは破棄する。ユーザーが「再読込」を選べばその時点の
  // 最新状態が fetch されるため drop で欠損しない (再入防止)。
  if (dialog_open) {
    return ConfigReloadGuardDecision::kDrop;
  }
  // 未保存編集がある状態での外部変更は、破棄可否をユーザーに確認する。
  if (table_dirty) {
    return ConfigReloadGuardDecision::kAskUser;
  }
  // dirty でなければ従来通り再構築 (サーバ状態に追従)。
  return ConfigReloadGuardDecision::kProceed;
}

// PoiEditorPanel::TagFilterChanged の未保存編集ガード判定 (#428)。
// タグフィルタ combo の activated(int) は同じ項目を選び直しても発火し、フィルタ適用は
// 行を setRowCount(0) で丸ごと入れ替えて undo_stack_->clear() するため、未保存編集を
// 無警告で破棄してしまう (#399 の decide_config_reload_guard は config_path 変更経路のみ
// カバー)。current index / 適用済み index / dirty から、UI が「何もしない / 確認ダイアログ /
// そのまま適用」のいずれを取るべきかを判定する純関数。Qt 非依存 (呼び出し側が結果を
// QMessageBox 表示 + combo revert へ写像する)。
enum class TagFilterChangeDecision
{
  kNoop,      // 適用済みと同じ index の再選択 (フィルタ結果不変なので何もしない)
  kAskUser,   // 未保存編集ありのため確認ダイアログを出す
  kProceed,   // そのままフィルタを適用する
};

// current_index : combo の activated で通知された選択 index
// applied_index : 最後にテーブルへ適用したフィルタ index (member で追跡)
// table_dirty   : UI スレッド上のユーザー編集有無 (未保存編集ガードの主軸)
inline TagFilterChangeDecision decide_tag_filter_change(
  int current_index,
  int applied_index,
  bool table_dirty)
{
  // 同一 index の再選択はフィルタ結果が変わらないので何もしない (dirty でも問わない)。
  // activated は同じ項目を選び直しただけでも発火するため冒頭で弾く。
  if (current_index == applied_index) {
    return TagFilterChangeDecision::kNoop;
  }
  // 未保存編集がある状態でのフィルタ変更は、破棄可否をユーザーに確認する。
  if (table_dirty) {
    return TagFilterChangeDecision::kAskUser;
  }
  // dirty でなければそのまま適用する (サーバ状態は変えず表示行を絞るだけ)。
  return TagFilterChangeDecision::kProceed;
}

// PoiEditorPanel::SaveButton の外部変更検出判定 (#399)。保存先ファイルが baseline (最後に
// テーブルへ読み込んだ / 保存した時点の内容) と食い違う場合に上書き確認を出すべきかを返す。
// WebUI の expected_version 楽観ロック相当のクライアント側実装で、config_version の厳密共有は
// 不要で内容比較で目的を達せられる。
//
// PR #413 review (low: itemText(0)/currentText 非対称) を踏まえ、比較は **保存先 path**
// (currentText 由来) が baseline_path と一致する時のみ行う。FileComboBox「the other」で
// 別ファイルを保存先に指定した場合は基準を持たないため比較しない (誤った基準ファイルとの
// 比較を避ける)。baseline_content が空 (読めなかった等) の場合もガード無効化 = 従来挙動。
inline bool should_confirm_overwrite(
  const std::string & save_path,
  const std::string & baseline_path,
  const std::string & baseline_content,
  const std::string & current_content)
{
  if (save_path != baseline_path) {
    return false;  // 保存先が baseline と別 path: 比較不能 (基準なし)
  }
  if (baseline_content.empty()) {
    return false;  // baseline を読めなかった: ガード無効 (従来挙動)
  }
  return baseline_content != current_content;  // path 一致 && baseline 非空 && 内容不一致
}

// PoiEditorPanel::calcYaw から移植 (#397 step 8 で自由関数化)。
// geometry_msgs::msg::Pose の orientation (quaternion) から yaw 角 [rad] を返す。
// tf2::Quaternion / tf2::Matrix3x3::getRPY を使用。
// 入力は単位四元数前提 (mapoi_server / Editor 側で保証)。非正規化・ゼロ四元数の挙動は
// 未定義に近い (PR #425 review low の契約明示)。
// 命名規約: detail 内の既存関数群は snake_case なので calc_yaw に rename (全 call site 置換済み)。
// 出典: #397 step 8 (元のメンバ関数 PoiEditorPanel::calcYaw から移動)。
inline double calc_yaw(const geometry_msgs::msg::Pose & pose)
{
  tf2::Quaternion q(
    pose.orientation.x,
    pose.orientation.y,
    pose.orientation.z,
    pose.orientation.w);
  tf2::Matrix3x3 m(q);
  double roll, pitch, yaw;
  m.getRPY(roll, pitch, yaw);
  return yaw;
}

// PoiEditorPanel::join から移植 (#397 step 8 で自由関数化)。
// 文字列ベクタを delim で連結して返す。delim が nullptr の場合は単純連結。
// 命名規約: join は元名と一致 (既存 snake_case 慣例と同一形)。
// 出典: https://marycore.jp/prog/cpp/vector-join/
inline std::string join(const std::vector<std::string> & v, const char * delim)
{
  std::string s;
  if (!v.empty()) {
    s += v[0];
    for (decltype(v.size()) i = 1, c = v.size(); i < c; ++i) {
      if (delim) s += delim;
      s += v[i];
    }
  }
  return s;
}

// PoiEditorPanel::SplitSentence から移植 (#397 step 8 で自由関数化)。
// sentence を delimiter で分割し、末尾トークン含む全トークンのベクタを返す。
// 命名規約: detail 内の既存関数群は snake_case なので split_sentence に rename (全 call site 置換済み)。
// 出典: https://lilaboc.work/archives/19026007.html
inline std::vector<std::string> split_sentence(std::string sentence, std::string delimiter)
{
  std::vector<std::string> words;
  size_t position = 0;

  // 空 delimiter は find が現在位置にマッチし続け position が進まず無限ループするため、
  // 分割せず全体を 1 要素で返す (自由関数化で再利用面が広がるためのガード、PR #425 review)。
  if (delimiter.empty()) {
    return {sentence};
  }

  while (sentence.find(delimiter.c_str(), position) != std::string::npos) {
    size_t next_position = sentence.find(delimiter.c_str(), position);
    std::string word = sentence.substr(position, next_position - position);
    position = next_position + delimiter.length();
    words.push_back(word);
  }
  std::string last_word = sentence.substr(position, sentence.length() - position);
  words.push_back(last_word);

  return words;
}

// PoiEditorPanel の New/Copy 行挿入 (#434) の視覚移動先を求める純関数。Qt 非依存。
// 行ドラッグで視覚順 (visual) ≠ 論理順 (logical) に並べ替え済みのテーブルで New/Copy すると、
// 挿入行は Qt の section 挿入規則で選択行と無関係な視覚位置に現れる。これを選択行の視覚直下へ
// moveSection するための移動先 visual index を返す。
//
//   inserted_visual: 挿入直後の新規行の現在 visual index
//   ref_visual     : 直上に置きたい選択行の現在 visual index
//   (両者は別の行なので inserted_visual != ref_visual を前提)
//
// QHeaderView::moveSection(from, to) は「visual from の section を取り除き、残り列の中で
// visual to の位置へ挿し込む」意味論。選択行の直後 (ref_visual+1) に入れたいが、取り除く行が
// 選択行より上 (inserted_visual < ref_visual) の場合は除去で選択行が 1 つ繰り上がるため、
// 移動先は ref_visual のまま。選択行より下 (inserted_visual > ref_visual) から動かす場合のみ
// +1 する。これで inserted_visual の位置に依らず「選択行の視覚直下」に収まる。
inline int insert_move_target_visual(int inserted_visual, int ref_visual)
{
  return ref_visual + (inserted_visual > ref_visual ? 1 : 0);
}

}  // namespace mapoi_rviz_plugins::detail
