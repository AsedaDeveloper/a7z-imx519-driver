# Thermal and clock behaviour at full load (checklist C8, partial)

15 minutes at the heaviest thing the camera can do — 4656x3496 at 15 fps through the 2-in-1 path with autofocus running — sampling every thermal zone, both
CPU clusters, the cooling devices and the delivered frame rate every 5 s. `tools/thermal_soak.py 900 --mode 4656x3496@15`, and `--idle` for the baseline.

## Result
| | idle | 15 min at 16 MP / 15 fps |
|---|---|---|
| hottest zone | 45 C | **51 C** (`cpul`) |
| skin | 33 C | 34 C |
| frame rate | - | 15.0 -> 15.1 fps, never below except one 12.7 dip at start-up |
| big-core clock | - | reached the full rated **2002 MHz** |
| critical trip | 110 C | never approached |

**Nothing overheats and nothing slows down.** The passive trip is 60 C and critical 110 C; the hottest zone settled around 50 C and was still flat after 15 minutes,
so the camera is nowhere near thermally limited in this configuration.

## What does get throttled, and why it does not matter here
During the run the **little cluster** is capped hard — `cpufreq-cpu0` sits at cooling state 8 of 8 and `scaling_max_freq` drops from 1794 MHz to **416 MHz** — while
the big cluster stays unrestricted at 2002 MHz. This is not the passive trip firing: at ~48 C the zones are 12 C below it. The thermal policy is `power_allocator`,
a power-budget governor, which trims the cheapest cores to stay inside a power budget rather than waiting for a temperature threshold. The cap is transient and
released as soon as the load stops (back to 1794 MHz, cooling state 0).

It costs the camera nothing because `a7zcap` pins itself to a big core on purpose (`pin_big_core`, `A7Z_CPU` to override) — the reason that pinning exists is
that the little cores are slow, and this is a second reason it matters. Anything *else* running on the little cores during capture will be slowed to 416 MHz.

## What this does not cover
* **Not a closed case.** There is no enclosure on this board, and the **fan runs at full speed throughout** (`pwm-fan` at 4/4 the entire run, idle included). These are
  open-air, actively-cooled numbers. A sealed case with no fan is the case the checklist actually asks about and it has not been tested — expect materially higher
  temperatures and, unlike here, real throttling.
* **Power draw was not measured.** `/sys/class/power_supply` is empty on this board, so there is no on-board reading; it needs an inline USB-C power meter.
* 15 minutes, one ambient temperature, one mode.

That is why C8 stays unticked.
