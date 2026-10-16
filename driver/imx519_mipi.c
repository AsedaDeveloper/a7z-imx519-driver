/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * IMX519 (Sony 16 MP, 2-lane MIPI, autofocus module) sensor driver for the Allwinner VIN framework (Radxa Cubie A7Z).
 *
 * Derived from: the Allwinner/Radxa BSP sensor drivers (imx477_mipi / imx219 structure, GPL-2.0-or-later) and the
 * Raspberry Pi kernel imx519 driver (register tables and sensor logic, GPL-2.0). See docs/HOW_IT_WORKS.md for the
 * hardware traps this driver works around (I2C at 100 kHz, receiver instance, no register readback).
 */
/* Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved. */
/*
 * A V4L2 driver for IMX519 Raw cameras.
 *
 * Copyright (c) 2017 by Allwinnertech Co., Ltd.  http://www.allwinnertech.com
 *
 * Authors:  Zhao Wei <zhaowei@allwinnertech.com>
 *    Yang Feng <yangfeng@allwinnertech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "../../utility/vin_log.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <asm/div64.h>
#include <linux/videodev2.h>
#include <linux/clk.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mediabus.h>
#include <linux/io.h>
#include "camera.h"
#include "sensor_helper.h"

MODULE_AUTHOR("IMX519 port for the Radxa Cubie A7Z");
MODULE_DESCRIPTION("A low-level driver for IMX519 sensors");
MODULE_LICENSE("GPL");

/*
 * Which group of MIPI data pads the receiver listens on (the combo PHY "offset", 0..3).
 * The A7Z camera connector J5 wires the camera to the MCSIB lanes; change at run time with
 *   echo N > /sys/module/imx519_mipi/parameters/phy_offset
 */
static int phy_offset = 1;
module_param(phy_offset, int, 0644);
MODULE_PARM_DESC(phy_offset, "MIPI combo PHY lane-group offset 0..3");

static int init_delay_ms = 100;
module_param(init_delay_ms, int, 0644);
MODULE_PARM_DESC(init_delay_ms, "delay before the first register write after power-up");
static int exp_us;
module_param(exp_us, int, 0644);
MODULE_PARM_DESC(exp_us, "exposure in microseconds applied at every stream start (0 = default)");
static int gain_x16;
module_param(gain_x16, int, 0644);
MODULE_PARM_DESC(gain_x16, "total gain x16 applied at every stream start (0 = default)");
static int stream_delay_ms = 0;
module_param(stream_delay_ms, int, 0644);
MODULE_PARM_DESC(stream_delay_ms, "delay between the last mode register write and stream-on");
static int term_res = 1;
module_param(term_res, int, 0644);
MODULE_PARM_DESC(term_res, "enable the 100ohm MIPI termination (1) or not (0)");
#define IMX519_COMBO_MODE() ((term_res ? CMB_TERMINAL_RES : 0) | ((phy_offset & 3) << 4) | MIPI_NORMAL_MODE)

#define MCLK              (24*1000*1000)
#define V4L2_IDENT_SENSOR 0x0519

/*
 * Our nominal (default) frame rate.
 */
#ifdef FPGA
#define SENSOR_FRAME_RATE 15
#else
#define SENSOR_FRAME_RATE 30
#endif

/*
 * The IMX519 sits on i2c address 0x1a (7-bit)
 */
#define I2C_ADDR 0x34		/* 7-bit 0x1a, the BSP takes the 8-bit form */
#define SENSOR_NAME "imx519_mipi"
static int imx519_mipi_sensor_vts;
static int imx519_in_update;		/* set while the driver itself moves the blanking controls (no register writes then) */
static int imx519_user_vblank;		/* VBLANK set through the standard control for the current mode (0 = mode default) */
static struct v4l2_ctrl *imx519_vblank_ctrl, *imx519_hblank_ctrl, *imx519_pixel_rate_ctrl, *imx519_again_ctrl;
static int imx519_test_pattern;		/* last value of the test_pattern control (re-applied every stream start) */

struct cfg_array {		/* coming later */
	struct regval_list *regs;
	int size;
};



/*
 * The default register settings
 *
 */

/*
 * IMX519 register settings, generated from the Raspberry Pi / Arducam driver (rpi-6.6.y imx519.c).
 * 2 data lanes, RAW10, 24 MHz external clock, 408 MHz link frequency (816 Mbps per lane).
 */

