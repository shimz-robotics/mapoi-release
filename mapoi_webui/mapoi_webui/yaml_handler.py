"""YAML handler for mapoi config files.

Reads/writes mapoi_config.yaml, replacing only the 'poi' section
while preserving 'map' and 'route' sections and unknown fields.
"""

import hashlib
import math

import yaml


def compute_config_version(config_path):
    """Return a content-derived version string (sha256 hex) for the yaml file (#241).

    POI Save 時の楽観的競合検出に使う: frontend が GET 時に受け取った version を
    POST 時に送り返し、backend で current version と一致しなければ 409 を返す。
    内容ベースなので mtime 解像度 (1 秒) や外部 git checkout 等にも安定して反応する。
    ファイル不在時は None を返し、caller 側で「version check 不可」として 200 OK 経路に流す。
    """
    try:
        with open(config_path, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()
    except FileNotFoundError:
        return None


# yaml 書き出し時の有効桁数 (#242)。frontend (web/js/poi-filter.js) と必ず一致させる。
# xy は mm 精度 (3 桁)、yaw は rad で約 0.006° 精度 (4 桁) — 0.0001 rad ≒ 0.0057°。
# frontend readForm + backend save_pois の両層で丸めることで、yaml 直編集や非 WebUI
# クライアントからの POST も含めて yaml の値展開を抑える defense-in-depth。
_POSE_XY_DIGITS = 3
_POSE_YAW_DIGITS = 4
_TOLERANCE_XY_DIGITS = 3
_TOLERANCE_YAW_DIGITS = 4


def _round_finite(value, digits):
    """Round value to digits decimal places; return value unchanged if not finite."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return value
    if not math.isfinite(value):
        return value
    return round(float(value), digits)


def _round_poi_floats(pois):
    """Round POI pose / tolerance floats in-place before yaml.dump (#242).

    yaml.dump は float をそのまま 17 桁前後の repr で書き出すため、`0.10000038...` の
    ような ULP ノイズが yaml に残り git diff を汚す。frontend で丸めても yaml 直編集 +
    非 WebUI クライアントからの POST 経路があるので backend 側でも同じ桁数で丸める。
    """
    if not isinstance(pois, list):
        return
    for poi in pois:
        if not isinstance(poi, dict):
            continue
        pose = poi.get('pose')
        if isinstance(pose, dict):
            if 'x' in pose:
                pose['x'] = _round_finite(pose['x'], _POSE_XY_DIGITS)
            if 'y' in pose:
                pose['y'] = _round_finite(pose['y'], _POSE_XY_DIGITS)
            if 'yaw' in pose:
                pose['yaw'] = _round_finite(pose['yaw'], _POSE_YAW_DIGITS)
        tolerance = poi.get('tolerance')
        if isinstance(tolerance, dict):
            if 'xy' in tolerance:
                tolerance['xy'] = _round_finite(tolerance['xy'], _TOLERANCE_XY_DIGITS)
            if 'yaw' in tolerance:
                tolerance['yaw'] = _round_finite(tolerance['yaw'], _TOLERANCE_YAW_DIGITS)


def load_config(config_path):
    """Load a mapoi_config.yaml file and return parsed dict."""
    with open(config_path, 'r') as f:
        return yaml.safe_load(f)


def save_pois(config_path, pois):
    """Replace only the 'poi' section in config_path, preserving other sections.

    Args:
        config_path: Path to mapoi_config.yaml
        pois: List of POI dicts to write
    """
    config = load_config(config_path)
    _round_poi_floats(pois)
    config['poi'] = pois
    with open(config_path, 'w') as f:
        yaml.dump(config, f, default_flow_style=None, allow_unicode=True, sort_keys=False)


def save_routes(config_path, routes):
    """Replace only the 'route' section in config_path, preserving other sections.

    Args:
        config_path: Path to mapoi_config.yaml
        routes: List of route dicts to write
    """
    config = load_config(config_path)
    config['route'] = routes
    with open(config_path, 'w') as f:
        yaml.dump(config, f, default_flow_style=None, allow_unicode=True, sort_keys=False)


def get_pois(config_path):
    """Read POI list from config file."""
    config = load_config(config_path)
    return config.get('poi', [])


def get_routes(config_path):
    """Read route list from config file."""
    config = load_config(config_path)
    return config.get('route', [])


def save_custom_tags(config_path, custom_tags):
    """Replace only the 'custom_tags' section in config_path, preserving other sections.

    Args:
        config_path: Path to mapoi_config.yaml
        custom_tags: List of custom tag dicts [{'name': str, 'description': str}, ...]
    """
    config = load_config(config_path)
    config['custom_tags'] = custom_tags
    with open(config_path, 'w') as f:
        yaml.dump(config, f, default_flow_style=None, allow_unicode=True, sort_keys=False)
