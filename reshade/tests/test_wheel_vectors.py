import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image


SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from analyze_wheel_vectors import compare_sequences, decode_motion, sample_regions


class WheelVectorTests(unittest.TestCase):
    def test_packed_motion_formats_keep_rows_and_first_two_channels(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'Motion.bin'
            expected = np.array([[[-1, 0.5], [3, -4]], [[-2, 1], [0, 0.125]]], dtype=np.float32)
            for dxgi, dtype, channels in ((2, '<f4', 4), (10, '<f2', 4), (16, '<f4', 2), (34, '<f2', 2)):
                with self.subTest(dxgi=dxgi):
                    data = np.full((2, 2, channels), np.nan, dtype=dtype)
                    data[..., :2] = expected
                    data.tofile(path)
                    np.testing.assert_array_equal(decode_motion(path, 2, 2, dxgi), expected)
                    with self.assertRaisesRegex(ValueError, 'expected'):
                        decode_motion(path, 2, 3, dxgi)

            np.array([[[-32768, 32767], [-32767, 0]], [[16384, -16384], [1, -1]]], dtype='<i2').tofile(path)
            np.testing.assert_allclose(decode_motion(path, 2, 2, 37),
                                       [[[-1, 1], [-1, 0]], [[16384 / 32767, -16384 / 32767],
                                                            [1 / 32767, -1 / 32767]]])

    def test_rigid_translation_and_projected_circulation(self):
        x, y = np.arange(48) + 0.5, np.arange(32) + 0.5
        cx, cy, rx, ry = wheel = 16, 16, 13, 8
        reference, annulus = (33, 4, 12, 12), (0.25, 0.9)
        nx, ny = np.meshgrid((x - cx) / rx, (y - cy) / ry)
        tangent = np.stack((-rx * ny, ry * nx), axis=-1)
        inside = np.hypot(nx, ny) <= 1
        baseline = np.array([4.25, -3.5])
        valid = np.ones_like(inside)
        for coefficient in (0, 0.2, -0.3):
            with self.subTest(coefficient=coefficient):
                offsets = np.where(inside[..., None], coefficient * tangent, 0)
                vectors = baseline + offsets
                report, arrays, reference_vector = sample_regions(
                    vectors, vectors, valid, x, y, wheel, reference, annulus,
                )
                np.testing.assert_array_equal(reference_vector, baseline)
                ring = report['wheel']['annulus']
                self.assertAlmostEqual(ring['circulation_fit']['coefficient'], coefficient)
                self.assertAlmostEqual(ring['circulation_fit']['circulation_rmse_output_pixels'], 0)
                np.testing.assert_allclose(arrays['wheel_annulus_radial_output_pixels'], 0, atol=1e-12)
                for sector in ring['angular_sectors']:
                    self.assertGreater(sector['valid_texels'], 0)
                    self.assertEqual(np.sign(sector['mean_tangential_output_pixels']), np.sign(coefficient))
                if coefficient == 0:
                    self.assertEqual(ring['residual_output_pixel_xy']['nonzero'], 0)
                    self.assertGreater(report['wheel']['output_pixel_xy']['nonzero'], 0)

    def test_duplicate_submission_interval_excludes_comparison(self):
        first = {'capture': '/tmp/NativeWheel-123-42-10-01.json', 'metadata_issues': [], 'regions': {}}
        second = {'capture': '/tmp/NativeWheel-123-42-10-02.json', 'metadata_issues': [], 'regions': {}}
        first['metadata'] = {'context_id': 1, 'sequence': 6, 'dispatch_id': 8, 'submitted_present_interval': 20}
        second['metadata'] = {'context_id': 1, 'sequence': 7, 'dispatch_id': 9, 'submitted_present_interval': 20}
        self.assertEqual(compare_sequences([first, second]), [])
        for capture in (first, second):
            self.assertIn('Duplicate submitted_present_interval within this capture group.', capture['metadata_issues'])

    def test_cli_scales_aligns_and_flags_invalid_vectors(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            aliases = ['NativeWheel-123-42-10-01', 'NativeWheel-123-42-10-02', 'NativeWheel-123-42-10-03']
            width, height, motion_width, motion_height = 16, 12, 9, 7
            for i, stem in enumerate(aliases):
                scene = np.full((height, width, 4), (50, 100, 200, 255), dtype=np.uint8)
                for side in ('Before', 'After'):
                    scene.tofile(directory / f'{stem}-Scene{side}.bin')
                motion = np.full((motion_height, motion_width, 2), (1, -1), dtype='<f4')
                motion[0, 0] = (np.nan, 0)
                motion[0, 1] = (200, 0)
                motion[-1, -1] = (np.inf, 0)
                motion.tofile(directory / f'{stem}-Motion.bin')
                metadata = {
                    'width': width, 'height': height, 'dxgi_format': 28, 'sequence': i + 1,
                    'submissions': 2 if i == 2 else 1, 'dispatch_id': 10 + i, 'context_id': 2,
                    'source_same_dispatch': True, 'recording_replaced': True,
                    'motion_width': motion_width, 'motion_height': motion_height, 'motion_dxgi_format': 16,
                    'render_width': 8, 'render_height': 6, 'upscale_width': width, 'upscale_height': height,
                    'display_resolution_motion_vectors': False, 'jitter_cancellation': False,
                    'reset': False, 'scale_x': 2, 'scale_y': -3,
                    'frame_time_ms': 16.7, 'jitter_x': 0.01, 'jitter_y': 0.02,
                    'recorded_present_interval': 15 + i, 'submitted_present_interval': 15 + i,
                    'captured_tick_ms': 1700 + 16 * i,
                }
                (directory / f'{stem}.json').write_text(json.dumps(metadata), encoding='utf-8')
            command = [sys.executable, str(SCRIPTS / 'analyze_wheel_vectors.py'), str(directory)]
            subprocess.run(command, check=True, capture_output=True)
            report_path = directory / 'wheel-vector-analysis.json'
            report = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(report['captures'][0]['regions'], {})
            self.assertNotIn('circulation_coefficients', report['comparisons'][0])

            subprocess.run([*command, '--wheel', '7', '6', '5', '4', '--reference', '12', '2', '3', '7'],
                           check=True, capture_output=True)
            report = json.loads(report_path.read_text(encoding='utf-8'))
            self.assertEqual(len(report['captures']), 3)
            self.assertEqual(len(report['comparisons']), 1)
            comparison = report['comparisons'][0]
            self.assertTrue(comparison['adjacent_dispatches_and_sequences'])
            self.assertTrue(comparison['adjacent_submission_intervals'])
            self.assertEqual(comparison['submitted_present_interval_gap'], 1)
            self.assertEqual(comparison['circulation_coefficients'], [0, 0])
            self.assertIn('replayed', report['captures'][2]['metadata_issues'][0])
            capture = report['captures'][0]
            counts = capture['active_motion']
            self.assertEqual(counts['nonfinite_raw_texels'], 1)
            self.assertEqual(counts['finite_outside_shader_uv_limit_texels'], 1)
            self.assertEqual(counts['nonfinite_padding_texels'], 1)
            self.assertEqual(counts['valid_texels'], 46)
            with np.load(directory / capture['artifacts']['numeric_vectors']) as vectors:
                self.assertEqual(vectors['raw_rg'].shape, (7, 9, 2))
                self.assertEqual(vectors['output_pixel_xy'].shape, (6, 8, 2))
                np.testing.assert_array_equal(vectors['output_pixel_xy'][1, 0], (4, 6))
            with Image.open(directory / capture['artifacts']['motion_hue']) as hue:
                self.assertEqual(hue.size, (width, height))
                self.assertEqual(hue.getpixel((1, 1)), (255, 0, 255))
                self.assertEqual(hue.getpixel((2, 1)), (255, 0, 255))
                self.assertNotEqual(hue.getpixel((4, 1)), (255, 0, 255))
            self.assertTrue((directory / capture['artifacts']['wheel_quiver']).is_file())


if __name__ == '__main__':
    unittest.main()
