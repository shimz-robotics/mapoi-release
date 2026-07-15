#pragma once

#include <array>

namespace mapoi {

struct SystemTagDef {
  const char * name;
  const char * description;
};

// system tag (#191): 旧 mapoi_server/maps/tag_definitions.yaml をハードコード化。
// system tag は変更非推奨 (削除/rename はコア機能の破壊的変更)、user tag は
// mapoi_config.yaml の custom_tags で拡張する設計のため、yaml 外出しのメリットが無い。
// header に出してあるのは unit test から件数 / name / description を固定検証するため。
inline constexpr std::array<SystemTagDef, 3> kSystemTags = {{
  {"waypoint", "Nav2 navigation target (single goal or route waypoint)"},
  {"landmark", "Reference point on the map (excluded from Nav2 navigation target)"},
  {"pause",    "Automatically pause navigation when robot enters the POI radius"},
}};

}  // namespace mapoi
