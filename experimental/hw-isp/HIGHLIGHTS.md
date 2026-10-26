# Highlights and the unused ISP stages (checklist A1 / B13)

libisp has the blocks we want — `__IspPltmCtrl` / `__AwPltmRun` (programmable local tone mapping), `ApplyDrcCurve`, `__GlobalToneMapping`, `config_gamma` — and 32
stage-enable flags living as bytes 88..119 of the param file's test block (`flagbytes` in `mkparam.py`). Thirteen of them are off in our tuning.

## What was measured
Same scene throughout (a lamp in frame, so real clipped highlights), 160-frame captures, autofocus and the exposure cap on, one flag flipped at a time.

**Do not set them all.** `flags_all_on` gives a completely black frame.

| flag | clipped | detail | note |
|---|---|---|---|
| baseline | 1.53% | 13.8 | |
| **24** | **1.00%** | 14.8 | the only flag that reduces clipping |
| 27 | 2.37% | 15.7 | sharpest, but the worst highlights |
| 6 | — | — | black frame |
| 0, 3, 7, 10, 17, 20, 21, 26, 30, 31 | 1.7-2.3% | 10.9-12.0 | all worse on both counts |

### Flag 24, repeated three times against a paired baseline
The room light drifted during the session (baseline clipping wandered between 1.53% and 2.95%), so single captures are not comparable — only runs taken in the
same stretch are. Paired:

| baseline | with flag 24 | change | noise (HP std, flat areas) |
|---|---|---|---|
| 1.53% | 1.00% | −34% | 0.686 → 0.681 |
| 2.95% | 1.91% | −35% | 0.657 → 0.659 |
| 2.70% | 1.93% | −29% | 0.664 → 0.656 |

Clipped pixels fall by about a third, every time. Noise is unchanged, colour is unchanged (R/G and B/G within 0.01), and detail is unchanged — the single-shot
"+1.0 detail" in the first sweep was scene drift, not an effect.

**Deployed as `tuning/tuning_v7.json`** (v6 plus `flagbytes 24:1`). Regression check after deploying: 1840 frames at 30.0 fps, 0 drops, autofocus normal.
Previous table kept on the board as `~/isp_param_config.bin.v6`.

## Why this matters for A1
The earlier approach (`A7Z_HAE`, setting exposure ourselves) protected highlights far harder — 14.7% clipped down to 0.9% — but cost most of the sharpness,
because libisp picks its noise reduction from the exposure it *requested*, which inflates when we apply less. Flag 24 is the opposite trade: a smaller gain, but
free. Nothing is given up.

## A1 is not finished
A third fewer clipped pixels is not real highlight protection — a bright window would still blow out. What is still missing:
* which stage flag 24 actually is — **two attempts failed**:
  1. The main enable path (`__isp_ctx_apply_enable`) maps each flag byte to a module-enable bit, but three of the thirty-two bytes, including the one flag 24
     writes, are absent from that table. So it is configured elsewhere, which is consistent with a stage carrying its own config such as PLTM, but that is
     inference, not proof.
  2. Diffing libisp's live 3A state (`ispGet3AParameters`, 73416 bytes, dumped with `A7Z_3ADUMP=`) between the two tunings does not isolate it: a control pair
     taken with the *same* tuning differs in 2531 bytes, more than the 1469 that differ between v6 and v7. The buffer is dominated by per-run statistics, so
     the method cannot separate the flag's effect from noise.

  The behaviour is at least consistent with tone mapping — highlights compressed, shadows slightly raised, detail untouched — but it is unproven. Identifying it
  needs a different approach: reading the ISP hardware registers directly, or a static scene with exposure pinned so the curve itself can be plotted.
* the PLTM / DRC curve parameters themselves. Only the enable flag was touched; the curves are untouched defaults.
* a scene with a genuinely blown window, rather than a lamp, to test against.
