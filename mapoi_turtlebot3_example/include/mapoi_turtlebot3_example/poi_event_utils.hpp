#ifndef MAPOI_TURTLEBOT3_EXAMPLE__POI_EVENT_UTILS_HPP_
#define MAPOI_TURTLEBOT3_EXAMPLE__POI_EVENT_UTILS_HPP_

#include <algorithm>
#include <string>
#include <vector>

namespace mapoi_turtlebot3_example
{

// PoiEvent.poi.tags に target tag が含まれるかを判定する共通ユーティリティ。
// camera_node / audio_guide_node が同型の実装を重複保持していたのを集約した
// (#248 項目 8)。PoiEvent 駆動 subscriber 全般で再利用する。
inline bool has_tag(const std::vector<std::string> & tags, const std::string & target)
{
  return std::find(tags.begin(), tags.end(), target) != tags.end();
}

}  // namespace mapoi_turtlebot3_example

#endif  // MAPOI_TURTLEBOT3_EXAMPLE__POI_EVENT_UTILS_HPP_