static struct regval_list sensor_default_regs[] = {
	/* common settings (written first, stream off) */
	{0x0100, 0x00},
	{0x0136, 0x18},
	{0x0137, 0x00},
	{0x3c7e, 0x01},
	{0x3c7f, 0x07},
	{0x3020, 0x00},
	{0x3e35, 0x01},
	{0x3f7f, 0x01},
	{0x5609, 0x57},
	{0x5613, 0x51},
	{0x561f, 0x5e},
	{0x5623, 0xd2},
	{0x5637, 0x11},
	{0x5657, 0x11},
	{0x5659, 0x12},
	{0x5733, 0x60},
	{0x5905, 0x57},
	{0x590f, 0x51},
	{0x591b, 0x5e},
	{0x591f, 0xd2},
	{0x5933, 0x11},
	{0x5953, 0x11},
	{0x5955, 0x12},
	{0x5a2f, 0x60},
	{0x5a85, 0x57},
	{0x5a8f, 0x51},
	{0x5a9b, 0x5e},
	{0x5a9f, 0xd2},
	{0x5ab3, 0x11},
	{0x5ad3, 0x11},
	{0x5ad5, 0x12},
	{0x5baf, 0x60},
	{0x5c15, 0x2a},
	{0x5c17, 0x80},
	{0x5c19, 0x31},
	{0x5c1b, 0x87},
	{0x5c25, 0x25},
	{0x5c27, 0x7b},
	{0x5c29, 0x2a},
	{0x5c2b, 0x80},
	{0x5c2d, 0x31},
	{0x5c2f, 0x87},
	{0x5c35, 0x2b},
	{0x5c37, 0x81},
	{0x5c39, 0x31},
	{0x5c3b, 0x87},
	{0x5c45, 0x25},
	{0x5c47, 0x7b},
	{0x5c49, 0x2a},
	{0x5c4b, 0x80},
	{0x5c4d, 0x31},
	{0x5c4f, 0x87},
	{0x5c55, 0x2d},
	{0x5c57, 0x83},
	{0x5c59, 0x32},
	{0x5c5b, 0x88},
	{0x5c65, 0x29},
	{0x5c67, 0x7f},
	{0x5c69, 0x2e},
	{0x5c6b, 0x84},
	{0x5c6d, 0x32},
	{0x5c6f, 0x88},
	{0x5e69, 0x04},
	{0x5e9d, 0x00},
	{0x5f18, 0x10},
	{0x5f1a, 0x0e},
	{0x5f20, 0x12},
	{0x5f22, 0x10},
	{0x5f24, 0x0e},
	{0x5f28, 0x10},
	{0x5f2a, 0x0e},
	{0x5f30, 0x12},
	{0x5f32, 0x10},
	{0x5f34, 0x0e},
	{0x5f38, 0x0f},
	{0x5f39, 0x0d},
	{0x5f3c, 0x11},
	{0x5f3d, 0x0f},
	{0x5f3e, 0x0d},
	{0x5f61, 0x07},
	{0x5f64, 0x05},
	{0x5f67, 0x03},
	{0x5f6a, 0x03},
	{0x5f6d, 0x07},
	{0x5f70, 0x07},
	{0x5f73, 0x05},
	{0x5f76, 0x02},
	{0x5f79, 0x07},
	{0x5f7c, 0x07},
	{0x5f7f, 0x07},
	{0x5f82, 0x07},
	{0x5f85, 0x03},
	{0x5f88, 0x02},
	{0x5f8b, 0x01},
	{0x5f8e, 0x01},
	{0x5f91, 0x04},
	{0x5f94, 0x05},
	{0x5f97, 0x02},
	{0x5f9d, 0x07},
	{0x5fa0, 0x07},
	{0x5fa3, 0x07},
	{0x5fa6, 0x07},
	{0x5fa9, 0x03},
	{0x5fac, 0x01},
	{0x5faf, 0x01},
	{0x5fb5, 0x03},
	{0x5fb8, 0x02},
	{0x5fbb, 0x01},
	{0x5fc1, 0x07},
	{0x5fc4, 0x07},
	{0x5fc7, 0x07},
	{0x5fd1, 0x00},
	{0x6302, 0x79},
	{0x6305, 0x78},
	{0x6306, 0xa5},
	{0x6308, 0x03},
	{0x6309, 0x20},
	{0x630b, 0x0a},
	{0x630d, 0x48},
	{0x630f, 0x06},
	{0x6311, 0xa4},
	{0x6313, 0x03},
	{0x6314, 0x20},
	{0x6316, 0x0a},
	{0x6317, 0x31},
	{0x6318, 0x4a},
	{0x631a, 0x06},
	{0x631b, 0x40},
	{0x631c, 0xa4},
	{0x631e, 0x03},
	{0x631f, 0x20},
	{0x6321, 0x0a},
	{0x6323, 0x4a},
	{0x6328, 0x80},
	{0x6329, 0x01},
	{0x632a, 0x30},
	{0x632b, 0x02},
	{0x632c, 0x20},
	{0x632d, 0x02},
	{0x632e, 0x30},
	{0x6330, 0x60},
	{0x6332, 0x90},
	{0x6333, 0x01},
	{0x6334, 0x30},
	{0x6335, 0x02},
	{0x6336, 0x20},
	{0x6338, 0x80},
	{0x633a, 0xa0},
	{0x633b, 0x01},
	{0x633c, 0x60},
	{0x633d, 0x02},
	{0x633e, 0x60},
	{0x633f, 0x01},
	{0x6340, 0x30},
	{0x6341, 0x02},
	{0x6342, 0x20},
	{0x6343, 0x03},
	{0x6344, 0x80},
	{0x6345, 0x03},
	{0x6346, 0x90},
	{0x6348, 0xf0},
	{0x6349, 0x01},
	{0x634a, 0x20},
	{0x634b, 0x02},
	{0x634c, 0x10},
	{0x634d, 0x03},
	{0x634e, 0x60},
	{0x6350, 0xa0},
	{0x6351, 0x01},
	{0x6352, 0x60},
	{0x6353, 0x02},
	{0x6354, 0x50},
	{0x6355, 0x02},
	{0x6356, 0x60},
	{0x6357, 0x01},
	{0x6358, 0x30},
	{0x6359, 0x02},
	{0x635a, 0x30},
	{0x635b, 0x03},
	{0x635c, 0x90},
	{0x635f, 0x01},
	{0x6360, 0x10},
	{0x6361, 0x01},
	{0x6362, 0x40},
	{0x6363, 0x02},
	{0x6364, 0x50},
	{0x6368, 0x70},
	{0x636a, 0xa0},
	{0x636b, 0x01},
	{0x636c, 0x50},
	{0x637d, 0xe4},
	{0x637e, 0xb4},
	{0x638c, 0x8e},
	{0x638d, 0x38},
	{0x638e, 0xe3},
	{0x638f, 0x4c},
	{0x6390, 0x30},
	{0x6391, 0xc3},
	{0x6392, 0xae},
	{0x6393, 0xba},
	{0x6394, 0xeb},
	{0x6395, 0x6e},
	{0x6396, 0x34},
	{0x6397, 0xe3},
	{0x6398, 0xcf},
	{0x6399, 0x3c},
	{0x639a, 0xf3},
	{0x639b, 0x0c},
	{0x639c, 0x30},
	{0x639d, 0xc1},
	{0x63b9, 0xa3},
	{0x63ba, 0xfe},
	{0x7600, 0x01},
	{0x79a0, 0x01},
	{0x79a1, 0x01},
	{0x79a2, 0x01},
	{0x79a3, 0x01},
	{0x79a4, 0x01},
	{0x79a5, 0x20},
	{0x79a9, 0x00},
	{0x79aa, 0x01},
	{0x79ad, 0x00},
	{0x79af, 0x00},
	{0x8173, 0x01},
	{0x835c, 0x01},
	{0x8a74, 0x01},
	{0x8c1f, 0x00},
	{0x8c27, 0x00},
	{0x8c3b, 0x03},
	{0x9004, 0x0b},
	{0x920c, 0x6a},
	{0x920d, 0x22},
	{0x920e, 0x6a},
	{0x920f, 0x23},
	{0x9214, 0x6a},
	{0x9215, 0x20},
	{0x9216, 0x6a},
	{0x9217, 0x21},
	{0x9385, 0x3e},
	{0x9387, 0x1b},
	{0x938d, 0x4d},
	{0x938f, 0x43},
	{0x9391, 0x1b},
	{0x9395, 0x4d},
	{0x9397, 0x43},
	{0x9399, 0x1b},
	{0x939d, 0x3e},
	{0x939f, 0x2f},
	{0x93a5, 0x43},
	{0x93a7, 0x2f},
	{0x93a9, 0x2f},
	{0x93ad, 0x34},
	{0x93af, 0x2f},
	{0x93b5, 0x3e},
	{0x93b7, 0x2f},
	{0x93bd, 0x4d},
	{0x93bf, 0x43},
	{0x93c1, 0x2f},
	{0x93c5, 0x4d},
	{0x93c7, 0x43},
	{0x93c9, 0x2f},
	{0x974b, 0x02},
	{0x995c, 0x8c},
	{0x995d, 0x00},
	{0x995e, 0x00},
	{0x9963, 0x64},
	{0x9964, 0x50},
	{0xaa0a, 0x26},
	{0xae03, 0x04},
	{0xae04, 0x03},
	{0xae05, 0x03},
	{0xbc1c, 0x08},
	{0xbcf1, 0x02},
	{0x38a3, 0x00},
};

