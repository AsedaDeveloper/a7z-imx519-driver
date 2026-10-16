"""Unit tests for the calibration code in tools/a7zcam.py (no camera needed).

    python3 -m unittest discover -s tests -v        (needs numpy + opencv)

They use synthetic Bayer frames, so they check the maths, not the hardware.
"""
import os, sys, unittest
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import a7zcam as z  # noqa: E402

H, W = 240, 320
RNG = np.random.default_rng(1)


def flat_raw(level, noise=1.0):
    """RGGB mosaic with a uniform signal (counts above the black level) plus Gaussian noise."""
    a = z.BLACK + level + RNG.normal(0, noise, (H, W))
    return np.clip(a, 0, z.FULL).astype(np.uint16)


def vignetted_raw(level=500.0, edge=0.4, noise=1.0):
    """Radial falloff to `edge` of the centre value at the corners, same for all colours."""
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    r = np.hypot(y - H / 2, x - W / 2) / np.hypot(H / 2, W / 2)
    shade = 1.0 - (1.0 - edge) * r ** 2
    a = z.BLACK + level * shade + RNG.normal(0, noise, (H, W))
    return np.clip(a, 0, z.FULL).astype(np.uint16), shade


class DarkTests(unittest.TestCase):
    def test_hot_pixels_found_and_no_false_alarms(self):
        frames = []
        hot_xy = [(10, 20), (101, 55), (200, 300), (33, 7)]
        for _ in range(8):
            f = flat_raw(2.0, noise=0.7).astype(np.int32)
            for y, x in hot_xy:
                f[y, x] += 60
            frames.append(np.clip(f, 0, z.FULL).astype(np.uint16))
        dark, hot = z.dark_build(frames)
        self.assertEqual(int(hot.sum()), len(hot_xy))
        for y, x in hot_xy:
            self.assertTrue(hot[y, x])

    def test_clean_dark_has_no_hot_pixels(self):
        dark, hot = z.dark_build([flat_raw(2.0, noise=0.7) for _ in range(8)])
        self.assertEqual(int(hot.sum()), 0)

    def test_dark_apply_removes_pattern_and_keeps_black_level(self):
        pattern = np.zeros((H, W), np.float32)
        pattern[:, ::2] = 5.0                      # fixed-pattern columns
        dark = z.BLACK + 3.0 + pattern
        scene = np.clip(z.BLACK + 100.0 + pattern + 3.0, 0, z.FULL).astype(np.uint16)
        out = z.dark_apply(scene, dark, None).astype(np.float32)
        self.assertAlmostEqual(float(out.mean()), z.BLACK + 100.0 + 0.0, delta=1.5)
        self.assertLess(float(np.abs(out[:, ::2].mean() - out[:, 1::2].mean())), 0.5)

    def test_hot_pixel_replacement(self):
        a = flat_raw(80.0, noise=0.0)
        a[100, 100] = 900
        hot = np.zeros((H, W), bool)
        hot[100, 100] = True
        out = z.dark_apply(a, np.full((H, W), z.BLACK, np.float32), hot)
        self.assertAlmostEqual(float(out[100, 100]), z.BLACK + 80.0, delta=2)

    def test_output_never_below_zero_or_above_full_scale(self):
        a = np.full((H, W), 1023, np.uint16)
        out = z.dark_apply(a, np.full((H, W), 0, np.float32), None)
        self.assertLessEqual(int(out.max()), z.FULL)
        out = z.dark_apply(np.zeros((H, W), np.uint16), np.full((H, W), 500, np.float32), None)
        self.assertGreaterEqual(int(out.min()), 0)


class LensShadingTests(unittest.TestCase):
    def test_flat_field_gives_unity_gain(self):
        lsc = z.lsc_calibrate([flat_raw(500.0) for _ in range(4)])
        for k in ("r", "g", "b"):
            g = np.array(lsc[k])
            self.assertLess(abs(float(g.mean()) - 1.0), 0.03)

    def test_vignette_is_flattened(self):
        raw, shade = vignetted_raw(edge=0.4)
        lsc = z.lsc_calibrate([raw])
        # corner gain should be about 1/0.4 = 2.5 (clipped to the 6x limit, smoothed)
        g = np.array(lsc["g"])
        self.assertGreater(float(g[0, 0]), 1.8)
        self.assertLess(float(g[0, 0]), 3.2)
        self.assertLess(abs(float(g[g.shape[0] // 2, g.shape[1] // 2]) - 1.0), 0.05)
        out = z.lsc_apply(raw, lsc).astype(np.float32) - z.BLACK
        c = out[H // 2 - 8:H // 2 + 8, W // 2 - 8:W // 2 + 8].mean()
        corner = out[:12, :12].mean()
        before = (raw.astype(np.float32) - z.BLACK)[:12, :12].mean() / (raw.astype(np.float32) - z.BLACK)[H // 2 - 8:H // 2 + 8, W // 2 - 8:W // 2 + 8].mean()
        self.assertLess(before, 0.55)
        self.assertGreater(corner / c, 0.9)

    def test_gain_never_below_one(self):
        lsc = z.lsc_calibrate([vignetted_raw()[0]])
        for k in ("r", "g", "b"):
            self.assertGreaterEqual(float(np.min(lsc[k])), 0.99)

    def test_apply_preserves_shape_dtype_and_black_level(self):
        lsc = z.lsc_calibrate([flat_raw(300.0)])
        out = z.lsc_apply(np.full((H, W), z.BLACK, np.uint16), lsc)
        self.assertEqual(out.shape, (H, W))
        self.assertEqual(out.dtype, np.uint16)
        self.assertLessEqual(abs(float(out.mean()) - z.BLACK), 0.5)


class BayerTests(unittest.TestCase):
    def test_split_planes_are_rggb(self):
        a = np.zeros((4, 4), np.uint16)
        a[0::2, 0::2] = 1; a[0::2, 1::2] = 2; a[1::2, 0::2] = 3; a[1::2, 1::2] = 4
        r, g1, g2, b = z._split(a)
        self.assertTrue((r == 1).all() and (g1 == 2).all() and (g2 == 3).all() and (b == 4).all())


if __name__ == "__main__":
    unittest.main()
