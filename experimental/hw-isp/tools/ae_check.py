#!/usr/bin/env python3
"""Does libisp's auto exposure hunt, and how does it answer a brightness step? (checklist B8)

usage (on the board, service running):  ae_check.py [SECONDS]      default 40

Logs per frame: mean luma and the sensor exposure x gain that applied to that frame (from the metadata ring).
  steady state  - how far exposure x gain wanders and how often its direction reverses while the scene is unchanged
  step response - if the mean luma jumps by >10% inside the run (cover or uncover the lens), frames until luma is within 10% of its
                  final value, the largest overshoot past the final value, and direction reversals after the step
Exposure takes effect 4 frames after a command, so an AE loop that ignores that shows up here as overshoot and reversals.
"""
import math, sys, time
sys.path.insert(0, "/usr/local/lib")
from a7z_v4l2 import Camera


def med(x):
    s = sorted(x)
    return s[len(s) // 2]


def reversals(e, thr):
    """Direction changes of log-exposure larger than thr (fractional) between turning points."""
    n, last, d, ext = 0, e[0], 0, e[0]
    for v in e[1:]:
        if d >= 0 and v < ext - thr: n, d, ext = n + (d != 0), -1, v
        elif d <= 0 and v > ext + thr: n, d, ext = n + (d != 0), 1, v
        elif (d >= 0 and v > ext) or (d <= 0 and v < ext) or d == 0: ext = v if d != 0 else ext
        if d == 0:
            if v > e[0] + thr: d, ext = 1, v
            elif v < e[0] - thr: d, ext = -1, v
    return n


def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    L, E, T = [], [], []
    with Camera() as cam:
        t0 = time.time()
        while time.time() - t0 < secs:
            y, u, v, m = cam.read_with_meta()
            L.append(sum(y[::997]) / len(y[::997]))
            E.append(math.log(m["exp_us"] * m["gain"] / 16) if m else float("nan"))
            T.append(time.time() - t0)
    open("/tmp/ae_trace.csv", "w").write("t,luma,ln_exp_gain\n" + "".join("%.3f,%.1f,%.4f\n" % r for r in zip(T, L, E)))
    ok = [i for i in range(len(E)) if E[i] == E[i]]
    L = [L[i] for i in ok]; E = [E[i] for i in ok]; T = [T[i] for i in ok]
    n = len(L)
    print("%d frames over %.0f s, luma %.0f..%.0f" % (n, T[-1], min(L), max(L)))

    step = None
    for i in range(30, n):
        base = med(L[i - 30:i - 5])
        if abs(L[i] - base) > 0.10 * max(base, 8):
            step = i
            break
    pre = n if step is None else max(step - 5, 1)
    es = E[:pre]
    print("\nsteady state (first %d frames, %.1f s)" % (pre, T[pre - 1]))
    print("  exposure x gain: min %.2fx  max %.2fx of median   luma sd %.2f" %
          (math.exp(min(es) - med(es)), math.exp(max(es) - med(es)),
           (sum((l - sum(L[:pre]) / pre) ** 2 for l in L[:pre]) / pre) ** 0.5))
    print("  direction reversals >5%%: %d   (>2%%: %d)" % (reversals(es, 0.05), reversals(es, 0.02)))

    if step is None:
        print("\nno brightness step seen (cover/uncover the lens while it runs to test the step response)")
        return
    fin = med(L[-30:])
    tgt = max(0.1 * fin, 3)
    settle = next((i - step for i in range(step, n) if all(abs(l - fin) <= tgt for l in L[i:i + 15])), None)
    dev = [l - fin for l in L[step + 3:]]
    over = max(dev) if fin < L[step] else -min(dev) if dev else 0
    print("\nstep at %.1fs: luma %.0f -> %.0f (final)" % (T[step], med(L[step - 30:step - 5]), fin))
    print("  settled within 10%% after %s frames (%.2f s)" % (settle, (settle or 0) / 30) if settle is not None else "  did not settle within the run")
    print("  exposure x gain moved %.1fx" % math.exp(abs(med(E[-30:]) - med(E[step - 30:step - 5]))))
    print("  reversals after step (>5%%): %d" % reversals(E[step:], 0.05))
    seg = [(round(T[i], 2), round(L[i]), round(math.exp(E[i] - E[0]), 2)) for i in range(step - 2, min(step + 45, n), 3)]
    print("  t, luma, rel. exp*gain:", seg)


if __name__ == "__main__":
    main()