static struct regval_list sensor_full_regs[] = {
	/* 4656x3496, hts 12650, vts 3572, about 9.4 fps */
	{0x0111, 0x02},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0342, 0x31},
	{0x0343, 0x6a},
	{0x0340, 0x0d},
	{0x0341, 0xf4},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x12},
	{0x0349, 0x2f},
	{0x034a, 0x0d},
	{0x034b, 0xa7},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0a},
	{0x3f4c, 0x01},
	{0x3f4d, 0x01},
	{0x4254, 0x7f},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x12},
	{0x040d, 0x30},
	{0x040e, 0x0d},
	{0x040f, 0xa8},
	{0x034c, 0x12},
	{0x034d, 0x30},
	{0x034e, 0x0d},
	{0x034f, 0xa8},
	{0x0301, 0x06},
	{0x0303, 0x04},
	{0x0305, 0x06},
	{0x0306, 0x01},
	{0x0307, 0x40},
	{0x0309, 0x0a},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x10},
	{0x0310, 0x01},
	{0x0820, 0x0a},
	{0x0821, 0x20},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3e20, 0x01},
	{0x3e37, 0x01},
	{0x3e3b, 0x00},
	{0x38a4, 0x00},
	{0x38a5, 0x00},
	{0x38a6, 0x00},
	{0x38a7, 0x00},
	{0x38a8, 0x01},
	{0x38a9, 0x23},
	{0x38aa, 0x01},
	{0x38ab, 0x23},
	{0x0106, 0x00},
	{0x0b00, 0x00},
	{0x3230, 0x00},
	{0x3f14, 0x01},
	{0x3f3c, 0x01},
	{0x3f0d, 0x0a},
	{0x3fbc, 0x00},
	{0x3c06, 0x00},
	{0x3c07, 0x48},
	{0x3c0a, 0x00},
	{0x3c0b, 0x00},
	{0x3f78, 0x00},
	{0x3f79, 0x40},
	{0x3f7c, 0x00},
	{0x3f7d, 0x00},
	/* default exposure / gain */
	{0x0202, 0x03}, {0x0203, 0xe8},
	{0x0204, 0x00}, {0x0205, 0x00},
	{0x020e, 0x01}, {0x020f, 0x00},
};

static struct regval_list sensor_4k_regs[] = {
	/* 3840x2160, hts 10486, vts 2260, about 18.0 fps */
	{0x0111, 0x02},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0342, 0x28},
	{0x0343, 0xf6},
	{0x0340, 0x08},
	{0x0341, 0xd4},
	{0x0344, 0x01},
	{0x0345, 0x98},
	{0x0346, 0x02},
	{0x0347, 0xa0},
	{0x0348, 0x10},
	{0x0349, 0x97},
	{0x034a, 0x0b},
	{0x034b, 0x17},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0900, 0x00},
	{0x0901, 0x11},
	{0x0902, 0x0a},
	{0x3f4c, 0x01},
	{0x3f4d, 0x01},
	{0x4254, 0x7f},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x0f},
	{0x040d, 0x00},
	{0x040e, 0x08},
	{0x040f, 0x70},
	{0x034c, 0x0f},
	{0x034d, 0x00},
	{0x034e, 0x08},
	{0x034f, 0x70},
	{0x0301, 0x06},
	{0x0303, 0x04},
	{0x0305, 0x06},
	{0x0306, 0x01},
	{0x0307, 0x40},
	{0x0309, 0x0a},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x10},
	{0x0310, 0x01},
	{0x0820, 0x0a},
	{0x0821, 0x20},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3e20, 0x01},
	{0x3e37, 0x01},
	{0x3e3b, 0x00},
	{0x38a4, 0x00},
	{0x38a5, 0x00},
	{0x38a6, 0x00},
	{0x38a7, 0x00},
	{0x38a8, 0x00},
	{0x38a9, 0xf0},
	{0x38aa, 0x00},
	{0x38ab, 0xb4},
	{0x0106, 0x00},
	{0x0b00, 0x00},
	{0x3230, 0x00},
	{0x3f14, 0x01},
	{0x3f3c, 0x01},
	{0x3f0d, 0x0a},
	{0x3fbc, 0x00},
	{0x3c06, 0x00},
	{0x3c07, 0x48},
	{0x3c0a, 0x00},
	{0x3c0b, 0x00},
	{0x3f78, 0x00},
	{0x3f79, 0x40},
	{0x3f7c, 0x00},
	{0x3f7d, 0x00},
	/* default exposure / gain */
	{0x0202, 0x03}, {0x0203, 0xe8},
	{0x0204, 0x00}, {0x0205, 0x00},
	{0x020e, 0x01}, {0x020f, 0x00},
};

static struct regval_list sensor_bin_regs[] = {
	/* 2328x1748, hts 6512, vts 2184, about 30.0 fps */
	{0x0111, 0x02},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0342, 0x19},
	{0x0343, 0x70},
	{0x0340, 0x08},
	{0x0341, 0x88},
	{0x0344, 0x00},
	{0x0345, 0x00},
	{0x0346, 0x00},
	{0x0347, 0x00},
	{0x0348, 0x12},
	{0x0349, 0x2f},
	{0x034a, 0x0d},
	{0x034b, 0xa7},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x0a},
	{0x3f4c, 0x05},
	{0x3f4d, 0x03},
	{0x4254, 0x7f},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x09},
	{0x040d, 0x18},
	{0x040e, 0x06},
	{0x040f, 0xd4},
	{0x034c, 0x09},
	{0x034d, 0x18},
	{0x034e, 0x06},
	{0x034f, 0xd4},
	{0x0301, 0x06},
	{0x0303, 0x04},
	{0x0305, 0x06},
	{0x0306, 0x01},
	{0x0307, 0x40},
	{0x0309, 0x0a},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x10},
	{0x0310, 0x01},
	{0x0820, 0x0a},
	{0x0821, 0x20},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3e20, 0x01},
	{0x3e37, 0x01},
	{0x3e3b, 0x00},
	{0x38a4, 0x00},
	{0x38a5, 0x00},
	{0x38a6, 0x00},
	{0x38a7, 0x00},
	{0x38a8, 0x00},
	{0x38a9, 0x91},
	{0x38aa, 0x00},
	{0x38ab, 0x91},
	{0x0106, 0x00},
	{0x0b00, 0x00},
	{0x3230, 0x00},
	{0x3f14, 0x01},
	{0x3f3c, 0x01},
	{0x3f0d, 0x0a},
	{0x3fbc, 0x00},
	{0x3c06, 0x00},
	{0x3c07, 0x48},
	{0x3c0a, 0x00},
	{0x3c0b, 0x00},
	{0x3f78, 0x00},
	{0x3f79, 0x40},
	{0x3f7c, 0x00},
	{0x3f7d, 0x00},
	/* default exposure / gain */
	{0x0202, 0x03}, {0x0203, 0xe8},
	{0x0204, 0x00}, {0x0205, 0x00},
	{0x020e, 0x01}, {0x020f, 0x00},
};

