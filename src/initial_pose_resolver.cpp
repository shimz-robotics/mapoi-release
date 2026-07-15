#include "mapoi_server/initial_pose_resolver.hpp"


namespace mapoi
{

namespace
{

bool is_landmark_poi(const YAML::Node & poi)
{
  if (!poi["tags"] || !poi["tags"].IsSequence()) return false;
  for (const auto & t : poi["tags"]) {
    if (t.as<std::string>() == "landmark") return true;
  }
  return false;
}

// pose ノード + x / y / yaw 全てを numeric として読めるかを確認する。
// bridge 側は欠落時 0.0 fallback なので、ここで弾けば「意図しない (0,0) spawn」を防げる。
bool has_valid_pose(const YAML::Node & poi)
{
  if (!poi["pose"]) return false;
  const auto & pose = poi["pose"];
  if (!pose.IsMap()) return false;
  for (const char * k : {"x", "y", "yaw"}) {
    if (!pose[k]) return false;
    try {
      (void)pose[k].as<double>();
    } catch (const YAML::Exception &) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string select_initial_poi_name(
  const YAML::Node & pois_list, const std::string & requested_name)
{
  if (!pois_list || !pois_list.IsSequence()) {
    return "";
  }
  if (!requested_name.empty()) {
    for (const auto & poi : pois_list) {
      if (poi["name"].as<std::string>("") != requested_name) continue;
      if (is_landmark_poi(poi)) break;     // landmark → fall back
      if (!has_valid_pose(poi)) break;     // pose 欠落 → fall back
      return requested_name;
    }
    // not-found 警告は呼び出し側で扱う (state-less 化のため log は出さない)
  }
  for (const auto & poi : pois_list) {
    if (is_landmark_poi(poi)) continue;     // landmark は到達不可、initial pose 候補から除外
    if (!has_valid_pose(poi)) continue;     // pose 欠落 POI もスキップ
    const std::string n = poi["name"].as<std::string>("");
    if (!n.empty()) return n;
  }
  return "";
}

}  // namespace mapoi
