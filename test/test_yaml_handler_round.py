"""Unit test for yaml_handler._round_poi_floats (#242).

frontend (web/js/poi-editor.readForm + web/js/poi-filter.roundTo) で丸めた値が
backend に届く前提だが、yaml 直編集や非 WebUI クライアントからの POST 経路でも
yaml の値展開を抑えるため backend 側でも同じ桁数で丸める。本 test はその桁数
契約 (frontend 側 POSE_XY_DIGITS=3 / POSE_YAW_DIGITS=4 等) を pin する。
"""
import math
import os
import tempfile
import unittest

import yaml

from mapoi_webui.yaml_handler import _round_poi_floats, save_pois


class TestRoundPoiFloats(unittest.TestCase):

    def test_pose_xy_rounded_to_three_digits(self):
        pois = [{
            'name': 'p',
            'pose': {'x': 0.10000038482226709, 'y': 1.2345678, 'yaw': 0.0},
            'tolerance': {'xy': 0.5, 'yaw': 0.785},
        }]
        _round_poi_floats(pois)
        self.assertEqual(pois[0]['pose']['x'], 0.1)
        self.assertEqual(pois[0]['pose']['y'], 1.235)

    def test_pose_yaw_rounded_to_four_digits(self):
        pois = [{
            'name': 'p',
            'pose': {'x': 0.0, 'y': 0.0, 'yaw': 3.1399991679827224},
            'tolerance': {'xy': 0.5, 'yaw': 0.785},
        }]
        _round_poi_floats(pois)
        self.assertEqual(pois[0]['pose']['yaw'], 3.14)

    def test_tolerance_yaw_rounded_to_four_digits(self):
        pois = [{
            'name': 'p',
            'pose': {'x': 0.0, 'y': 0.0, 'yaw': 0.0},
            'tolerance': {'xy': 0.50001, 'yaw': 0.7850002283279937},
        }]
        _round_poi_floats(pois)
        self.assertEqual(pois[0]['tolerance']['xy'], 0.5)
        self.assertEqual(pois[0]['tolerance']['yaw'], 0.785)

    def test_non_finite_values_preserved(self):
        """NaN / Infinity / bool / 文字列は丸めず透過する (validator が別途 reject する想定)."""
        pois = [{
            'name': 'p',
            'pose': {'x': math.nan, 'y': math.inf, 'yaw': 'oops'},
            'tolerance': {'xy': True, 'yaw': 0.785},
        }]
        _round_poi_floats(pois)
        self.assertTrue(math.isnan(pois[0]['pose']['x']))
        self.assertTrue(math.isinf(pois[0]['pose']['y']))
        self.assertEqual(pois[0]['pose']['yaw'], 'oops')
        self.assertIs(pois[0]['tolerance']['xy'], True)

    def test_missing_pose_or_tolerance_keys_safe(self):
        """pose / tolerance がそもそも dict でない POI は素通り (validator が別途 reject)."""
        # 1.23456 を使う: 1.2345 (= 半端の境界値) は JS Math.round (half-up) と
        # Python round (banker's rounding) で結果が分かれるため避ける。frontend で
        # 既に丸めた値を再丸めする想定なので、実運用では半端境界は来ない。
        pois = [
            {'name': 'no_pose'},
            {'name': 'partial', 'pose': {'x': 1.23456}},
            {'name': 'broken', 'pose': 'string', 'tolerance': None},
        ]
        _round_poi_floats(pois)
        self.assertEqual(pois[1]['pose']['x'], 1.235)

    def test_non_list_pois_no_raise(self):
        _round_poi_floats(None)
        _round_poi_floats({'not': 'a list'})

    def test_save_pois_writes_rounded_yaml(self):
        """End-to-end: save_pois → yaml.dump 出力 → re-load で桁数が縮まっていること."""
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'mapoi_config.yaml')
            with open(path, 'w') as f:
                yaml.safe_dump({'map': {}, 'poi': [], 'route': []}, f)
            save_pois(path, [{
                'name': 'p',
                'pose': {'x': 0.10000038482226709, 'y': 2.0, 'yaw': 3.1399991679827224},
                'tolerance': {'xy': 0.5, 'yaw': 0.7850002283279937},
                'tags': ['waypoint'],
            }])
            with open(path) as f:
                reloaded = yaml.safe_load(f)
            poi = reloaded['poi'][0]
            self.assertEqual(poi['pose']['x'], 0.1)
            self.assertEqual(poi['pose']['yaw'], 3.14)
            self.assertEqual(poi['tolerance']['yaw'], 0.785)


if __name__ == '__main__':
    unittest.main()