static struct regval_list sensor_1080p_regs[] = {
	/* 1920x1080, hts 6027, vts 1180, about 60.0 fps */
	{0x0111, 0x02},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0342, 0x17},
	{0x0343, 0x8b},
	{0x0340, 0x04},
	{0x0341, 0x9c},
	{0x0344, 0x01},
	{0x0345, 0x98},
	{0x0346, 0x02},
	{0x0347, 0xa2},
	{0x0348, 0x10},
	{0x0349, 0x97},
	{0x034a, 0x0b},
	{0x034b, 0x15},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x0a},
	{0x3f4c, 0x05},
	{0x3f4d, 0x03},
	{0x4254, 0x7f},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x07},
	{0x040d, 0x80},
	{0x040e, 0x04},
	{0x040f, 0x38},
	{0x034c, 0x07},
	{0x034d, 0x80},
	{0x034e, 0x04},
	{0x034f, 0x38},
	{0x0301, 0x06},
	{0x0303, 0x04},
	{0x0305, 0x06},
	{0x0306, 0x01},
	{0x0307, 0x40},
	{0x0309, 0x0a},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x10},
	{0x0310, 0x01},
	{0x0820, 0x0a},
	{0x0821, 0x20},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3e20, 0x01},
	{0x3e37, 0x01},
	{0x3e3b, 0x00},
	{0x38a4, 0x00},
	{0x38a5, 0x00},
	{0x38a6, 0x00},
	{0x38a7, 0x00},
	{0x38a8, 0x00},
	{0x38a9, 0x78},
	{0x38aa, 0x00},
	{0x38ab, 0x5a},
	{0x0106, 0x00},
	{0x0b00, 0x00},
	{0x3230, 0x00},
	{0x3f14, 0x01},
	{0x3f3c, 0x01},
	{0x3f0d, 0x0a},
	{0x3fbc, 0x00},
	{0x3c06, 0x00},
	{0x3c07, 0x48},
	{0x3c0a, 0x00},
	{0x3c0b, 0x00},
	{0x3f78, 0x00},
	{0x3f79, 0x40},
	{0x3f7c, 0x00},
	{0x3f7d, 0x00},
	/* default exposure / gain */
	{0x0202, 0x03}, {0x0203, 0xe8},
	{0x0204, 0x00}, {0x0205, 0x00},
	{0x020e, 0x01}, {0x020f, 0x00},
};

static struct regval_list sensor_720p_regs[] = {
	/* 1280x720, hts 6144, vts 820, about 84.7 fps */
	{0x0111, 0x02},
	{0x0112, 0x0a},
	{0x0113, 0x0a},
	{0x0114, 0x01},
	{0x0342, 0x18},
	{0x0343, 0x00},
	{0x0340, 0x03},
	{0x0341, 0x34},
	{0x0344, 0x04},
	{0x0345, 0x18},
	{0x0346, 0x04},
	{0x0347, 0x12},
	{0x0348, 0x0e},
	{0x0349, 0x17},
	{0x034a, 0x09},
	{0x034b, 0xb6},
	{0x0220, 0x00},
	{0x0221, 0x11},
	{0x0222, 0x01},
	{0x0900, 0x01},
	{0x0901, 0x22},
	{0x0902, 0x0a},
	{0x3f4c, 0x05},
	{0x3f4d, 0x03},
	{0x4254, 0x7f},
	{0x0401, 0x00},
	{0x0404, 0x00},
	{0x0405, 0x10},
	{0x0408, 0x00},
	{0x0409, 0x00},
	{0x040a, 0x00},
	{0x040b, 0x00},
	{0x040c, 0x05},
	{0x040d, 0x00},
	{0x040e, 0x02},
	{0x040f, 0xd0},
	{0x034c, 0x05},
	{0x034d, 0x00},
	{0x034e, 0x02},
	{0x034f, 0xd0},
	{0x0301, 0x06},
	{0x0303, 0x04},
	{0x0305, 0x06},
	{0x0306, 0x01},
	{0x0307, 0x40},
	{0x0309, 0x0a},
	{0x030b, 0x02},
	{0x030d, 0x04},
	{0x030e, 0x01},
	{0x030f, 0x10},
	{0x0310, 0x01},
	{0x0820, 0x0a},
	{0x0821, 0x20},
	{0x0822, 0x00},
	{0x0823, 0x00},
	{0x3e20, 0x01},
	{0x3e37, 0x01},
	{0x3e3b, 0x00},
	{0x38a4, 0x00},
	{0x38a5, 0x00},
	{0x38a6, 0x00},
	{0x38a7, 0x00},
	{0x38a8, 0x00},
	{0x38a9, 0x50},
	{0x38aa, 0x00},
	{0x38ab, 0x3c},
	{0x0106, 0x00},
	{0x0b00, 0x00},
	{0x3230, 0x00},
	{0x3f14, 0x01},
	{0x3f3c, 0x01},
	{0x3f0d, 0x0a},
	{0x3fbc, 0x00},
	{0x3c06, 0x00},
	{0x3c07, 0x48},
	{0x3c0a, 0x00},
	{0x3c0b, 0x00},
	{0x3f78, 0x00},
	{0x3f79, 0x40},
	{0x3f7c, 0x00},
	{0x3f7d, 0x00},
	/* default exposure / gain */
	{0x0202, 0x03}, {0x0203, 0xe8},
	{0x0204, 0x00}, {0x0205, 0x00},
	{0x020e, 0x01}, {0x020f, 0x00},
};

static struct regval_list sensor_fmt_raw[] = {

};


/*
 * IMX519 exposure and gain.
 *
 * The VIN/ISP code works in 1/16 units: exp_val = exposure in lines x 16, gain_val = gain x 16.
 *
 * Exposure: the coarse integration time register (0x0202/3) is only 16 bits and the frame length register (0x0340/1)
 * is limited to 0xffdc lines, i.e. about 2 s in the 16 MP mode. For longer exposures the sensor has a shift register
 * (0x3100, 0..7): the frame length and exposure registers are then in units of (1 << shift) lines. The frame length
 * is always made at least exposure + 32 lines so the sensor never clips the exposure.
 *
 * Gain: analog gain code = 1024 - 1024 / G (G up to 16x, code 960), above 16x the remaining gain goes to the
 * digital gain register (8.8 fixed point, 0x0100 = 1.0).
 */
#define IMX519_FRAME_LENGTH_MAX		0xffdc
#define IMX519_LONG_EXP_SHIFT_MAX	7
#define IMX519_EXP_OFFSET		32
#define IMX519_EXP_MIN_LINES		20
#define IMX519_GAIN_MAX_X16		(64 * 16)

static int sensor_g_exp(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);

	*value = info->exp;
	sensor_dbg("sensor_get_exposure = %d\n", info->exp);
	return 0;
}

static int sensor_g_gain(struct v4l2_subdev *sd, __s32 *value)
{
	struct sensor_info *info = to_state(sd);

	*value = info->gain;
	sensor_dbg("sensor_get_gain = %d\n", info->gain);
	return 0;
}

