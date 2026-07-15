// Pure decision helpers for mapoi/config_path callbacks (#169).
// Qt / ROS 依存を持たせず、Panel callback はこの判定結果を UI 更新関数へ写像するだけにする。
#pragma once

#include <filesystem>
#include <string>

namespace mapoi_rviz_plugins::detail
{

enum class ConfigPathUpdateAction
{
  ReinitializeMap,
  RefreshCurrentMap,
  Noop,
};

inline ConfigPathUpdateAction decide_poi_editor_config_path_action(
  const std::string & current_map,
  const std::string & new_map,
  const std::string & current_config_path)
{
  if (current_config_path.empty() || current_map != new_map) {
    return ConfigPathUpdateAction::ReinitializeMap;
  }
  return ConfigPathUpdateAction::RefreshCurrentMap;
}

inline ConfigPathUpdateAction apply_poi_editor_content_update_suppression(
  ConfigPathUpdateAction action,
  bool suppress_content_update)
{
  if (action == ConfigPathUpdateAction::RefreshCurrentMap && suppress_content_update) {
    return ConfigPathUpdateAction::Noop;
  }
  return action;
}

inline ConfigPathUpdateAction decide_mapoi_panel_config_path_action(
  const std::string & current_map,
  const std::string & new_map)
{
  return current_map != new_map
    ? ConfigPathUpdateAction::ReinitializeMap
    : ConfigPathUpdateAction::RefreshCurrentMap;
}

// 内容 diff ガード (#403)。publisher (mapoi_rviz2_publisher の path+mtime dedup) と同型の
// 判定を両パネルで共有する。RefreshCurrentMap のみ dedup 対象:
//   - ReinitializeMap は map 切替 = path が必ず変わるため dedup 不成立 (素通し)
//   - stat 失敗 (stat_ok=false) は dedup 不能として従来挙動 (素通し) — publisher と同じ方針
//   - path も mtime も前回と同一 → Noop (再 fetch / 再構築を抑制)
// mtime が変わった場合は従来通り再取得する (save 後の reload_map_info による同一 path
// 再 publish で内容が変わる flow (#135) を壊さない)。
inline ConfigPathUpdateAction apply_config_content_dedup(
  ConfigPathUpdateAction action, bool stat_ok,
  const std::string & path, std::filesystem::file_time_type mtime,
  const std::string & last_path, std::filesystem::file_time_type last_mtime)
{
  if (action != ConfigPathUpdateAction::RefreshCurrentMap) {
    return action;  // ReinitializeMap / Noop は dedup 対象外 (素通し)
  }
  if (!stat_ok) {
    return action;  // stat 失敗: dedup 不能として従来挙動 (素通し)
  }
  if (path == last_path && mtime == last_mtime) {
    return ConfigPathUpdateAction::Noop;  // path も mtime も不変 → 再 fetch 不要
  }
  return action;  // mtime が変わった = 内容が変わった可能性あり → 従来通り再取得
}

}  // namespace mapoi_rviz_plugins::detail
