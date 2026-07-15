#!/usr/bin/env python3
"""mapoi_turtlebot3_example の robot_radius 値が WebUI launch と Nav2 yaml で一致するかを検査する。

WebUI 側 (mapoi_webui の `robot_radius` launch param) と Nav2 側
(`controller_server` / `local_costmap` / `global_costmap` 等の `robot_radius`)
は意味が同じだが、現状 mapoi_turtlebot3_example では 2 ソースに値を持っており、
片方だけ修正されると drift する (#117 / #127)。

CI で本 script を回し、両者がずれた場合に fail させる。

使い方::
    python3 scripts/check_robot_radius_drift.py

Exit codes:
- 0: 一致 (OK)
- 1: drift / 構造異常を検出
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent

LAUNCH_YAML = REPO_ROOT / 'mapoi_turtlebot3_example' / 'launch' / 'turtlebot3_navigation.launch.yaml'
NAV2_PARAM_DIR = REPO_ROOT / 'mapoi_turtlebot3_example' / 'param'
DISTROS = ('humble', 'jazzy')
NAV2_YAML_NAME = 'burger.yaml'
WEBUI_INCLUDE_TARGET = 'mapoi_webui'


def find_robot_radius_in_nav2_yaml(yaml_path: Path) -> list[tuple[tuple[Any, ...], float]]:
    """Walk a Nav2 param yaml and collect every `robot_radius` (path, value) pair.

    Nav2 では local_costmap / global_costmap の各 namespace に同名の key が
    並ぶことが普通なので、見つかった全ての値を path 付きで返す。
    """
    with yaml_path.open() as f:
        data = yaml.safe_load(f)

    found: list[tuple[tuple[Any, ...], float]] = []

    def walk(node: Any, path: tuple[Any, ...] = ()) -> None:
        if isinstance(node, dict):
            for k, v in node.items():
                if k == 'robot_radius' and not isinstance(v, (dict, list)):
                    found.append((path + (k,), v))
                walk(v, path + (k,))
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, path + (i,))

    walk(data)
    return found


def find_robot_radius_in_launch_yaml(yaml_path: Path, target_include: str) -> str | None:
    """`launch` トップレベルの include 群から `target_include` を含む file 属性を探し、
    その include に渡された `robot_radius` arg の value を返す。
    """
    with yaml_path.open() as f:
        data = yaml.safe_load(f)

    items = data.get('launch', []) if isinstance(data, dict) else []
    for item in items:
        if not isinstance(item, dict):
            continue
        include = item.get('include')
        if not isinstance(include, dict):
            continue
        file_attr = str(include.get('file', ''))
        if target_include not in file_attr:
            continue
        args = include.get('arg', [])
        if not isinstance(args, list):
            continue
        for a in args:
            if isinstance(a, dict) and a.get('name') == 'robot_radius':
                return a.get('value')
    return None


def to_float(label: str, value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError) as e:
        raise SystemExit(
            f'ERROR: {label} = {value!r} を float に変換できません: {e}'
        )


def main() -> int:
    issues: list[str] = []

    if not LAUNCH_YAML.exists():
        print(f'ERROR: launch yaml が見つかりません: {LAUNCH_YAML}', file=sys.stderr)
        return 1

    launch_value_raw = find_robot_radius_in_launch_yaml(LAUNCH_YAML, WEBUI_INCLUDE_TARGET)
    if launch_value_raw is None:
        print(
            f'ERROR: {LAUNCH_YAML.relative_to(REPO_ROOT)} の "{WEBUI_INCLUDE_TARGET}" '
            f'include に robot_radius arg が見つかりません',
            file=sys.stderr,
        )
        return 1

    launch_value = to_float(f'launch ({LAUNCH_YAML.name})', launch_value_raw)

    for distro in DISTROS:
        nav2_yaml = NAV2_PARAM_DIR / distro / NAV2_YAML_NAME
        if not nav2_yaml.exists():
            issues.append(f'Nav2 yaml が存在しない: {nav2_yaml.relative_to(REPO_ROOT)}')
            continue
        nav2_values = find_robot_radius_in_nav2_yaml(nav2_yaml)
        if not nav2_values:
            issues.append(
                f'Nav2 yaml に robot_radius が見つからない: '
                f'{nav2_yaml.relative_to(REPO_ROOT)}'
            )
            continue
        for path, raw in nav2_values:
            label = f'Nav2 {distro} {".".join(map(str, path))}'
            v = to_float(label, raw)
            if v != launch_value:
                issues.append(
                    f'drift: launch={launch_value} '
                    f'({LAUNCH_YAML.relative_to(REPO_ROOT)}) vs '
                    f'{label}={v} ({nav2_yaml.relative_to(REPO_ROOT)})'
                )

    if issues:
        print('robot_radius drift detected:', file=sys.stderr)
        for i in issues:
            print(f'  - {i}', file=sys.stderr)
        return 1

    print(f'OK: launch and Nav2 ({", ".join(DISTROS)}) all share robot_radius = {launch_value}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