/* exp_val: lines x 16.  Writes shift, frame length and exposure. Caller holds the register group. */
static void imx519_write_exposure(struct v4l2_subdev *sd, unsigned int exp_val)
{
	unsigned int lines = (exp_val + 8) >> 4;
	unsigned int frame, coarse, shift = 0;

	if (lines < IMX519_EXP_MIN_LINES)
		lines = IMX519_EXP_MIN_LINES;
	frame = lines + IMX519_EXP_OFFSET;
	if (frame < (unsigned int)imx519_mipi_sensor_vts)
		frame = imx519_mipi_sensor_vts;
	while (frame > IMX519_FRAME_LENGTH_MAX && shift < IMX519_LONG_EXP_SHIFT_MAX) {
		shift++;
		frame >>= 1;
	}
	if (frame > IMX519_FRAME_LENGTH_MAX)
		frame = IMX519_FRAME_LENGTH_MAX;
	coarse = lines >> shift;
	if (coarse < 1)
		coarse = 1;

	sensor_write(sd, 0x3100, shift);
	sensor_write(sd, 0x0340, (frame >> 8) & 0xff);
	sensor_write(sd, 0x0341, frame & 0xff);
	sensor_write(sd, 0x0202, (coarse >> 8) & 0xff);
	sensor_write(sd, 0x0203, coarse & 0xff);
}

/* gain_val: gain x 16 */
static void imx519_write_gain(struct v4l2_subdev *sd, int gain_val)
{
	unsigned int ana, dig;

	if (gain_val < 16)
		gain_val = 16;
	if (gain_val > IMX519_GAIN_MAX_X16)
		gain_val = IMX519_GAIN_MAX_X16;
	if (gain_val <= 16 * 16) {
		ana = 1024 - 16384 / gain_val;
		dig = 0x0100;
	} else {
		ana = 960;
		dig = gain_val;		/* (gain / 16 / 16) in 8.8 fixed point == gain_val */
	}
	if (dig > 0xffff)
		dig = 0xffff;
	sensor_write(sd, 0x0204, (ana >> 8) & 0xff);
	sensor_write(sd, 0x0205, ana & 0xff);
	sensor_write(sd, 0x020e, (dig >> 8) & 0xff);
	sensor_write(sd, 0x020f, dig & 0xff);
}

static int sensor_s_exp(struct v4l2_subdev *sd, unsigned int exp_val)
{
	struct sensor_info *info = to_state(sd);

	sensor_write(sd, 0x0104, 0x01);		/* group hold: apply all at the next frame boundary */
	imx519_write_exposure(sd, exp_val);
	sensor_write(sd, 0x0104, 0x00);
	info->exp = exp_val;
	return 0;
}

static int sensor_s_gain(struct v4l2_subdev *sd, int gain_val)
{
	struct sensor_info *info = to_state(sd);

	sensor_write(sd, 0x0104, 0x01);
	imx519_write_gain(sd, gain_val);
	sensor_write(sd, 0x0104, 0x00);
	info->gain = gain_val;
	return 0;
}

static int sensor_s_exp_gain(struct v4l2_subdev *sd, struct sensor_exp_gain *exp_gain)
{
	struct sensor_info *info = to_state(sd);
	int exp_val = exp_gain->exp_val, gain_val = exp_gain->gain_val;

	if (exp_val < 0)
		exp_val = 0;
	if (exp_val > 0x7ffffff)
		exp_val = 0x7ffffff;
	if (gain_val < 16)
		gain_val = 16;
	if (gain_val > IMX519_GAIN_MAX_X16)
		gain_val = IMX519_GAIN_MAX_X16;

	sensor_write(sd, 0x0104, 0x01);
	imx519_write_exposure(sd, exp_val);
	imx519_write_gain(sd, gain_val);
	sensor_write(sd, 0x0104, 0x00);

	sensor_dbg("sensor_set_exp_gain exp = %d, gain = %d Done!\n", exp_val, gain_val);
	info->exp = exp_val;
	info->gain = gain_val;
	return 0;
}

/*
static int sensor_s_sw_stby(struct v4l2_subdev *sd, int on_off)
{
	int ret;
	data_type rdval;

	ret = sensor_read(sd, 0x0100, &rdval);
	if (ret != 0)
		return ret;

	if (on_off == CSI_GPIO_LOW)
		ret = sensor_write(sd, 0x0100, rdval & 0xfe);
	else
		ret = sensor_write(sd, 0x0100, rdval | 0x01);
	return ret;
}
*/
/*
 * Stuff that knows about the sensor.
 */

static int sensor_power(struct v4l2_subdev *sd, int on)
{
	int ret;

	ret = 0;
	switch (on) {
	case STBY_ON:
		sensor_print("STBY_ON!\n");
		cci_lock(sd);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		cci_unlock(sd);
		vin_set_mclk(sd, OFF);
		break;
	case STBY_OFF:
		sensor_print("STBY_OFF!\n");
		cci_lock(sd);
		vin_set_mclk_freq(sd, MCLK);
		vin_set_mclk(sd, ON);
		msleep(20);
		cci_unlock(sd);
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);
		break;
	case PWR_ON:
		sensor_print("PWR_ON! (power sequence runs)\n");
		cci_lock(sd);
		vin_gpio_set_status(sd, PWDN, 1);
		vin_gpio_set_status(sd, RESET, 1);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		usleep_range(1000, 1200);
		{ int r1 = vin_set_mclk_freq(sd, MCLK); int r2 = vin_set_mclk(sd, ON);
		  sensor_print("PWR_ON: set_mclk_freq=%d set_mclk(on)=%d\n", r1, r2); }
		usleep_range(10000, 12000);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_HIGH);
		vin_set_pmu_channel(sd, IOVDD, ON);
		vin_set_pmu_channel(sd, AVDD, ON);
		vin_set_pmu_channel(sd, DVDD, ON);
		vin_set_pmu_channel(sd, AFVDD, ON);
		usleep_range(10000, 12000);
		vin_gpio_write(sd, PWDN, CSI_GPIO_HIGH);
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);
		usleep_range(30000, 31000);
		cci_unlock(sd);
		break;
	case PWR_OFF:
		sensor_print("PWR_OFF!\n");
		cci_lock(sd);
		vin_set_mclk(sd, OFF);
		vin_gpio_write(sd, POWER_EN, CSI_GPIO_LOW);
		vin_set_pmu_channel(sd, AFVDD, OFF);
		vin_set_pmu_channel(sd, DVDD, OFF);
		vin_set_pmu_channel(sd, AVDD, OFF);
		vin_set_pmu_channel(sd, IOVDD, OFF);
		usleep_range(10000, 12000);
		vin_gpio_write(sd, PWDN, CSI_GPIO_LOW);
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);

		vin_gpio_set_status(sd, RESET, 0);
		vin_gpio_set_status(sd, PWDN, 0);
		cci_unlock(sd);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sensor_reset(struct v4l2_subdev *sd, u32 val)
{
	switch (val) {
	case 0:
		vin_gpio_write(sd, RESET, CSI_GPIO_HIGH);
		usleep_range(10000, 12000);
		break;
	case 1:
		vin_gpio_write(sd, RESET, CSI_GPIO_LOW);
		usleep_range(10000, 12000);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
 * The IMX519 model ID (0x0519) must be read as one two-byte transfer starting at 0x0016: reading the low byte
 * on its own returns 0, and the BSP's sensor_read() reads one byte at a time. So do the transfer directly.
 */
static int sensor_detect(struct v4l2_subdev *sd)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	u8 reg[2] = { 0x00, 0x16 }, id[2] = { 0, 0 };
	struct i2c_msg msgs[2];
	int ret, i;

	if (!client)
		return -ENODEV;
	for (i = 0; i < 3; i++) {
		msgs[0].addr = client->addr;
		msgs[0].flags = 0;
		msgs[0].len = 2;
		msgs[0].buf = reg;
		msgs[1].addr = client->addr;
		msgs[1].flags = I2C_M_RD;
		msgs[1].len = 2;
		msgs[1].buf = id;
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret == 2)
			break;
		usleep_range(5000, 6000);
	}
	if (ret != 2) {
		sensor_err("%s: i2c read of the chip id failed (%d)\n", __func__, ret);
		return -ENODEV;
	}
	sensor_print("%s: chip id register = 0x%02x%02x\n", __func__, id[0], id[1]);
	if ((((u16)id[0] << 8) | id[1]) != 0x0519)
		return -ENODEV;
	sensor_print("find the sony IMX519 ***********\n");
	return 0;
}

