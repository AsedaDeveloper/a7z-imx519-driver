"""Tests for the rpicam-* command translation (no camera needed): python3 -m unittest discover -s tests -v"""
import os, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import rpicam as r  # noqa: E402


class Translate(unittest.TestCase):
    def t(self, prog, *argv):
        return r.translate(prog, list(argv))

    def test_still_basic(self):
        args, w = self.t("rpicam-still", "-o", "a.jpg", "--width", "1920", "--height", "1080", "--shutter", "20000", "--gain", "2")
        self.assertEqual(args[:2], ["still", "a.jpg"])
        self.assertEqual(args[args.index("--size") + 1], "1080p")
        self.assertAlmostEqual(float(args[args.index("--exposure") + 1]), 0.02)
        self.assertEqual(float(args[args.index("--gain") + 1]), 2.0)
        self.assertEqual(w, [])

    def test_legacy_name_and_nonmode_size_warns(self):
        args, w = self.t("libcamera-still", "-o", "a.jpg", "--width", "4056", "--height", "3040")
        self.assertEqual(args[args.index("--size") + 1], "full")
        self.assertTrue(any("not a sensor mode" in x for x in w))

    def test_default_still_is_full_resolution(self):
        args, _ = self.t("rpicam-still", "-o", "a.jpg")
        self.assertEqual(args[args.index("--size") + 1], "full")

    def test_autofocus(self):
        args, _ = self.t("rpicam-still", "-o", "a.jpg", "--autofocus-mode", "auto")
        self.assertIn("--af", args)
        args, _ = self.t("rpicam-still", "-o", "a.jpg", "--autofocus-on-capture")
        self.assertIn("--af", args)
        args, _ = self.t("rpicam-still", "-o", "a.jpg")
        self.assertNotIn("--af", args)

    def test_lens_position(self):
        args, w = self.t("rpicam-still", "-o", "a.jpg", "--lens-position", "0")
        self.assertEqual(args[args.index("--focus") + 1], "0")
        args, w = self.t("rpicam-still", "-o", "a.jpg", "--lens-position", "3.5")
        self.assertNotIn("--focus", args)
        self.assertTrue(any("dioptres" in x for x in w))

    def test_flip_rotation_raw(self):
        args, _ = self.t("rpicam-still", "-o", "a.jpg", "--hflip", "--rotation", "180", "-r")
        self.assertIn("--hflip", args)
        self.assertEqual(args[args.index("--rotate") + 1], "180")
        self.assertEqual(args[args.index("--raw") + 1], "a.dng")

    def test_vid(self):
        args, w = self.t("rpicam-vid", "-o", "v.mp4", "-t", "10000", "--framerate", "25", "--bitrate", "6000000")
        self.assertEqual(args[:2], ["video", "v.mp4"])
        self.assertEqual(float(args[args.index("--seconds") + 1]), 10.0)
        self.assertEqual(args[args.index("--fps") + 1], "25")
        self.assertEqual(args[args.index("--bitrate") + 1], "6000000")
        self.assertEqual(args[args.index("--size") + 1], "1080p")

    def test_unknown_options_are_reported_not_fatal(self):
        args, w = self.t("rpicam-still", "-o", "a.jpg", "--tuning-file", "x.json", "--awb", "daylight")
        self.assertIsNotNone(args)
        self.assertTrue(any("ignored unsupported options" in x for x in w))

    def test_list_cameras(self):
        args, _ = self.t("rpicam-hello", "--list-cameras")
        self.assertIsNone(args)

    def test_pick_size_exact_and_nearest(self):
        w = []
        self.assertEqual(r.pick_size(1280, 720, "full", w), "720p")
        self.assertEqual(w, [])
        self.assertEqual(r.pick_size(1600, 900, "full", w), "1080p")   # next larger mode
        self.assertEqual(r.pick_size(0, 0, "1080p", []), "1080p")


if __name__ == "__main__":
    unittest.main()
