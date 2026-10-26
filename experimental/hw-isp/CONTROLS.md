# Driving the lens (checklist B2)

```sh
a7zfocus 7.2      # park the lens at 7.2 dioptre; autofocus holds off
a7zfocus af       # one autofocus now, then hold
a7zfocus auto     # hand control back to the autofocus watcher
```

Measured from OpenCV while reading `/dev/video9`, sharpness of the same scene:

| command | sharpness |
|---|---|
| `a7zfocus 3.0` | 3.7 |
| `a7zfocus 7.0` | 8.3 |
| `a7zfocus 11.0` | 6.2 |
| `a7zfocus af` | 6.0 (settled by itself) |

## Why this is not a V4L2 control, which is what B2 actually asks for
Two things are broken in the vendor stack, and both were measured rather than assumed:

* **The actuator exposes no controls.** `ak7375_act` is a proper V4L2 lens subdev (`/dev/v4l-subdev1` in the media graph), but
  `v4l2-ctl -d /dev/v4l-subdev1 --list-ctrls` returns an empty list. There is no `V4L2_CID_FOCUS_ABSOLUTE` to set. The lens only moves through the
  vendor ioctl `VIDIOC_VIN_ACT_SET_CODE`.
* **The `focus_absolute` that does exist is inert.** The vin video nodes advertise `focus_absolute` (0x009a090a, range 0..127). Writing it is accepted
  and does reach libisp — its log prints `name is V4L2_CID_FOCUS_ABSOLUTE, value is 0x0` — but the lens never moves: stepping 0, 32, 64, 96, 127 across a
  300-frame capture left sharpness flat at 2.4-2.5 the whole way.

On top of that, applications read frames from the v4l2loopback node, and v4l2loopback does not forward controls to the producer, so even a working control
on `/dev/video0` would not reach an app through `/dev/video9`.

So focus goes through a one-line command file instead (`A7Z_CTL`, default `/run/a7zcam.ctl`), which `a7zcap` reads once per frame. `a7zfocus` is a three-line
wrapper around it, and anything that can write a file can drive the lens.

**B2 stays open.** A real `FOCUS_ABSOLUTE` needs the kernel's `ak7375_act` driver to register the control, and the loopback to proxy it — neither is done here.
