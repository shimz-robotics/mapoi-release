"""Unit test for yaml_handler.compute_config_version (#241).

WebUI Save 時の楽観的競合検出に使う content-derived version (sha256 hex)。
frontend が GET 時に受け取った version を POST 時に送り返し、backend は current
version と一致しなければ 409 を返す。本 test は version 計算の決定性 / 変化検出 /
ファイル不在時の None 返却 / save_pois 経由の version 遷移を pin する。
"""
import os
import tempfile
import unittest

import yaml

from mapoi_webui.yaml_handler import compute_config_version, save_pois


class TestComputeConfigVersion(unittest.TestCase):

    def test_returns_none_for_missing_file(self):
        """ファイル不在 → None。caller 側で 'version check 不可' として扱う."""
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(compute_config_version(os.path.join(tmp, 'absent.yaml')))

    def test_same_content_same_version(self):
        """同一内容なら version は同じ (sha256 deterministic, hex 64 文字)."""
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'a.yaml')
            with open(p, 'w') as f:
                f.write('poi: []\nroute: []\n')
            v1 = compute_config_version(p)
            v2 = compute_config_version(p)
            self.assertEqual(v1, v2)
            self.assertEqual(len(v1), 64)

    def test_different_content_different_version(self):
        """内容が違えば version は変わる → 競合検出が成立する."""
        with tempfile.TemporaryDirectory() as tmp:
            p1 = os.path.join(tmp, 'a.yaml')
            p2 = os.path.join(tmp, 'b.yaml')
            with open(p1, 'w') as f:
                f.write('poi: []\n')
            with open(p2, 'w') as f:
                f.write('poi: [{name: x}]\n')
            self.assertNotEqual(compute_config_version(p1), compute_config_version(p2))

    def test_version_changes_after_save_pois(self):
        """save_pois 後は version が変わる → frontend が新 version を pin する経路."""
        with tempfile.TemporaryDirectory() as tmp:
            p = os.path.join(tmp, 'cfg.yaml')
            with open(p, 'w') as f:
                yaml.safe_dump({'map': {}, 'poi': [], 'route': []}, f)
            v_before = compute_config_version(p)
            save_pois(p, [{
                'name': 'p1',
                'pose': {'x': 0.1, 'y': 0.2, 'yaw': 0.0},
                'tolerance': {'xy': 0.5, 'yaw': 0.785},
                'tags': ['waypoint'],
            }])
            v_after = compute_config_version(p)
            self.assertNotEqual(v_before, v_after)


if __name__ == '__main__':
    unittest.main()
