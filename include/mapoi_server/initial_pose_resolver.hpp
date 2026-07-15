#pragma once

// initial pose 候補 POI 名の選定 (#144 / #149 / #150)。
// `mapoi_server` (mapoi/initialpose_poi 配信) と `mapoi_gazebo_bridge` /
// `mapoi_gz_bridge` (robot spawn 位置決定) で共通する選定ロジックを純関数
// として切り出したもの。3 箇所の重複を排除して、`initial_poi_name` 仕様
// (除外条件 / fallback) が拡張された場合の simulator spawn と /initialpose の
// 挙動乖離を防ぐ (#150)。
//
// ロジック:
//   1) requested_name が指定されていれば、その POI を探す
//      - landmark タグ持ち → fall back
//      - pose ノード or x/y/yaw 欠落 / numeric 不可 → fall back
//   2) fall back: POI list 先頭で「landmark なし & pose 完備」の POI を採用
//   3) 候補なしなら空文字列を返す
//
// 前提: POI 名はマップ内で一意 (project 全体の暗黙前提、`mapoi_nav2_bridge` /
//   bridge / WebUI の lookup は全て「先頭一致で break」する design)。同名 POI
//   が複数定義された場合、本関数は先頭一致で判定するため、後続の同名 POI は
//   探索しない。yaml schema validation は ``scripts/check_sample_yaml_consistency.py``
//   等で別途担保する。
//
// state を持たないため呼び出し側で test しやすい (#149 round 4 high の意図を
// 維持)。

#include <string>

#include <yaml-cpp/yaml.h>


namespace mapoi
{

// 採用すべき initial POI 名を返す。候補なしなら空文字列。
std::string select_initial_poi_name(
  const YAML::Node & pois_list, const std::string & requested_name);

}  // namespace mapoi
