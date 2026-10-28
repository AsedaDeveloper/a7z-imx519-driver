# NPU object detection (checklist B12)

The A733's NPU (VeriSilicon Vivante VIP9000 class, 3 TOPS INT8, `/dev/vipcore`, kernel driver "VIPLite driver version 2.0.3.0-AW-2024-05-29") **works on this board** with Allwinner's VIPLite v2.0 userspace.
Nothing from the vendor SDK is copied into this repo; this folder holds only our runner and decoder.

## What was installed on the board (Oct 28)
Source: `github.com/ZIFENG278/ai-sdk` (third-party mirror of Allwinner's ai-sdk, 1.1 GB, cloned on the Mac, only ~16 MB copied to the board under `~/npu/ai-sdk`):
`viplite-tina/lib/aarch64-none-linux-gnu/v2.0/{libNBGlinker.so,libVIPhal.so,inc}` (matches the 2.0.x kernel driver), `examples/{vpm_run,libawnn_viplite,libawutils,yolov5,lenet}`. The libraries are closed binaries from that repo.

## Checks
* `vpm_run` (built on the board, `make AI_SDK_PLATFORM=a733`) ran the bundled sample network 20 times: **2.8 ms per inference**, return 0, with the camera service running.
* YOLOv5s COCO (`examples/yolov5/model/v3/yolov5.nb`, 640x640 uint8 input): on the SDK's `dog.jpg` it found dog 0.87, car 0.54, bicycle 0.49 (the "car" is a truck in the photo). NPU time **47 ms** in `npu_run`.
* On a live frame from `/dev/video9` (camera aimed at a ceiling and a wall) it returned no detections, which is right; **a real object in front of the live camera has not been tried yet.** End to end including Python pre/post-processing: about 75-83 ms for the NPU call as timed from the runner.

## Build and run
The stock yolov5 example needs the OpenCV C++ headers, which the board does not have (and has no internet to install them), so the pre/post-processing is in Python and the C part is a 25-line runner:
```sh
S=ai-sdk/examples; V=ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0
mkdir -p stub/log   # one header: ALOGD(...) etc. as no-ops, because awnn_internal.h includes Android's <log/log.h>
gcc -O2 -Istub -o npu_run npu_run.c $S/libawnn_viplite/awnn_lib.c $S/libawnn_viplite/awnn_quantize.c -I$S/libawnn_viplite -I$V/inc -L$V -lNBGlinker -lVIPhal -lm
LD_LIBRARY_PATH=$V python3 yolo_detect.py image.jpg out.jpg      # prints label, score, [x0 y0 x1 y1]; writes the annotated image
```
`npu_run.c` hard-codes the three YOLOv5s output sizes; other networks need `sizes[]` changed.

## Not done
* No detection loop wired to the camera service (it would read frames from the loopback node; ~11 fps would be the ceiling from the 80 ms figure, the NPU itself is ~20 fps).
* Only prebuilt `.nb` models; converting your own needs Radxa's ACUITY toolkit on an x86 PC.