static int sensor_init(struct v4l2_subdev *sd, u32 val)
{
	int ret;
	struct sensor_info *info = to_state(sd);

	sensor_dbg("sensor_init\n");

	/* Make sure it is a target sensor */
	ret = sensor_detect(sd);
	if (ret) {
		sensor_err("chip found is not an target chip.\n");
		return ret;
	}

	info->focus_status = 0;
	info->low_speed = 0;
	info->width = 4656;
	info->height = 3496;
	info->hflip = 0;
	info->vflip = 0;
	info->gain = 0;

	info->tpf.numerator = 1;
	info->tpf.denominator = 10;	/* about 10 fps in the 16 MP mode */

	info->preview_first_flag = 1;

	return 0;
}

static long sensor_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	int ret = 0;
	struct sensor_info *info = to_state(sd);

	switch (cmd) {
	case GET_CURRENT_WIN_CFG:
		if (info->current_wins != NULL) {
			memcpy(arg,
			       info->current_wins,
			       sizeof(*info->current_wins));
			ret = 0;
		} else {
			sensor_err("empty wins!\n");
			ret = -1;
		}
		break;
	case SET_FPS:
		break;
	case VIDIOC_VIN_SENSOR_EXP_GAIN:
		sensor_s_exp_gain(sd, (struct sensor_exp_gain *)arg);
		break;
	case VIDIOC_VIN_SENSOR_CFG_REQ:
		to_state(sd)->combo_mode = IMX519_COMBO_MODE();
		sensor_cfg_req(sd, (struct sensor_config *)arg);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

/*
 * Store information about the video data format.
 */
static struct sensor_format_struct sensor_formats[] = {
	{
		.desc = "Raw RGB Bayer",
		.mbus_code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.regs = sensor_fmt_raw,
		.regs_size = ARRAY_SIZE(sensor_fmt_raw),
		.bpp = 1
	},
};
#define N_FMTS ARRAY_SIZE(sensor_formats)

/*
 * Then there is the issue of window sizes.  Try to capture the info here.
 */

static struct sensor_win_size sensor_win_sizes[] = {
	/* 4656*3496 */
	{
	 .width = 4656,
	 .height = 3496,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 12650,
	 .vts = 3572,
	 .pclk = 426666667,
	 .mipi_bps = 816000000,
	 .fps_fixed = 9,
	 .bin_factor = 1,
	 .intg_min = 20 << 4,
	 .intg_max = 505928 << 4,	/* about 15 s, using the long-exposure shift */
	 .gain_min = 1 << 4,
	 .gain_max = 64 << 4,
	 .regs = sensor_full_regs,
	 .regs_size = ARRAY_SIZE(sensor_full_regs),
	 .set_size = NULL,
	 },
	/* 3840*2160 */
	{
	 .width = 3840,
	 .height = 2160,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 10486,
	 .vts = 2260,
	 .pclk = 426666667,
	 .mipi_bps = 816000000,
	 .fps_fixed = 18,
	 .bin_factor = 1,
	 .intg_min = 20 << 4,
	 .intg_max = 610337 << 4,	/* about 15 s, using the long-exposure shift */
	 .gain_min = 1 << 4,
	 .gain_max = 64 << 4,
	 .regs = sensor_4k_regs,
	 .regs_size = ARRAY_SIZE(sensor_4k_regs),
	 .set_size = NULL,
	 },
	/* 2328*1748 */
	{
	 .width = 2328,
	 .height = 1748,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 6512,
	 .vts = 2184,
	 .pclk = 426666667,
	 .mipi_bps = 816000000,
	 .fps_fixed = 30,
	 .bin_factor = 2,
	 .intg_min = 20 << 4,
	 .intg_max = 982800 << 4,	/* about 15 s, using the long-exposure shift */
	 .gain_min = 1 << 4,
	 .gain_max = 64 << 4,
	 .regs = sensor_bin_regs,
	 .regs_size = ARRAY_SIZE(sensor_bin_regs),
	 .set_size = NULL,
	 },
	/* 1920*1080 */
	{
	 .width = 1920,
	 .height = 1080,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 6027,
	 .vts = 1180,
	 .pclk = 426666667,
	 .mipi_bps = 816000000,
	 .fps_fixed = 60,
	 .bin_factor = 2,
	 .intg_min = 20 << 4,
	 .intg_max = 1061888 << 4,	/* about 15 s, using the long-exposure shift */
	 .gain_min = 1 << 4,
	 .gain_max = 64 << 4,
	 .regs = sensor_1080p_regs,
	 .regs_size = ARRAY_SIZE(sensor_1080p_regs),
	 .set_size = NULL,
	 },
	/* 1280*720 */
	{
	 .width = 1280,
	 .height = 720,
	 .hoffset = 0,
	 .voffset = 0,
	 .hts = 6144,
	 .vts = 820,
	 .pclk = 426666667,
	 .mipi_bps = 816000000,
	 .fps_fixed = 85,
	 .bin_factor = 2,
	 .intg_min = 20 << 4,
	 .intg_max = 1041666 << 4,	/* about 15 s, using the long-exposure shift */
	 .gain_min = 1 << 4,
	 .gain_max = 64 << 4,
	 .regs = sensor_720p_regs,
	 .regs_size = ARRAY_SIZE(sensor_720p_regs),
	 .set_size = NULL,
	 },
};

#define N_WIN_SIZES (ARRAY_SIZE(sensor_win_sizes))

static int sensor_reg_init(struct sensor_info *info)
{

	int ret = 0;
	struct v4l2_subdev *sd = &info->sd;
	struct sensor_format_struct *sensor_fmt = info->fmt;
	struct sensor_win_size *wsize = info->current_wins;

	/* the module only accepts register writes some time after power-up */
	msleep(init_delay_ms);

	ret = sensor_write_array(sd, sensor_default_regs,
			       ARRAY_SIZE(sensor_default_regs));
	if (ret < 0) {
		sensor_err("write sensor_default_regs error\n");
		return ret;
	}

	ret = sensor_write_array(sd, sensor_fmt->regs, sensor_fmt->regs_size);
	sensor_print("fmt regs (%d entries) ret=%d\n", sensor_fmt->regs_size, ret);

	if (wsize->regs) {
		ret = sensor_write_array(sd, wsize->regs, wsize->regs_size);
		sensor_print("mode regs %dx%d (%d entries) ret=%d\n", wsize->width, wsize->height, wsize->regs_size, ret);
	}

	if (wsize->set_size)
		wsize->set_size(sd);

	/* the frame length of THIS mode must be known before any exposure is written */
	imx519_mipi_sensor_vts = imx519_user_vblank ? imx519_user_vblank + wsize->height : wsize->vts;
	sensor_write(sd, 0x0340, (imx519_mipi_sensor_vts >> 8) & 0xff);	/* frame length, also without an exposure value */
	sensor_write(sd, 0x0341, imx519_mipi_sensor_vts & 0xff);

	/* module parameters: defaults for programs that do not use the V4L2 controls */
	if (exp_us > 0 || gain_x16 > 0) {
		sensor_write(sd, 0x0104, 0x01);
		if (exp_us > 0) {
			u64 ln = (u64)exp_us * (u64)wsize->pclk;

			do_div(ln, (u32)wsize->hts);
			do_div(ln, 1000000U);
			imx519_write_exposure(sd, (unsigned int)ln << 4);
		}
		if (gain_x16 > 0)
			imx519_write_gain(sd, gain_x16);
		sensor_write(sd, 0x0104, 0x00);
		sensor_print("applied exp_us=%d gain_x16=%d\n", exp_us, gain_x16);
	}

	/* values set through the standard V4L2 controls take priority over the module parameters */
	if (info->exp > 0) {
		sensor_write(sd, 0x0104, 0x01);
		imx519_write_exposure(sd, info->exp);
		sensor_write(sd, 0x0104, 0x00);
	}
	if (info->gain > 0) {
		sensor_write(sd, 0x0104, 0x01);
		imx519_write_gain(sd, info->gain);
		sensor_write(sd, 0x0104, 0x00);
	}

	if (imx519_test_pattern) {
		sensor_write(sd, 0x0600, 0x00);
		sensor_write(sd, 0x0601, imx519_test_pattern);
	}

	/* start streaming last, so the first frame already uses the chosen exposure and gain */
	if (stream_delay_ms > 0)
		msleep(stream_delay_ms);
	sensor_write(sd, 0x0100, 0x01);

	info->width = wsize->width;
	info->height = wsize->height;

	sensor_print("s_fmt set width = %d, height = %d\n", wsize->width,
		      wsize->height);

	return 0;
}

static int sensor_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sensor_info *info = to_state(sd);

	sensor_print("%s on = %d, %d*%d %x\n", __func__, enable,
		  info->current_wins->width,
		  info->current_wins->height, info->fmt->mbus_code);

	if (!enable)
		return 0;
	return sensor_reg_init(info);
}

