# A standard V4L2 device for the IMX519 (checklist B1)

`sudo v4l2node start` publishes the ISP's output — autofocused, white-balanced, exposure-capped — on `/dev/video9`, where any ordinary V4L2 application can open it.
`v4l2node stop` / `v4l2node status` do the obvious things. The script is `tools/v4l2node.sh`.

```
$ sudo v4l2node start
/dev/video9 serving 1920x1080 YU12 at 30 fps (ISP, autofocus, exposure cap)
```

## Why it is a loopback and not just /dev/video0
The obvious design is a daemon that runs libisp while applications open the camera node directly. **It does not work.** `userspace/ispd.c` does exactly that —
sets the sensor mode and format, calls `ispStart`, then idles — and `userspace/v4l2cap.c` is a plain V4L2 client issuing only standard calls, no vendor ioctls,
the same sequence ffmpeg or OpenCV would use. With the daemon running, that client gets:

```
S_FMT:    Device or resource busy
STREAMON: Operation not permitted
```

The vendor driver ties ISP operation to whichever process owns the stream, so libisp and the capture loop must live in the same process. A standard node
therefore has to be *fed*, not shared: `a7zcap` owns `/dev/video0` plus the ISP and republishes every frame into a v4l2loopback device (`A7Z_LOOPBACK=/dev/videoN`).
Frames are published from the autofocus loop too, so a refocus does not interrupt the node.

## Verified
A plain V4L2 client (`userspace/v4l2cap.c`, no vendor ioctls, no libisp) read **20 of 20 frames at 1920x1080** from `/dev/video9`, 3110400 bytes each, exactly
`1920*1080*3/2`. The decoded frame is a correct ISP picture: in focus, correctly white-balanced, full-range YUV.

The node presents itself as **single-planar YU12**, which matters: `/dev/video0` is multi-planar (`V4L2_CAP_VIDEO_CAPTURE_MPLANE`), and ffmpeg's and OpenCV's V4L2
backends only handle single-planar. The loopback removes that obstacle as well as the ISP one.

## Build and install (done on this board)
```sh
tar xzf v4l2loopback-0.12.7.tar.gz
make -C /lib/modules/$(uname -r)/build M=$PWD modules     # kernel headers are installed; builds clean
sudo cp v4l2loopback.ko /lib/modules/$(uname -r)/extra/ && sudo depmod -a
```
The module is third-party (github.com/umlaeute/v4l2loopback, v0.12.7) and is not vendored here.

## Not yet proven (checklist B19)
The acceptance test is ffmpeg, GStreamer, OpenCV and `v4l2-ctl` each reading this node. None of them can run here yet: the board has no network (wlan0 down,
only the USB gadget link), `ffmpeg`, `gstreamer` and `cv2` are not installed, and this `v4l2-ctl` build was compiled without streaming support
(`--stream-mmap` is not a recognised option). What is proven is that a standard single-planar V4L2 client reads the node correctly, which is the same code path
those applications use. Finishing B19 needs packages on the board — internet sharing over the USB link, or hand-copied .debs.
