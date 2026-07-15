"""turtlebot3_world demo config の幾何 / 不変量を pin する (#236 medium #3 / #5).

mock-based launch_test (mapoi_server/test/test_poi_event_route_integration.py,
#236 medium #4 / #5) は bridge の *振る舞い* を pin するが、demo の *データ*
(座標・半径・tag) の正しさは別途必要。PR #235 (#230) で demo config を機能カタログ
構成へ刷新した際、正しさの根拠が手動 Docker 確認に偏り、座標・半径の微調整で demo が
静かに壊れる回帰が温床になっていた (Cursor review round 2 medium #3 / #5)。本 test は
実 demo config を読み、その回帰を防ぐ。

limitation (medium #3): landmark reachability は route waypoint を結ぶ *直線* polyline
への最小距離で近似する。実際の Nav2 経路は障害物回避で曲線を取りうるため、本 test が
通っても実機で landmark 半径を踏み逃す可能性は残る (= 「直線でも届かない」を弾く下限
guard であり、「実経路で必ず踏む」保証ではない)。実経路での踏破確認は sim / 実機の
手動 demo に委ねる。

yaw 厳密着地 (medium #5 後半) も Nav2 controller の実挙動が要るため、本 test では
controller 設定値 (goal.tolerance.yaw) が厳密値のまま保たれていることだけを pin する。
"""

import math
from pathlib import Path

import yaml

# 実 demo config (source of truth)。ament_add_pytest_test は source の test file を
# その場で実行するため、__file__ から package root 相対で辿れる。
CONFIG_PATH = (
    Path(__file__).resolve().parents[1] / "maps" / "turtlebot3_world" / "mapoi_config.yaml"
)

# waypoint tolerance.xy の下限。本 demo は waypoint_arrival_mode=mapoi 既定 (#243) だが、
# Nav2 の xy_goal_tolerance (0.25) より小さいと robot が radius 内に入る前に Nav2 が停止し、
# ENTER / pause の EVENT_PAUSED を取りこぼす。安全側に Nav2 tol + 余裕の 0.30 を下限とする
# (mapoi モードでも進行自体は OR で保証されるが、event 発火には radius 進入が必要)。
XY_TOLERANCE_FLOOR = 0.30

# msg spec (#138): tolerance.yaw は 0 / 負値禁止、最小 0.001 rad。
YAW_TOLERANCE_FLOOR = 0.001

# goal POI の厳密 yaw 着地 (medium #5)。config は 0.10 rad を採用。
GOAL_YAW_TOLERANCE = 0.10


def _load_config():
    with open(CONFIG_PATH, encoding="utf-8") as f:
        return yaml.safe_load(f)


def _poi_by_name(cfg):
    return {p["name"]: p for p in cfg.get("poi", [])}


def _has_tag(poi, tag):
    return tag in (poi.get("tags") or [])


def _point_to_segment_distance(px, py, ax, ay, bx, by):
    """点 (px,py) と線分 A-B の最小距離。"""
    abx, aby = bx - ax, by - ay
    seg_len_sq = abx * abx + aby * aby
    if seg_len_sq == 0.0:
        # A == B の退化線分は点との距離。
        return math.hypot(px - ax, py - ay)
    # 線分上への射影パラメータ t を [0,1] にクランプ。
    t = ((px - ax) * abx + (py - ay) * aby) / seg_len_sq
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * abx, ay + t * aby
    return math.hypot(px - cx, py - cy)


def _point_to_polyline_distance(px, py, points):
    """点 (px,py) と折れ線 (points: [(x,y), ...]) の最小距離。"""
    if len(points) == 1:
        return math.hypot(px - points[0][0], py - points[0][1])
    best = float("inf")
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        best = min(best, _point_to_segment_distance(px, py, ax, ay, bx, by))
    return best


def test_config_file_exists():
    assert CONFIG_PATH.is_file(), f"demo config が見つからない: {CONFIG_PATH}"


def test_all_poi_xy_tolerance_above_floor():
    """全 POI の tolerance.xy が下限 (0.30) 以上 (#230 / #243)。

    mapoi モード既定でも ENTER / EVENT_PAUSED 発火には robot が radius 内に入る必要があり、
    Nav2 が停止する ~0.25 より小さいと取りこぼすため、Nav2 tol + 余裕の 0.30 を下限に保つ。
    """
    cfg = _load_config()
    for poi in cfg["poi"]:
        xy = poi["tolerance"]["xy"]
        assert xy >= XY_TOLERANCE_FLOOR - 1e-9, (
            f"POI '{poi['name']}' の tolerance.xy={xy} が下限 {XY_TOLERANCE_FLOOR} 未満。"
            f" Nav2 が POI 半径到達前に goal 判定で停止し radius event が出ない恐れ"
        )


