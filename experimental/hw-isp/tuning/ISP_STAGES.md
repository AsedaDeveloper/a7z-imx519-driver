# The ISP's stages, by name (checklist B13)

## Stage-enable flags
The param file's test block ends in 32 enable bytes at file offset 88 of the param struct (`p[88 + k]` in `mkparam.py`, `flagbytes` in the tuning JSON). They are
the fields of `struct isp_test_param` from the vendor BSP header (`drivers/vin/vin-isp/isp_tuning_priv.h`), in order, starting with `manual_en`. Compiled on the board:
`sizeof(struct isp_test_param)` is 124 and `manual_en` sits at 88, both exactly the file's numbers (`struct_offsets.c`).

| flag | stage | in our deployed param (v7) | what we measured |
|---|---|---|---|
| 0 | manual_en | off | worse on clipping and detail |
| 1 | afs_en | on | anti-flicker statistics |
| 2 | ae_en | on | |
| 3 | af_en | off | ISP autofocus statistics; ours is software autofocus |
| 4 | awb_en | on | |
| 5 | hist_en | on | |
| 6 | wdr_split_en | off | **black frame** if set (no WDR input) |
| 7 | wdr_stitch_en | off | worse |
| 8 | otf_dpc_en | on | defect-pixel correction |
| 9 | ctc_en | on | cross-talk correction |
| 10 | gca_en | off | worse |
| 11 | nrp_en | on | noise-reduction pre-process |
| 12 | denoise_en | on | 2D noise reduction |
| 13 | tdf_en | on | 3D (temporal) noise reduction |
| 14 | blc_en | on | black level |
| 15 | wb_en | on | |
| 16 | dig_gain_en | on | |
| 17 | lsc_en | off | worse (rsc lens shading; the MSC mesh shading, flag 18, is the one in use) |
| 18 | msc_en | on | mesh lens shading |
| 19 | pltm_en | on | local tone mapping (PLTM) |
| 20 | cfa_en | off | worse |
| 21 | lca_en | off | worse |
| 22 | sharp_en | on | switching it off: high-pass energy 0.39 -> 0.30 (-23%) |
| 23 | ccm_en | on | |
| **24** | **defog_en** | **on (v7)** | **the flag that cut clipped pixels by about a third** |
| 25 | cnr_en | on | chroma noise reduction |
| 26 | drc_en | off | worse on both counts |
| 27 | gtm_en | off | global tone mapping: sharpest, worst highlights |
| 28 | gamma_en | on | switching it off: mean luma 135 -> 106, 5th percentile 47 -> 22 |
| 29 | cem_en | on | |
| 30 | encpp_en | off | worse |
| 31 | enc_3dnr_en | off | worse |
(32 `enc_2dnr_en` would be the next byte; it is outside the 32 the file carries.)

How it was checked, beyond the header: the 12 off flags in the deployed file are exactly the stages one would expect off for this sensor, and with flag 24 they are the "thirteen" the
earlier sweep covered; flag 6 (WDR split) blanks the frame; flag 24 (defog) was the one absent from the main enable table, which is what a stage with its own config looks like;
and two flips on the running camera behaved as the names say (table above: `sharp_en`, `gamma_en`; baseline repeated 0.39 / 0.40).
Everything the checklist item lists is therefore already in the pipeline, or was tested and left off: local tone mapping (PLTM, on), DRC (off, worse), 2D and 3D noise
reduction (on), sharpening (on), defect-pixel correction (on), lens shading (MSC on, LSC off).

## Where the parameters live (tuning block)
`struct isp_tunning_param` offsets in the header, from `struct_offsets.c`. The header is a close revision of the library's, not an identical one (99384 bytes against the file's 99440):
the colour matrices land exactly where `mkparam.py` edits them (84362), the lens-shading tables are 6 bytes earlier than in the file (282 against 288). Trust the order and sizes, check each offset
against a known value before patching.

| block | offset | bytes | note |
|---|---|---|---|
| lsc_tbl (rsc) | 282 | 18432 | 12 tables x 768 u16 (file: 288) |
| msc_tbl | 18784 | 34848 | |
| gamma_tbl_ini | 53632 | 30720 | |
| color_matrix_ini | 84362 | 72 | matches `mkparam.py` |
| gca_cfg | 84440 | 18 | 9 x s16 |
| pltm_cfg | 84460 | 180 | 45 x s32: the local tone-mapping parameters |
| sharp_comm_cfg | 84640 | 20 | 10 x s16 |
| denoise_comm_cfg | 84660 | 66 | 33 x s16 |
| tdf_comm_cfg | 84726 | 90 | 45 x s16 (3D noise reduction) |
| sensor_temp | 84836 | 336 | |
| isp_sharp_ss_value / ls_value / hsv | 85408 | 66 / 66 / 92 | sharpening strength tables |
| isp_pltm_stat_gd_cv / df_cv | 85880 / 85940 | 60 / 132 | |
| isp_cem_table | 86072 | 5888 | |
| isp_pltm_lum_map_cv | 97848 | 512 | |
| isp_pltm_gtm_tbl | 98360 | 1024 | |

## Not known
What each element of `pltm_cfg`, `sharp_comm_cfg`, `denoise_comm_cfg` and `tdf_comm_cfg` means: the header gives only the array sizes (`#define ISP_PLTM_MAX 45` and so on), and the
per-ISO dynamic block (`isp_iso_settings`) is not in the 3A/tuning split the compiled header predicts (the 3A block is 5760 bytes in the header, 2724 in the file).
Finding which element is "strength" needs a sweep, one element at a time, on a scene with real highlights (the same method as the flag sweep).
