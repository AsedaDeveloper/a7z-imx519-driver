#!/usr/bin/env python3
"""Raspberry-Pi-style command line (rpicam-still / rpicam-vid / rpicam-hello, also the older libcamera-* names)
on top of a7zcam, so existing Pi scripts keep working. It is NOT libcamera: it understands the common options and
tells you about the ones it cannot honour.

    rpicam-still -o photo.jpg --width 1920 --height 1080 --shutter 20000 --gain 2 --autofocus-mode auto
    rpicam-vid   -o clip.mp4  -t 10000 --width 1920 --height 1080 --framerate 30 --bitrate 8000000
    rpicam-hello --list-cameras
"""
import os, sys, argparse, subprocess, tempfile

HERE = os.path.dirname(os.path.realpath(__file__))
# name: (width, height); same table as a7zcam.MODES, repeated so this file can be tested without the camera libraries
SIZES = {"720p": (1280, 720), "1080p": (1920, 1080), "2k": (2328, 1748), "4k": (3840, 2160), "full": (4656, 3496)}


def pick_size(width, height, default, warnings):
    """Closest sensor mode for a requested size. The sensor modes are fixed (no scaler), so an exact match is needed
    for the picture to have the size asked for."""
    if not width and not height:
        return default
    w = width or 0
    h = height or 0
    for name, (mw, mh) in SIZES.items():
        if (mw, mh) == (w, h):
            return name
    # nearest by total pixel count, but never smaller than the request when a larger mode exists
    want = (w or h * 4 // 3) * (h or w * 3 // 4)
    bigger = [(n, s) for n, s in SIZES.items() if s[0] >= w and s[1] >= h]
    pool = bigger or list(SIZES.items())
    name = min(pool, key=lambda kv: abs(kv[1][0] * kv[1][1] - want))[0]
    warnings.append(f"size {w}x{h} is not a sensor mode; using {name} ({SIZES[name][0]}x{SIZES[name][1]}). "
                    f"Modes: " + ", ".join(f"{n}={s[0]}x{s[1]}" for n, s in SIZES.items()))
    return name


def _common(p):
    p.add_argument("-o", "--output")
    p.add_argument("-t", "--timeout", type=int, default=None, help="ms")
    p.add_argument("--width", type=int, default=0)
    p.add_argument("--height", type=int, default=0)
    p.add_argument("--shutter", type=float, help="exposure time in microseconds")
    p.add_argument("--gain", "--analoggain", type=float)
    p.add_argument("--hflip", action="store_true")
    p.add_argument("--vflip", action="store_true")
    p.add_argument("--rotation", type=int, default=0, choices=[0, 90, 180, 270])
    p.add_argument("--autofocus-mode", choices=["default", "manual", "auto", "continuous"], default="default")
    p.add_argument("--autofocus-on-capture", action="store_true")
    p.add_argument("--lens-position", type=float)
    p.add_argument("-n", "--nopreview", action="store_true")
    p.add_argument("-v", "--verbose", nargs="?", const=1, type=int)
    p.add_argument("--list-cameras", action="store_true")


def translate(prog, argv):
    """Returns (a7zcam argument list or None, warnings). None means: nothing to run (list-cameras handled)."""
    warnings = []
    name = prog.replace("libcamera-", "rpicam-")
    p = argparse.ArgumentParser(prog=name, add_help=True)
    _common(p)
    p.add_argument("-r", "--raw", action="store_true")
    p.add_argument("--framerate", type=float, default=30.0)
    p.add_argument("--bitrate", type=int, default=0)
    p.add_argument("--codec", default="h264")
    p.add_argument("--quality", type=int)
    a, unknown = p.parse_known_args(argv)
    if unknown:
        warnings.append("ignored unsupported options: " + " ".join(unknown))
    if a.list_cameras:
        return None, warnings
    flip = []
    rot = a.rotation
    if a.hflip: flip.append("--hflip")
    if a.vflip: flip.append("--vflip")
    af = a.autofocus_mode in ("auto", "continuous") or a.autofocus_on_capture
    if a.autofocus_mode == "continuous":
        warnings.append("continuous autofocus is only available in 'a7zcam live'; focusing once instead")
    focus = []
    if a.lens_position is not None:
        if a.lens_position == 0:
            focus = ["--focus", "0"]
        else:
            warnings.append("--lens-position in dioptres is not calibrated for this lens motor; use 'a7zcam focus --set CODE' (0..4095)")
    if name == "rpicam-still":
        if not a.output:
            raise SystemExit("rpicam-still: -o OUTPUT is required")
        size = pick_size(a.width, a.height, "full", warnings)
        args = ["still", a.output, "--size", size] + flip + ["--rotate", str(rot)] + focus
        if a.shutter: args += ["--exposure", str(a.shutter / 1e6)]
        if a.gain: args += ["--gain", str(a.gain)]
        if af and not focus: args += ["--af"]
        if a.raw:
            args += ["--raw", os.path.splitext(a.output)[0] + ".dng"]
        return args, warnings
    if name == "rpicam-vid":
        if not a.output:
            raise SystemExit("rpicam-vid: -o OUTPUT is required")
        if a.codec != "h264":
            warnings.append(f"codec {a.codec} is not supported; recording h264")
        size = pick_size(a.width, a.height, "1080p", warnings)
        secs = (a.timeout if a.timeout is not None else 5000) / 1000.0
        if secs <= 0:
            secs = 3600.0
            warnings.append("-t 0 (run forever) is not supported; recording 1 hour")
        args = ["video", a.output, "--size", size, "--seconds", str(secs), "--fps", str(int(round(a.framerate)))] + flip + ["--rotate", str(rot)] + focus
        if a.bitrate: args += ["--bitrate", str(a.bitrate)]
        if af and not focus: args += ["--af"]
        return args, warnings
    if name == "rpicam-hello":
        return ["still", os.path.join(tempfile.gettempdir(), "rpicam-hello.jpg"), "--size", "720p"], warnings
    raise SystemExit(f"unknown command {prog}: use rpicam-still, rpicam-vid or rpicam-hello")


def list_cameras():
    print("Available cameras\n-----------------")
    print("0 : imx519 [4656x3496 10-bit RGGB] (/base/vin/imx519)")
    print("    Modes: 'SRGGB10' : " + " ".join(f"{w}x{h}" for w, h in SIZES.values()))
    print("    (Allwinner VIN, not libcamera; see docs/LIBCAMERA.md)")


def main():
    prog = os.path.basename(sys.argv[0])
    if prog.endswith(".py"):                       # python3 rpicam.py still -o x.jpg
        if len(sys.argv) < 2:
            raise SystemExit(__doc__)
        prog, args_in = "rpicam-" + sys.argv[1].replace("rpicam-", ""), sys.argv[2:]
    else:
        args_in = sys.argv[1:]
    args, warnings = translate(prog, args_in)
    for w in warnings:
        print("rpicam: " + w, file=sys.stderr)
    if args is None:
        list_cameras()
        return
    out = args[1]
    h264_raw = args[0] == "video" and out.lower().endswith((".h264", ".264"))
    tmp = None
    if h264_raw:                                   # a7zcam writes MP4; rpicam-vid users usually want a raw H.264 stream
        tmp = out + ".tmp.mp4"
        args[1] = tmp
    sys.argv = ["a7zcam"] + args
    sys.path.insert(0, HERE)
    import a7zcam
    a7zcam.main()
    if tmp:
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", tmp, "-c", "copy", "-bsf:v", "h264_mp4toannexb", "-f", "h264", out], check=True)
        os.remove(tmp)


if __name__ == "__main__":
    main()