static int sensor_g_mbus_config(struct v4l2_subdev *sd, unsigned int pad,
				struct v4l2_mbus_config *cfg)
{
	cfg->type = V4L2_MBUS_CSI2_DPHY;
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	cfg->flags = 0 | V4L2_MBUS_CSI2_2_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#else
	cfg->bus.mipi_csi2.num_data_lanes = 0 | V4L2_MBUS_CSI2_2_LANE | V4L2_MBUS_CSI2_CHANNEL_0;
#endif

	return 0;
}

/*
 * Standard sensor controls used by libcamera (PIXEL_RATE, HBLANK, VBLANK, ANALOGUE_GAIN) next to the Allwinner ones
 * (exposure in lines x 16, gain x 16). Their ranges follow the current mode.
 */
static void imx519_update_blank_ctrls(struct sensor_info *info)
{
	struct sensor_win_size *w = info->current_wins;
	s64 hblank, vblank_min, vblank_max;

	if (!w || !imx519_vblank_ctrl)
		return;
	hblank = w->hts - w->width;
	vblank_min = w->vts - w->height;
	vblank_max = IMX519_FRAME_LENGTH_MAX - w->height;
	imx519_in_update = 1;
	__v4l2_ctrl_modify_range(imx519_pixel_rate_ctrl, w->pclk, w->pclk, 1, w->pclk);
	__v4l2_ctrl_s_ctrl_int64(imx519_pixel_rate_ctrl, w->pclk);
	__v4l2_ctrl_modify_range(imx519_hblank_ctrl, hblank, hblank, 1, hblank);
	__v4l2_ctrl_s_ctrl(imx519_hblank_ctrl, hblank);
	__v4l2_ctrl_modify_range(imx519_vblank_ctrl, vblank_min, vblank_max, 1, vblank_min);
	__v4l2_ctrl_s_ctrl(imx519_vblank_ctrl, vblank_min);
	imx519_in_update = 0;
}

static int imx519_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state, struct v4l2_subdev_format *fmt)
{
	struct sensor_info *info = to_state(sd);
	struct sensor_win_size *before = info->current_wins;
	int ret = sensor_set_fmt(sd, state, fmt);

	if (!ret && fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		if (info->current_wins != before)
			imx519_user_vblank = 0;			/* a new mode starts with its own frame length */
		v4l2_ctrl_lock(imx519_vblank_ctrl);
		imx519_update_blank_ctrls(info);
		v4l2_ctrl_unlock(imx519_vblank_ctrl);
	}
	return ret;
}

static int sensor_g_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_g_gain(sd, &ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_g_exp(sd, &ctrl->val);
	}
	return -EINVAL;
}

