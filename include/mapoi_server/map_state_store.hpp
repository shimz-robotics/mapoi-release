#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mapoi
{

// #297: last-selected map の永続化 store。state file (<state_dir>/last_selected_map) の
// **有無**で「真の初回起動 (無し)」と「運用中の crash/再起動 (有り)」を判別するのが契約。
// initial_pose_resolver と同じく Node 非依存に切り出し、unit test を軽量に保つ。

// state file 名 (state_dir 直下)。
inline constexpr const char * kLastMapStateFile = "last_selected_map";

// state file を読む。
// - nullopt: state file が存在しない (= 初回起動)
// - has_value: state file が存在する (= 運用中再起動)。値は whitespace trim 済みの
//   1 行目 map 名。読めない / 内容が空の場合は empty string (restore 可否は呼び出し側判断)。
std::optional<std::string> read_last_map(const std::filesystem::path & state_dir);

// map 名が単一の directory 名として安全かを検査する (path traversal 防止、PR #327 review)。
// state file は破損・手動編集されうる外部入力なので、separator ('/' '\\') や "." ".." を
// 含む値を path segment として maps_path に連結する前に必ずこれで弾く。
bool is_plain_directory_name(const std::string & name);

// map 名を state file に書く。state_dir が無ければ作成する。
// crash 耐性のため直接上書きせず tmp file へ書き切ってから rename する (POSIX atomic)。
// 失敗時は false を返し error_message に理由を格納 (log 出力は呼び出し側の責務)。
bool write_last_map(
  const std::filesystem::path & state_dir, const std::string & map_name,
  std::string & error_message);

}  // namespace mapoi