def test_all_poi_yaw_tolerance_positive():
    """全 POI の tolerance.yaw が msg spec の下限 (0.001 rad) 以上 (#138)。"""
    cfg = _load_config()
    for poi in cfg["poi"]:
        yaw = poi["tolerance"]["yaw"]
        assert yaw >= YAW_TOLERANCE_FLOOR, (
            f"POI '{poi['name']}' の tolerance.yaw={yaw} が下限 {YAW_TOLERANCE_FLOOR} 未満"
        )


def test_goal_yaw_is_strict():
    """goal POI は厳密 yaw 着地 (0.10 rad) を維持する (medium #5)。"""
    pois = _poi_by_name(_load_config())
    assert "goal" in pois, "demo config に 'goal' POI が無い"
    yaw = pois["goal"]["tolerance"]["yaw"]
    # abs_tol は config 値の浮動小数表現誤差 (0.10000038…) を許容しつつ、0.0001 rad 超の
    # 意図しないドリフトは弾く厳しさ ("厳密 yaw" の意図に合わせる)。
    assert math.isclose(yaw, GOAL_YAW_TOLERANCE, rel_tol=0.0, abs_tol=1e-4), (
        f"goal の tolerance.yaw={yaw} が厳密値 {GOAL_YAW_TOLERANCE} から外れた"
    )


def test_landmark_tag_exclusive_with_waypoint_and_pause():
    """landmark tag は waypoint / pause と排他 (#143)。"""
    cfg = _load_config()
    for poi in cfg["poi"]:
        if _has_tag(poi, "landmark"):
            assert not _has_tag(poi, "waypoint"), (
                f"POI '{poi['name']}' が landmark と waypoint を併用 (#143 で排他)"
            )
            assert not _has_tag(poi, "pause"), (
                f"POI '{poi['name']}' が landmark と pause を併用 (#143 で排他)"
            )


def test_route_references_resolve():
    """全 route の waypoints / landmarks 参照が POI として実在し、tag 整合する。"""
    cfg = _load_config()
    pois = _poi_by_name(cfg)
    for route in cfg.get("route", []):
        name = route["name"]
        for wp in route.get("waypoints", []):
            assert wp in pois, f"route '{name}' の waypoint '{wp}' が POI に存在しない"
            assert _has_tag(pois[wp], "waypoint"), (
                f"route '{name}' の waypoint '{wp}' が waypoint tag を持たない"
            )
        for lm in route.get("landmarks") or []:
            assert lm in pois, f"route '{name}' の landmark '{lm}' が POI に存在しない"
            assert _has_tag(pois[lm], "landmark"), (
                f"route '{name}' の landmark '{lm}' が landmark tag を持たない"
            )


def test_listed_landmarks_reachable_along_route_path():
    """route.landmarks 列挙対象が、waypoint 直線経路の tolerance.xy 内に掛かる (medium #3).

    tour_full 最終区間など、landmark 半径への余裕が小さい区間で座標 / 半径を調整した際に
    landmark が経路から外れて踏み逃す回帰を検出する。直線近似の下限 guard (docstring 参照)。
    """
    cfg = _load_config()
    pois = _poi_by_name(cfg)
    checked = 0
    for route in cfg.get("route", []):
        landmarks = route.get("landmarks") or []
        if not landmarks:
            continue
        path_points = [
            (pois[wp]["pose"]["x"], pois[wp]["pose"]["y"])
            for wp in route["waypoints"]
        ]
        for lm in landmarks:
            lm_poi = pois[lm]
            lx, ly = lm_poi["pose"]["x"], lm_poi["pose"]["y"]
            tol = lm_poi["tolerance"]["xy"]
            dist = _point_to_polyline_distance(lx, ly, path_points)
            assert dist <= tol, (
                f"route '{route['name']}' の landmark '{lm}': waypoint 直線経路への"
                f"最小距離 {dist:.3f} が tolerance.xy {tol} を超過 (経路上で踏めない)"
            )
            checked += 1
    assert checked > 0, "landmarks を持つ route が 1 つも無い (test が空回り)"
