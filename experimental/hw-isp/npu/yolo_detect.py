#!/usr/bin/env python3
"""YOLOv5s (COCO) on the NPU: yolo_detect.py image.jpg [out.jpg] [nbg] -- prints detections, writes an annotated copy.
Needs npu_run (built from npu_run.c) next to it and the ai-sdk v2.0 libraries on LD_LIBRARY_PATH. Preprocessing and decoding follow Allwinner's yolov5 example."""
import sys, os, subprocess, tempfile, numpy as np, cv2
HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = "person bicycle car motorcycle airplane bus train truck boat traffic_light fire_hydrant stop_sign parking_meter bench bird cat dog horse sheep cow elephant bear zebra giraffe backpack umbrella handbag tie suitcase frisbee skis snowboard sports_ball kite baseball_bat baseball_glove skateboard surfboard tennis_racket bottle wine_glass cup fork knife spoon bowl banana apple sandwich orange broccoli carrot hot_dog pizza donut cake chair couch potted_plant bed dining_table toilet tv laptop mouse remote keyboard cell_phone microwave oven toaster sink refrigerator book clock vase scissors teddy_bear hair_drier toothbrush".split()
ANCH = {8: [10, 13, 16, 30, 33, 23], 16: [30, 61, 62, 45, 59, 119], 32: [116, 90, 156, 198, 373, 326]}
img = cv2.imread(sys.argv[1]); out = sys.argv[2] if len(sys.argv) > 2 else 'detected.jpg'
nbg = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, 'yolov5.nb')
S = 640; sc = min(S / img.shape[0], S / img.shape[1]); rw, rh = int(sc * img.shape[1]), int(sc * img.shape[0])
rgb = cv2.cvtColor(cv2.resize(img, (rw, rh)), cv2.COLOR_BGR2RGB)
top, left = (S - rh) // 2, (S - rw) // 2
canvas = np.zeros((S, S, 3), np.uint8); canvas[top:top + rh, left:left + rw] = rgb
tmp = tempfile.mkdtemp(); inp = os.path.join(tmp, 'in.u8'); canvas.transpose(2, 0, 1).tofile(inp)          # planar RGB
r = subprocess.run([os.path.join(HERE, 'npu_run'), nbg, inp, os.path.join(tmp, 'o')], capture_output=True, text=True)
print(r.stderr.strip().splitlines()[-1] if r.stderr.strip() else r.stdout.strip())
sig = lambda x: 1 / (1 + np.exp(-x))
boxes, scores, labels = [], [], []
for i, st in enumerate((8, 16, 32)):
    n = S // st; f = np.fromfile(os.path.join(tmp, 'o%d.f32' % i), np.float32).reshape(3, n, n, 85)
    obj = sig(f[..., 4]); cls = sig(f[..., 5:]); ci = cls.argmax(-1); conf = obj * cls.max(-1)
    for a, h, w in zip(*np.where(conf >= 0.4)):
        d = sig(f[a, h, w, :4]); cx = (d[0] * 2 - .5 + w) * st; cy = (d[1] * 2 - .5 + h) * st
        bw = d[2] ** 2 * 4 * ANCH[st][a * 2]; bh = d[3] ** 2 * 4 * ANCH[st][a * 2 + 1]
        boxes.append([cx - bw / 2, cy - bh / 2, bw, bh]); scores.append(float(conf[a, h, w])); labels.append(int(ci[a, h, w]))
keep = cv2.dnn.NMSBoxes(boxes, scores, 0.4, 0.45) if boxes else []
for k in np.array(keep).reshape(-1):
    x, y, bw, bh = boxes[k]; x0 = int((x - left) / sc); y0 = int((y - top) / sc); x1 = int((x + bw - left) / sc); y1 = int((y + bh - top) / sc)
    print('%-14s %.2f  [%d, %d, %d, %d]' % (NAMES[labels[k]], scores[k], x0, y0, x1, y1))
    cv2.rectangle(img, (x0, y0), (x1, y1), (0, 255, 0), 2); cv2.putText(img, '%s %.2f' % (NAMES[labels[k]], scores[k]), (x0, max(12, y0 - 4)), cv2.FONT_HERSHEY_SIMPLEX, .6, (0, 255, 0), 2)
if not len(keep): print('no detections')
cv2.imwrite(out, img)
