#!/usr/bin/env python3
"""sample mapoi_config.yaml を validation する (#146 / PR #160 / #163 / #170 follow-up)。

**注意**: 名称は歴史的経緯で ``check_sample_yaml_consistency.py`` のまま残してい
るが、現在は ``mapoi_turtlebot3_example`` 配下の sample yaml の **個別 validation**
のみ行う。PR #170 (#163) 以前は ``mapoi_server`` / ``mapoi_turtlebot3_example`` の
**pair sync** 検査も含んでいたが、``mapoi_server`` から sample を廃止して single
source of truth に集約したため pair sync は不要になった。server 側に sample が
再発生していないか検知する仕組みは別 issue (#171) で計画。

mapoi_turtlebot3_example 配下のサンプル yaml が以下を満たすべき:

- POI 名のユニーク性
- ``route.waypoints`` / ``route.landmarks`` が POI 一覧に存在する
- ``route.waypoints`` の参照 POI は ``waypoint`` タグ持ち、``route.landmarks``
  の参照 POI は ``landmark`` タグ持ち (タグと route 用途のミスマッチを検出)
- tag 排他 (``waypoint × landmark``、``landmark × pause``、#85 / #143)
- ``tolerance.{xy,yaw}`` の範囲制約 (>= 0.001、yaw <= 2π、#138 / #158)
- ``poi`` 配列の先頭が ``landmark`` ではなく pose 完備 (= default initial pose、#149)

CI で本 script を回し、上記いずれかが破れたら fail させる。

**スコープ**: 本 script はサンプル yaml のドリフト防止を目的とした **軽量バリ
データ** で、``mapoi_server`` の yaml load (C++ 側) や ``mapoi_webui_node`` の
backend validation の網羅的レプリカではない。``map`` block / ``gazebo`` block
の妥当性、custom_tags 名規約、pose 数値の物理的妥当性などはチェックしない。

使い方::
    python3 scripts/check_sample_yaml_consistency.py

Exit codes:
- 0: OK
- 1: 整合性違反を検出 / yaml 構造異常
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent

# mapoi_server/maps の sample は #163 で廃止し、example 側のみが single source of truth。
# 本 script は example 側の individual validation のみ行う (旧 server / example pair sync は廃止)。
SAMPLE_FILES = [
    REPO_ROOT / 'mapoi_turtlebot3_example' / 'maps' / 'turtlebot3_world' / 'mapoi_config.yaml',
    REPO_ROOT / 'mapoi_turtlebot3_example' / 'maps' / 'turtlebot3_dqn_stage1' / 'mapoi_config.yaml',
]

TOL_MIN = 0.001
TOL_YAW_MAX = 2 * math.pi


def _load(path: Path) -> dict:
    with path.open() as f:
        data = yaml.safe_load(f) or {}
    if not isinstance(data, dict):
        raise SystemExit(f'ERROR: {path} の top level が dict ではない: {type(data).__name__}')
    return data


def _validate_single(path: Path, data: dict) -> list[str]:
    issues: list[str] = []
    rel = path.relative_to(REPO_ROOT)

    pois = data.get('poi') or []
    if not isinstance(pois, list) or not pois:
        issues.append(f'{rel}: "poi" 配列が空または存在しない')
        return issues

    names_seen: set[str] = set()
    for i, poi in enumerate(pois):
        if not isinstance(poi, dict):
            issues.append(f'{rel}: poi[{i}] が object ではない')
            continue
        name = poi.get('name')
        if not isinstance(name, str) or not name:
            issues.append(f'{rel}: poi[{i}] に有効な "name" がない')
            continue
        if name in names_seen:
            issues.append(f'{rel}: POI 名 "{name}" が重複している')
        names_seen.add(name)

        tags = poi.get('tags') or []
        tags_l = [str(t).lower() for t in tags] if isinstance(tags, list) else []
        if 'waypoint' in tags_l and 'landmark' in tags_l:
            issues.append(f'{rel}: POI "{name}" に "waypoint" と "landmark" を併用 (#85 排他)')
        if 'pause' in tags_l and 'landmark' in tags_l:
            issues.append(f'{rel}: POI "{name}" に "pause" と "landmark" を併用 (#143 排他)')

        tol = poi.get('tolerance')
        if not isinstance(tol, dict):
            issues.append(f'{rel}: POI "{name}" の "tolerance" が dict ではない')
        else:
            for key, max_value in (('xy', None), ('yaw', TOL_YAW_MAX)):
                v = tol.get(key)
                if not isinstance(v, (int, float)) or isinstance(v, bool):
                    issues.append(f'{rel}: POI "{name}" tolerance.{key} が数値ではない: {v!r}')
                    continue
                fv = float(v)
                if not math.isfinite(fv) or fv < TOL_MIN:
                    issues.append(
                        f'{rel}: POI "{name}" tolerance.{key} = {fv} が下限 {TOL_MIN} 未満'
                    )
                if max_value is not None and fv > max_value:
                    issues.append(
                        f'{rel}: POI "{name}" tolerance.{key} = {fv} が上限 {max_value:.6f} 超過'
                    )

    first = pois[0] if isinstance(pois[0], dict) else {}
    first_tags = [str(t).lower() for t in (first.get('tags') or [])]
    if 'landmark' in first_tags:
        issues.append(
            f'{rel}: POI list 先頭 "{first.get("name", "?")}" が landmark タグ持ち '
            f'(#149 default initial pose 採用条件に違反)'
        )

    poi_tags = {
        poi['name']: [str(t).lower() for t in (poi.get('tags') or [])]
        for poi in pois
        if isinstance(poi, dict) and isinstance(poi.get('name'), str)
    }
    routes = data.get('route') or []
    if isinstance(routes, list):
        route_names_seen: set[str] = set()
        for i, route in enumerate(routes):
            if not isinstance(route, dict):
                issues.append(f'{rel}: route[{i}] が object ではない')
                continue
            rname = route.get('name')
            if not isinstance(rname, str) or not rname:
                issues.append(f'{rel}: route[{i}] に有効な "name" がない')
                continue
            if rname in route_names_seen:
                issues.append(f'{rel}: route 名 "{rname}" が重複している')
            route_names_seen.add(rname)
            for field, expected_tag in (('waypoints', 'waypoint'), ('landmarks', 'landmark')):
                refs = route.get(field) or []
                if not isinstance(refs, list):
                    issues.append(f'{rel}: route "{rname}" の {field} が list ではない')
                    continue
                for ref in refs:
                    if ref not in names_seen:
                        issues.append(
                            f'{rel}: route "{rname}" の {field} が未定義 POI を参照: "{ref}"'
                        )
                        continue
                    if expected_tag not in poi_tags.get(ref, []):
                        issues.append(
                            f'{rel}: route "{rname}" の {field} に列挙された POI "{ref}" に '
                            f'"{expected_tag}" タグがない (route 用途と tag のミスマッチ)'
                        )

    return issues


def main() -> int:
    issues: list[str] = []

    for path in SAMPLE_FILES:
        if not path.exists():
            issues.append(f'sample yaml が存在しない: {path.relative_to(REPO_ROOT)}')
            continue
        issues.extend(_validate_single(path, _load(path)))

    if issues:
        print('sample yaml consistency check failed:', file=sys.stderr)
        for i in issues:
            print(f'  - {i}', file=sys.stderr)
        return 1

    print(f'OK: {len(SAMPLE_FILES)} sample yaml file(s) are valid.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
