# MIPI link margin (checklist B6)

The question B6 asks is: when the link is pushed, does the hardware *tell* you, or do you only find out by looking at the picture? On this SoC the answer is that it
tells you — but not through the register the vendor header points at, and not as a rate.

## The register that actually reports errors
`combo_rx_reg_i.h` names `CMB_MIP_CSI2_ECC_ERR_CNT_REG_OFF 0x118` and `..._CHECK_SUM_ERR_CNT 0x11C`, which read like error counters. At `0x5811400` (the camera's
`mipi1` node, second reg range) they are not:

* `0x11C` reads a constant `0x16BC002B` while streaming and `0` when idle — not a count of anything.
* `0x118` changes only in the first ~1.5 s of a run and then holds the same value for the rest of it (checked over 32 s / ~480 frames). Its values are not
  counts either: `0x04F`, `0x84F`, `0xA4F`, `0xECF` — all inside `MIPI_INT_ALL = 0x1fff`.

Those are the bit positions of the combo-CSI port's **channel-0 interrupt-pending word** (`CMB_PORT_MIPI_CH0_INT_PD`, `combo_csi_reg_i.h:404`), which the
neighbouring block maps at the same offset. The driver never enables or clears these interrupts, so the bits are **sticky**: zero when the receiver powers up at
stream start, then latched for the whole run. So the word is a per-run *"did the link ever go wrong"* record, not an error rate — which is exactly what the
frozen value was telling us.

Bits that matter: 7 `frame_sync_err`, 8 `line_sync_err`, 9 `ecc_warn`, 10 `ecc_err`, 11 `chksum_err`, 12 `eot_err`.

## What the bits say across lane rates
`tools/link_ladder.py IOP ...` sets `fast_iop` (lane rate = 3 x IOP Mbit/s), captures at full resolution, accumulates the pending word for the whole run and scores
the last frame with `corruption_check.py`. One module, one 15-pin cable, one indoor scene, 60-90 frames per rate:

| lane rate | frames | outliers /Mpx | int_pd | error bits latched |
|---|---|---|---|---|
| 816 (stock, `A7Z_FPS=9`) | yes | 2.5 and 3.6 (two runs) | `0x04f` | **none** |
| 1224 | **none** | - | `0x04f` | none (the stream never started) |
| 1296 (the `A7Z_FPS=15` default) | yes | 3.6 | `0x84f` | **chksum_err** |
| 1344 | yes | 290 | `0xa4f` | chksum_err, **ecc_warn** |
| 1368 | yes | 9063 | `0xecf` | chksum_err, ecc_warn, **ecc_err**, **frame_sync_err** |
| 1392 | yes | 29062 | `0xecf` | the same four |

Two things worth having:

* **The bits track the damage, one step ahead of it.** `ecc_warn` appears at 1344, the first rate where the picture is measurably damaged (290 outliers per Mpx
  against a floor of ~3 in this scene); `ecc_err` and `frame_sync_err` appear at 1368, where the damage becomes gross. The error bits and the image metric agree.
* **The shipped default already latches `chksum_err`.** At 1296 Mbit/s the picture is at the noise floor and looks clean by every image test, but the receiver
  records a checksum error during the run, and the stock 816 Mbit/s mode does not — twice, cleanly. So the default is running with less margin than the image
  metric alone suggests. It is one latched bit over a whole run, not continuous corruption, but it is the reason to treat 1296 as the ceiling rather than a
  comfortable operating point.

1224 Mbit/s produces no frames here because the ladder varies only `fast_iop`: the pixel clock (`fast_ivt`) stays at the default, and below ~1296 Mbit/s the two
lanes cannot carry it. Reaching a lower rate means lowering `fast_ivt` too (that is what the lower-rate rows of README.md's table did), which also lowers the frame rate.

## Using it
* `a7zstatus` prints `mipi link  int_pd 0x...  latched errors this run: ...` and warns on `ecc_error`, `frame_sync`, `line_sync` or `eot`. It needs `/dev/mem`,
  so the line appears only when run as root; everything else in `a7zstatus` still works as an ordinary user.
* `sudo tools/link_ladder.py 432 448 456 464` repeats the table above (stop the camera service first; pass `BIN=/home/radxa/bin/a7zcap.enc`, since sudo does not expand `~`).

## What this does not cover
* Per-lane D-PHY status (`SOT_ERR_D0..D3`, `RX_ALG_ERR`, `CD_LP*`) exists in `dphy_reg.c` as a read function but is not exposed through any node, and its base
  address is not in the live device tree; reaching it would mean matching the register map by hand.
* There is no **handling**: the driver does not enable these interrupts, so nothing reacts to a latched error. A real recovery path (reset the link, drop the rate)
  would be a driver change.
* One module, one cable, one scene, short runs. The bits are a per-run summary, so a single rare error in an hour-long run looks the same as a thousand.

## What the receiver carries (B17)
`tools/mipi_types.py` samples the packet-header register of mipi1 while streaming. 2328x1748: 97%+ RAW10 lines of 2910 bytes, frame start/end, virtual channel 0 only. 4656x3496 (2-in-1): RAW10 lines of 5820 bytes, same. No embedded-data (0x12) or vendor data types, no second virtual channel, in 4.3-9.1 million samples; debugfs `/sys/kernel/debug/mpp/mipi` shows the port configured for 1 channel. Phase-detect data therefore does not arrive as its own stream on this stack.
