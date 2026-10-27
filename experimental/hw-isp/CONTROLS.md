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

## As standard V4L2 controls on the loopback node (what B2 asks for)
`/dev/video9` now carries the two standard camera controls, so any V4L2 client can drive the lens with no vendor ioctl and no helper:

```sh
v4l2-ctl -d /dev/video9 --set-ctrl focus_automatic_continuous=0    # hold the lens where it is
v4l2-ctl -d /dev/video9 --set-ctrl focus_absolute=700              # park it at 7.00 dioptre
v4l2-ctl -d /dev/video9 --set-ctrl focus_automatic_continuous=1    # autofocus back in charge
```
OpenCV: `cap.set(cv2.CAP_PROP_AUTOFOCUS, 0); cap.set(cv2.CAP_PROP_FOCUS, 900)`.

* `focus_absolute` is in **hundredths of a dioptre**, 0..1200 (0..12 dpt). Reading it gives the lens position (the feeder writes the autofocus result back into it).
* `focus_automatic_continuous` 1 = the autofocus watcher is in charge (default), 0 = lens held. Setting it to 0 and then a value, back to back as OpenCV does, is handled.
* How: `kernel/v4l2loopback-focus.patch` (v4l2loopback 0.12.7, 11 lines) registers the two controls and only stores them; `a7zcap` reads them back on its own output fd every third frame
  (`lbctl_poll`) and does what the control file would. With an unpatched loopback the feeder logs once and stops polling, and the control file keeps working.
* Install: apply the patch, `make -C /lib/modules/$(uname -r)/build M=$PWD modules`, stop the service, `rmmod v4l2loopback`, copy the `.ko` over
  `/lib/modules/$(uname -r)/extra/v4l2loopback.ko`, `depmod -a`, start the service.

Measured on the board as `radxa` (video group), service running: `focus_absolute` 300 / 700 / 1100 put the lens at 3.00 / 7.00 / 11.00 dpt (from the per-frame metadata) and
the value reads back; setting 300 twice gave the same sharpness (1.29, 1.26); OpenCV `CAP_PROP_FOCUS` 500 / 900 -> 5.00 / 9.00 dpt; autofocus resumes after `focus_automatic_continuous=1`.
Frame rate stayed at 30 fps with 0 dropped frames.

## What is still not standard
The kernel side is unchanged: the `ak7375_act` subdev still has no controls, and the `focus_absolute` on the vin nodes (`/dev/video0`) is inert. The control lives on the loopback node,
which is the one applications use. The older command file below still works.

## The command file
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

**B2 is now answered at the loopback node (above); the kernel driver itself is untouched.**