static int sensor_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sensor_info *info =
			container_of(ctrl->handler, struct sensor_info, handler);
	struct v4l2_subdev *sd = &info->sd;

	switch (ctrl->id) {
	case V4L2_CID_GAIN:
		return sensor_s_gain(sd, ctrl->val);
	case V4L2_CID_EXPOSURE:
		return sensor_s_exp(sd, ctrl->val);
	case V4L2_CID_PIXEL_RATE:
	case V4L2_CID_HBLANK:
		return 0;				/* read-only: fixed by the mode */
	case V4L2_CID_VBLANK: {
		unsigned int frame = ctrl->val + info->current_wins->height;

		if (imx519_in_update)
			return 0;

		imx519_user_vblank = ctrl->val == (info->current_wins->vts - info->current_wins->height) ? 0 : ctrl->val;
		imx519_mipi_sensor_vts = frame;
		sensor_write(sd, 0x0104, 0x01);
		if (info->exp > 0) {
			imx519_write_exposure(sd, info->exp);	/* re-applies the frame length (and the long-exposure shift) */
		} else {
			sensor_write(sd, 0x0340, (frame >> 8) & 0xff);
			sensor_write(sd, 0x0341, frame & 0xff);
		}
		sensor_write(sd, 0x0104, 0x00);
		return 0;
	}
	case V4L2_CID_ANALOGUE_GAIN:
		sensor_write(sd, 0x0104, 0x01);
		sensor_write(sd, 0x0204, (ctrl->val >> 8) & 0xff);
		sensor_write(sd, 0x0205, ctrl->val & 0xff);
		sensor_write(sd, 0x020e, 0x01);		/* digital gain 1.0 */
		sensor_write(sd, 0x020f, 0x00);
		sensor_write(sd, 0x0104, 0x00);
		info->gain = (16 * 1024) / (1024 - ctrl->val);
		return 0;
	case V4L2_CID_TEST_PATTERN:
		imx519_test_pattern = ctrl->val;
		sensor_write(sd, 0x0600, 0x00);
		sensor_write(sd, 0x0601, ctrl->val);
		return 0;
	}
	return -EINVAL;
}

/* ----------------------------------------------------------------------- */

static const struct v4l2_ctrl_ops sensor_ctrl_ops = {
	.g_volatile_ctrl = sensor_g_ctrl,
	.s_ctrl = sensor_s_ctrl,
};

static const struct v4l2_subdev_core_ops sensor_core_ops = {
	.reset = sensor_reset,
	.init = sensor_init,
	.s_power = sensor_power,
	.ioctl = sensor_ioctl,
#if IS_ENABLED(CONFIG_COMPAT)
	.compat_ioctl32 = sensor_compat_ioctl32,
#endif
};

static const struct v4l2_subdev_video_ops sensor_video_ops = {
	.s_stream = sensor_s_stream,
};

static const struct v4l2_subdev_pad_ops sensor_pad_ops = {
	.enum_mbus_code = sensor_enum_mbus_code,
	.enum_frame_size = sensor_enum_frame_size,
	.enum_frame_interval = sensor_enum_frame_interval,
	.get_fmt = sensor_get_fmt,
	.set_fmt = imx519_set_fmt,
	.get_mbus_config = sensor_g_mbus_config,
};

static const struct v4l2_subdev_ops sensor_ops = {
	.core = &sensor_core_ops,
	.video = &sensor_video_ops,
	.pad = &sensor_pad_ops,
};

/* ----------------------------------------------------------------------- */
static struct cci_driver cci_drv = {
	.name = SENSOR_NAME,
	.addr_width = CCI_BITS_16,
	.data_width = CCI_BITS_8,
};

static const struct v4l2_ctrl_config sensor_custom_ctrls[] = {
	{
		.ops = &sensor_ctrl_ops,
		.id = V4L2_CID_FRAME_RATE,
		.name = "frame rate",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 15,
		.max = 120,
		.step = 1,
		.def = 120,
	},
};

static const char * const imx519_test_pattern_menu[] = {
	"Disabled", "Solid Color", "100% Color Bars", "Fade to Grey Color Bars", "PN9",
};

static int sensor_init_controls(struct v4l2_subdev *sd, const struct v4l2_ctrl_ops *ops)
{
	struct sensor_info *info = to_state(sd);
	struct v4l2_ctrl_handler *handler = &info->handler;
	struct v4l2_ctrl *ctrl;
	int i;
	int ret = 0;

	v4l2_ctrl_handler_init(handler, 7 + ARRAY_SIZE(sensor_custom_ctrls));

	ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_EXPOSURE, 0,
			      0x7ffffff, 1, 0);		/* about 8 million lines: long exposures */
	(void)ctrl;	/* not volatile: the value last set is the value reported */
	v4l2_ctrl_new_std(handler, ops, V4L2_CID_GAIN, 16,
			      IMX519_GAIN_MAX_X16, 1, 16);		/* gain x16: 16 = 1x ... 1024 = 64x */
	{
		struct sensor_win_size *w = &sensor_win_sizes[0];

		imx519_pixel_rate_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_PIXEL_RATE, w->pclk, w->pclk, 1, w->pclk);
		imx519_hblank_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_HBLANK, w->hts - w->width, w->hts - w->width, 1, w->hts - w->width);
		imx519_vblank_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_VBLANK, w->vts - w->height,
						       IMX519_FRAME_LENGTH_MAX - w->height, 1, w->vts - w->height);
		imx519_again_ctrl = v4l2_ctrl_new_std(handler, ops, V4L2_CID_ANALOGUE_GAIN, 0, 960, 1, 0);	/* register code: gain = 1024 / (1024 - code) */
		if (imx519_pixel_rate_ctrl)
			imx519_pixel_rate_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
		if (imx519_hblank_ctrl)
			imx519_hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;
	}
	v4l2_ctrl_new_std_menu_items(handler, ops, V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx519_test_pattern_menu) - 1, 0, 0,
				     imx519_test_pattern_menu);
	for (i = 0; i < ARRAY_SIZE(sensor_custom_ctrls); i++)
		v4l2_ctrl_new_custom(handler, &sensor_custom_ctrls[i], NULL);

	if (handler->error) {
		ret = handler->error;
		v4l2_ctrl_handler_free(handler);
	}

	sd->ctrl_handler = handler;

	return ret;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 6, 0))
static int sensor_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#else
static int sensor_probe(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;
	struct sensor_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL)
		return -ENOMEM;
	sd = &info->sd;
	cci_dev_probe_helper(sd, client, &sensor_ops, &cci_drv);
	sensor_init_controls(sd, &sensor_ctrl_ops);
	mutex_init(&info->lock);

#if IS_ENABLED(CONFIG_SAME_I2C)
	info->sensor_i2c_addr = I2C_ADDR >> 1;
#endif
	info->fmt = &sensor_formats[0];
	info->fmt_pt = &sensor_formats[0];
	info->win_pt = &sensor_win_sizes[0];
	info->time_hs  = 0x25;
	info->fmt_num = N_FMTS;
	info->win_size_num = N_WIN_SIZES;
	info->sensor_field = V4L2_FIELD_NONE;
	info->af_first_flag = 1;
	info->combo_mode = IMX519_COMBO_MODE();

	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static int sensor_remove(struct i2c_client *client)
#else
static void sensor_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd;

	sd = cci_dev_remove_helper(client, &cci_drv);
	kfree(to_state(sd));
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
	return 0;
#endif
}

static const struct i2c_device_id sensor_id[] = {
	{SENSOR_NAME, 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_driver = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = SENSOR_NAME,
		   },
	.probe = sensor_probe,
	.remove = sensor_remove,
	.id_table = sensor_id,
};
static __init int init_sensor(void)
{
	return cci_dev_init_helper(&sensor_driver);
}

static __exit void exit_sensor(void)
{
	cci_dev_exit_helper(&sensor_driver);
}

VIN_INIT_DRIVERS(init_sensor);
module_exit(exit_sensor);
