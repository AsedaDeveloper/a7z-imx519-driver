// a7zcap: capture frames through the Allwinner A733 hardware ISP (libAWIspApi) on the vendor kernel.
// Sequence copied from Radxa's AWISPdemo: DMA_MERGE, S_INPUT, S_PARM, ISP_CFG, S_FMT, REQBUFS, QBUF,
// ispGetIspId/ispStart, STREAMON, DQBUF/QBUF loop.
// build: gcc -O2 -o a7zcap a7zcap.c -lAWIspApi -lisp -lpthread
// usage: a7zcap WIDTH HEIGHT NFRAMES OUTFILE_PREFIX [KEEP_LAST=1]
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include "sunxi_camera_v2.h"
#include "AWIspApi.h"

#ifndef NBUF
#define NBUF 4
#endif
#define NPL 3

static int xioctl(int fd, unsigned long req, void *arg, const char *name)
{
	int r = ioctl(fd, req, arg);
	if (r < 0) fprintf(stderr, "[a7zcap] %s failed: %s\n", name, strerror(errno));
	return r;
}


// --- A7Z_DUMP_PARAM: find the ISP's live parameter struct in our own memory (test+3a+tuning+iso = 116284 bytes)
static void dump_live_param(void)
{
	static const char *vars[] = { "ov13850_mipi", "ov13850_mipi_video", "ov13850_mipi_capture" };
	for (int v = 0; v < 3; v++) {
		char fn[128]; snprintf(fn, sizeof fn, "/tmp/cand_%s.bin", vars[v]);
		FILE *f = fopen(fn, "rb"); if (!f) continue;
		static unsigned char cand[116284]; if (fread(cand, 1, sizeof cand, f) != sizeof cand) { fclose(f); continue; } fclose(f);
		FILE *m = fopen("/proc/self/maps", "r"); char line[512];
		while (fgets(line, sizeof line, m)) {
			unsigned long a, e; char perm[8];
			if (sscanf(line, "%lx-%lx %7s", &a, &e, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
			unsigned char *base = (unsigned char *)a; size_t len = e - a;
			// 3a block head [8..40) of candidate, at offset 124
			unsigned char *hit = memmem(base, len, cand + 124 + 8, 32);
			while (hit) {
				unsigned char *st = hit - 8 - 124;
				if (st >= base && st + 116284 <= base + len) {
					int same = memcmp(st, cand, 116284) == 0;
					int tun = memcmp(st + 124 + 2724 + 8, cand + 124 + 2724 + 8, 32) == 0;
					fprintf(stderr, "[a7zcap] param match %s at %p (3a hit %p, tuning head match=%d, whole-identical=%d) map: %s", vars[v], st, hit, tun, same, line);
					char out[128]; snprintf(out, sizeof out, "/tmp/live_param_%s_%p.bin", vars[v], st);
					FILE *o = fopen(out, "wb"); if (o) { fwrite(st, 1, 116284, o); fclose(o); }
				}
				size_t rem = (base + len) - (hit + 1);
				hit = rem >= 32 ? memmem(hit + 1, rem, cand + 124 + 8, 32) : NULL;
			}
		}
		fclose(m);
	}
}



// ---- lens ownership: when A7Z_AF is set, libisp's lens commands are swallowed so only our own autofocus moves the lens ----
static volatile int g_block_isp_lens = 0;
static volatile int g_in_af = 0;
static int g_af_expdiv = 1, g_af_gainmax = 256;   // A7Z_AF_EXPDIV / A7Z_AF_GAINMAX: exposure cap during autofocus
static __thread int g_my_lens_call = 0;
int ioctl(int fd, unsigned long req, ...)
{
	static int (*real_ioctl)(int, unsigned long, ...) = NULL;
	if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
	va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
	if (g_in_af && getenv("A7Z_IOCTL_LOG") && req != VIDIOC_DQBUF && req != VIDIOC_QBUF) { unsigned *u = (unsigned *)arg; fprintf(stderr, "[a7zcap] IOCTL fd=%d req=%lx a=%08x %08x %08x %08x\n", fd, req, arg ? u[0] : 0, arg ? u[1] : 0, arg ? u[2] : 0, arg ? u[3] : 0); }
	if (req == VIDIOC_VIN_SENSOR_EXP_GAIN && arg) {
		struct sensor_exp_gain *eg = (struct sensor_exp_gain *)arg;
		if (g_af_expdiv > 1 && g_in_af && eg->gain_val > 0) {    // autofocus only: shorter exposure (faster frames), gain raised to keep the brightness
			int g = eg->gain_val * g_af_expdiv; if (g > g_af_gainmax) g = g_af_gainmax;
			if (g > eg->gain_val) { eg->exp_val = (int)((long long)eg->exp_val * eg->gain_val / g); eg->gain_val = g; }
		}
		if (getenv("A7Z_EXPLOG")) fprintf(stderr, "[a7zcap] EXPGAIN exp=%d gain=%d r=%d b=%d\n", eg->exp_val, eg->gain_val, eg->r_gain, eg->b_gain);
	}
	if (g_block_isp_lens && !g_my_lens_call && (req == VIDIOC_VIN_ACT_SET_CODE || req == VIDIOC_VIN_ACT_INIT)) return 0;
	return real_ioctl(fd, req, arg);
}
static int g_sfd = -1, g_fd = -1, g_np = 3, g_gw = 1920, g_gh = 1080;
static void *g_mp[8][3];
static double g_lens_ms = 0; static int g_lens_calls = 0;
static void set_lens(int code)
{
	if (g_sfd < 0 || getenv("A7Z_AF_NOLENS")) return;
	struct actuator_ctrl ac = { .code = (unsigned)code };
	struct timespec ta, tb; clock_gettime(CLOCK_MONOTONIC, &ta);
	g_my_lens_call = 1; ioctl(g_sfd, VIDIOC_VIN_ACT_SET_CODE, &ac); g_my_lens_call = 0;
	clock_gettime(CLOCK_MONOTONIC, &tb); g_lens_ms += (tb.tv_sec - ta.tv_sec) * 1e3 + (tb.tv_nsec - ta.tv_nsec) * 1e-6; g_lens_calls++;
}

// --- focus sweep: A7Z_SWEEP="start,end,step"  (lens codes), prints sharpness per code
static double sharpness(const unsigned char *y, int stride, int x0, int x1, int y0, int y1)
{
	double acc = 0; long n = 0;
	for (int r = y0; r < y1; r++)
		for (int c = x0; c < x1; c++) {
			int v = 4 * y[r * stride + c] - y[(r - 1) * stride + c] - y[(r + 1) * stride + c] - y[r * stride + c - 1] - y[r * stride + c + 1];
			acc += v < 0 ? -v : v; n++;
		}
	return acc / (double)n;
}


// Run on a big core. The scheduler otherwise puts this process on a little core about half the time, which makes autofocus 3x slower. A7Z_CPU=n overrides (-1 = no pinning).
#include <sched.h>
static void pin_big_core(void)
{
	int cpu = getenv("A7Z_CPU") ? atoi(getenv("A7Z_CPU")) : -1;
	if (!getenv("A7Z_CPU")) {                                           // pick the cores with the highest cpu_capacity
		int best = 0, cap = 0, n = (int)sysconf(_SC_NPROCESSORS_ONLN);
		for (int c = 0; c < n; c++) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", c); FILE *f = fopen(p, "r"); int v = 0; if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); } if (v > cap) { cap = v; best = c; } }
		cpu = cap ? best : -1;
	}
	if (cpu < 0) return;
	cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof set, &set) == 0) fprintf(stderr, "[a7zcap] pinned to cpu %d\n", cpu);
}
static long g_af_frames = 0; static int g_af_trace = 0;
// The capture buffers are uncached DMA memory: reading them pixel by pixel from the CPU is slow (and on a little core it made each AF frame take 100-400 ms, so
// the driver dropped 1 frame in 2 to 1 in 8). Copy the region once with row memcpy, hand the buffer back, then score the cached copy.
static unsigned char *g_roi; static size_t g_roi_cap;
static double grab_sharp(int skip, int avg)
{
	double sh = 0; int got = 0;
	for (int n = 0; n < skip + avg; n++) {
		fd_set fds; FD_ZERO(&fds); FD_SET(g_fd, &fds); struct timeval tv = { 3, 0 };
		if (select(g_fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
		struct v4l2_buffer bb; struct v4l2_plane pp[3]; memset(&bb, 0, sizeof bb); memset(pp, 0, sizeof pp);
		bb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bb.memory = V4L2_MEMORY_MMAP; bb.length = g_np; bb.m.planes = pp;
		if (ioctl(g_fd, VIDIOC_DQBUF, &bb) < 0) break;
		g_af_frames++;
		if (g_af_trace) { struct timespec tq; clock_gettime(CLOCK_MONOTONIC, &tq); fprintf(stderr, "%ld seq%u ts%ld ", (long)(tq.tv_sec % 100 * 1000 + tq.tv_nsec / 1000000), bb.sequence, (long)(bb.timestamp.tv_sec % 100 * 1000 + bb.timestamp.tv_usec / 1000)); }
		int have = n >= skip, x0 = g_gw / 4, x1 = g_gw * 3 / 4, y0 = g_gh / 4, y1 = g_gh * 3 / 4;
		if (have) {
			int w = x1 - x0 + 2, h = y1 - y0 + 2;
			if ((size_t)w * h > g_roi_cap) { g_roi_cap = (size_t)w * h; g_roi = realloc(g_roi, g_roi_cap); }
			for (int r = 0; r < h; r++) memcpy(g_roi + (size_t)r * w, g_mp[bb.index][0] + (size_t)(y0 - 1 + r) * g_gw + (x0 - 1), w);
		}
		ioctl(g_fd, VIDIOC_QBUF, &bb);                                   // buffer back to the driver before any arithmetic
		if (have) { int w = x1 - x0 + 2, h = y1 - y0 + 2; sh += sharpness(g_roi, w, 1, w - 1, 1, h - 1); got++; }
	}
	return got ? sh / got : -1.0;
}
static double g_af_peak = 0; static int g_af_code = 0;
static void af_scan(int lo, int hi, int step)
{
	double best = -1; int bc = lo;
	for (int c = lo; c <= hi; c += step) {
		set_lens(c); double sh = grab_sharp(6, 2);
		if (sh > best) { best = sh; bc = c; }
	}
	g_af_peak = best; g_af_code = bc;
}
static void autofocus_slow(int full)
{
	int c0 = 0, c1 = 3000, st = 150;
	if (!full) { c0 = g_af_code - 400; c1 = g_af_code + 400; st = 50; if (c0 < 0) c0 = 0; if (c1 > 4000) c1 = 4000; }
	af_scan(c0, c1, st);
	int c = g_af_code; af_scan(c - st < 0 ? 0 : c - st, c + st > 4000 ? 4000 : c + st, st / 5 > 5 ? st / 5 : 5);
	set_lens(g_af_code); double v = grab_sharp(6, 4);
	fprintf(stderr, "[a7zcap] AF: best lens code %d, sharpness %.2f\n", g_af_code, v); g_af_peak = v;
}


// ---- faster autofocus (default; A7Z_AF_SLOW=1 keeps the old fixed 0-3000 scan). Follows the contrast-detect scan of Raspberry Pi's libcamera
// (src/ipa/rpi/controller/rpi/af.cpp, doScan/findPeak) and the imx519 tuning file's rpi.af values: work in dioptres (the tuning map is 0 -> code 0,
// 15 dioptre -> code 4095, i.e. ~273 codes per dioptre), start at the far end, step 1.0 dioptre toward near, STOP as soon as the contrast falls below
// 75% of the best seen (the peak has been passed), then take 3 fine samples at 0.25 dioptre around the interpolated peak and fit a parabola.
// A7Z_AF_SKIP = frames discarded after each lens move (default 4; probe: the first 3 frames still show the old focus; the Pi tuning uses 5; run A7Z_AF_PROBE=1 to measure what this pipeline needs).
static int g_af_skip = 3;   // settle frames after a fine-scan lens move (a move shows on the 4th frame; image-scored equal to 4)
static double g_af_dpt = 0.0;
#define AF_CODE_PER_DPT 273.0
#define AF_DPT_MAX 12.0
static int af_code(double dpt) { if (dpt < 0) dpt = 0; if (dpt > AF_DPT_MAX) dpt = AF_DPT_MAX; return (int)(dpt * AF_CODE_PER_DPT + 0.5); }
static double af_at(double dpt) { set_lens(af_code(dpt)); return grab_sharp(g_af_skip, 1); }
// parabola through sample i and its two neighbours (the same-side pair at the ends), clamped between them; falls back to the sample itself
static double af_peak(const double *f, const double *c, int n, int i)
{
	double r = f[i];
	if (n >= 3) {
		if (i == 0) i++; else if (i + 1 >= n) i--;
		double abx = f[i - 1] - f[i], aby = c[i - 1] - c[i], cbx = f[i + 1] - f[i], cby = c[i + 1] - c[i];
		double den = 2.0 * (aby * cbx - cby * abx);
		if (fabs(den) >= 1.0 / 64.0 && den * abx > 0.0) {
			r = (aby * cbx * cbx - cby * abx * abx) / den;
			double lo = abx < cbx ? abx : cbx, hi = abx > cbx ? abx : cbx; if (r < lo) r = lo; if (r > hi) r = hi;
			r += f[i];
		}
	}
	return r;
}
static void autofocus_fast(int full)
{
	struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); long f0 = g_af_frames;
	const double coarse = 1.0, fine = 0.25, ratio = 0.75;
	double fo[40], sh[40]; int n = 0, mi = 0;
	double start = full ? 0.0 : (g_af_dpt - 2.0 < 0 ? 0.0 : g_af_dpt - 2.0);
	for (double d = start; d <= AF_DPT_MAX + 1e-6 && n < 40; d += coarse) {
		fo[n] = d; sh[n] = af_at(d); if (sh[n] > sh[mi]) mi = n; n++;
		if (n >= 2 && sh[n - 1] < ratio * sh[mi]) break;          // peak passed
	}
	double pk = af_peak(fo, sh, n, mi);
	double ff[3], fs[3]; int fi = 0;
	for (int k = 0; k < 3; k++) {                                // fine scan around the coarse peak: pk+fine, pk, pk-fine
		double d = pk + fine - k * fine; if (d < 0) d = 0; if (d > AF_DPT_MAX) d = AF_DPT_MAX;
		ff[k] = d; fs[k] = af_at(d); if (fs[k] > fs[fi]) fi = k;
	}
	double best = af_peak(ff, fs, 3, fi);
	set_lens(af_code(best)); double v = grab_sharp(g_af_skip, 3);
	if (v < 0.9 * fs[fi]) { best = ff[fi]; set_lens(af_code(best)); v = grab_sharp(g_af_skip, 3); }   // interpolated point worse than a measured one
	g_af_dpt = best; g_af_code = af_code(best); g_af_peak = v;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "[a7zcap] AF: best lens code %d (%.2f dioptre), sharpness %.2f (fast: %ld frames, %.1f s)\n", g_af_code, best, v, g_af_frames - f0, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9);
}

// ---- sweep autofocus (default): same dioptre-space contrast search as above but the coarse stage SWEEPS the lens (one step per frame, no settling wait) and
// assigns each frame's sharpness to the lens position commanded g_af_lat frames earlier (measured pipeline latency, see g_af_lat). The peak must be
// prominent (>= 1.4x the weakest sample) and the sharpness must stay below 60% of it for two frames before the sweep stops, so noise bumps in the dim, flat
// far end no longer end it early. Then 3 settled fine samples around the peak and a parabola; no verify capture. A7Z_AF_CDAF=1 -> the libcamera-style stepper above.
static int g_af_nostop = 0;   // set for the retry sweep: no early stop, scan the whole range
static int g_af_lat = 1;   // frames between a lens command and the frame that shows it; measured 1 at the undropped 50 ms frame rate (was 3 when the old CPU-bound loop dropped frames)
static void autofocus_sweep(int full)
{
	struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); long f0 = g_af_frames;
	const double step = getenv("A7Z_AF_STEP") ? atof(getenv("A7Z_AF_STEP")) : 0.75, fine = getenv("A7Z_AF_FINE") ? atof(getenv("A7Z_AF_FINE")) : 0.3;
	double pos[80], val[80], f[80], c[80]; int n = 0, m = 0, mi = 0, below = 0;
	double start = full ? 0.0 : (g_af_dpt - 2.0 < 0 ? 0.0 : g_af_dpt - 2.0);
	set_lens(af_code(start)); grab_sharp(g_af_skip, 1);                       // settle at the start of the sweep
	for (int i = 0; i < 79; i++) {
		double d = start + i * step; if (d > AF_DPT_MAX + step) break;
		pos[i] = d; set_lens(af_code(d)); val[i] = grab_sharp(0, 1); n = i + 1;
		int j = i - g_af_lat;                                                     // this frame shows the lens as commanded at frame j
		if (j < 0) continue;
		f[m] = pos[j]; c[m] = val[i]; if (c[m] > c[mi]) mi = m; m++;
		double lo = c[0]; for (int k = 1; k < m; k++) if (c[k] < lo) lo = c[k];
		if (!g_af_nostop && m >= 4 && c[mi] >= 1.4 * lo && c[m - 1] < 0.6 * c[mi]) { if (++below >= 2) break; } else below = 0;
	}
	if (getenv("A7Z_AF_DEBUG")) { for (int k = 0; k < m; k++) fprintf(stderr, "[a7zcap] sweep %.2f dpt: %.3f\n", f[k], c[k]); }
	double pk = af_peak(f, c, m, mi);
	double ff[3], fs[3]; int fi = 0;
	for (int k = 0; k < 3; k++) {
		double d = pk - fine + k * fine; if (d < 0) d = 0; if (d > AF_DPT_MAX) d = AF_DPT_MAX;
		ff[k] = d; fs[k] = af_at(d); if (fs[k] > fs[fi]) fi = k;
	}
	if (!g_af_nostop && ((mi == 0 && m >= 4) || fs[fi] < 0.8 * c[mi] || getenv("A7Z_AF_FORCE_RETRY"))) {          // suspicious: peak at the start of the sweep, or the settled samples are far below the sweep's best
		fprintf(stderr, "[a7zcap] AF: suspicious result (sweep peak %.2f dpt, settled %.3f vs sweep %.3f), redoing a full sweep\n", f[mi], fs[fi], c[mi]);
		g_af_nostop = 1; autofocus_sweep(1); g_af_nostop = 0; return;
	}
	double best = af_peak(ff, fs, 3, fi);
	set_lens(af_code(best));
	g_af_dpt = best; g_af_code = af_code(best); g_af_peak = fs[fi];
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "[a7zcap] AF: best lens code %d (%.2f dioptre), sharpness %.2f (sweep: %ld frames, %.1f s, sweep peak %.2f dpt, prominence %.1fx, lens ioctl %.0f ms avg over %d)\n",
		g_af_code, best, fs[fi], g_af_frames - f0, (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9, f[mi], c[mi] / (c[0] > 0.01 ? c[0] : 0.01), g_lens_calls ? g_lens_ms / g_lens_calls : 0.0, g_lens_calls);
	(void)n;
}
static void autofocus_dispatch(int full);
static void autofocus(int full) { g_in_af = 1; autofocus_dispatch(full); g_in_af = 0; }
static void autofocus_dispatch(int full) { if (getenv("A7Z_AF_SLOW")) autofocus_slow(full); else if (getenv("A7Z_AF_CDAF")) autofocus_fast(full); else autofocus_sweep(full); }

// A7Z_AF_PROBE=1: how many frames after a lens move until the sharpness settles? Jumps the lens 0 -> 1500 -> 0 and prints sharpness per frame.
static void af_probe(void)
{
	int seq[3] = { 0, 1500, 0 };
	for (int j = 0; j < 3; j++) {
		set_lens(seq[j]); fprintf(stderr, "[a7zcap] PROBE lens -> %d: sharpness per frame:", seq[j]);
		for (int k = 0; k < 14; k++) fprintf(stderr, " %.2f", grab_sharp(0, 1));
		fprintf(stderr, "\n");
	}
}

// ---- lens-shading (flat-field) correction applied to output frames: A7Z_FLAT=/path/flatmap.bin (3 planes R,G,B of 36x64 float gains, linear light) ----
static float *g_flat = NULL;
static float g_dec[256]; static unsigned char g_enc[4097];
static unsigned char *g_fy, *g_fu, *g_fv; static size_t g_fycap, g_fuvcap;
static void load_flat(const char *path)
{
	FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "[a7zcap] cannot open flat map %s\n", path); return; }
	g_flat = malloc(3 * 36 * 64 * sizeof(float));
	if (fread(g_flat, sizeof(float), 3 * 36 * 64, f) != 3 * 36 * 64) { free(g_flat); g_flat = NULL; fprintf(stderr, "[a7zcap] bad flat map\n"); }
	fclose(f);
	for (int i = 0; i < 256; i++) g_dec[i] = powf(i / 255.0f, 2.2f);
	for (int i = 0; i <= 4096; i++) g_enc[i] = (unsigned char)(255.0f * powf(i / 4096.0f, 1.0f / 2.2f) + 0.5f);
	if (g_flat) fprintf(stderr, "[a7zcap] shading correction on (%s)\n", path);
}
static inline void flat_gain(int x, int y, int w, int h, float *g)
{
	float fx = (x + 0.5f) / w * 64.0f - 0.5f, fy = (y + 0.5f) / h * 36.0f - 0.5f;
	if (fx < 0) fx = 0; if (fx > 63) fx = 63; if (fy < 0) fy = 0; if (fy > 35) fy = 35;
	int x0 = (int)fx, y0 = (int)fy; int x1 = x0 < 63 ? x0 + 1 : x0, y1 = y0 < 35 ? y0 + 1 : y0; float ax = fx - x0, ay = fy - y0;
	for (int c = 0; c < 3; c++) {
		const float *m = g_flat + c * 36 * 64;
		float t = m[y0 * 64 + x0] * (1 - ax) + m[y0 * 64 + x1] * ax, b2 = m[y1 * 64 + x0] * (1 - ax) + m[y1 * 64 + x1] * ax;
		g[c] = t * (1 - ay) + b2 * ay;
	}
}
static void apply_flat(const unsigned char *Y, const unsigned char *U, const unsigned char *V, int w, int rows, int vis, size_t ylen, size_t ulen, size_t vlen)
{
	if (ylen > g_fycap) { g_fy = realloc(g_fy, ylen); g_fycap = ylen; }
	if (ulen > g_fuvcap || vlen > g_fuvcap) { size_t m = ulen > vlen ? ulen : vlen; g_fu = realloc(g_fu, m); g_fv = realloc(g_fv, m); g_fuvcap = m; }
	memcpy(g_fy, Y, ylen); memcpy(g_fu, U, ulen); memcpy(g_fv, V, vlen);
	int cw = w / 2;
	for (int by = 0; by < vis / 2; by++)
		for (int bx = 0; bx < cw; bx++) {
			float uu = U[by * cw + bx] - 128.0f, vv = V[by * cw + bx] - 128.0f, su = 0, sv = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					int x = bx * 2 + dx, y = by * 2 + dy; float yy = Y[y * w + x], g[3];
					float r = yy + 1.402f * vv, gg = yy - 0.344f * uu - 0.714f * vv, b = yy + 1.772f * uu;
					r = r < 0 ? 0 : r > 255 ? 255 : r; gg = gg < 0 ? 0 : gg > 255 ? 255 : gg; b = b < 0 ? 0 : b > 255 ? 255 : b;
					flat_gain(x, y, w, vis, g);
					float lr = g_dec[(int)(r + 0.5f)] * g[0], lg = g_dec[(int)(gg + 0.5f)] * g[1], lb = g_dec[(int)(b + 0.5f)] * g[2];
					lr = lr > 1 ? 1 : lr; lg = lg > 1 ? 1 : lg; lb = lb > 1 ? 1 : lb;
					float R = g_enc[(int)(lr * 4096)], Gc = g_enc[(int)(lg * 4096)], B = g_enc[(int)(lb * 4096)];
					float ny = 0.299f * R + 0.587f * Gc + 0.114f * B;
					g_fy[y * w + x] = (unsigned char)(ny + 0.5f);
					su += 0.564f * (B - ny); sv += 0.713f * (R - ny);
				}
			float nu = 128.0f + su * 0.25f, nv = 128.0f + sv * 0.25f;
			g_fu[by * cw + bx] = (unsigned char)(nu < 0 ? 0 : nu > 255 ? 255 : nu + 0.5f);
			g_fv[by * cw + bx] = (unsigned char)(nv < 0 ? 0 : nv > 255 ? 255 : nv + 0.5f);
		}
	(void)rows;
}
static void emit_frame(FILE *f, void *const planes[3], const struct v4l2_plane *pl, size_t ml0, int np, int gw, int gh)
{
	size_t l0 = pl[0].bytesused ? pl[0].bytesused : ml0;
	if (g_flat && np >= 3) {
		size_t l1 = pl[1].bytesused ? pl[1].bytesused : l0 / 4, l2 = pl[2].bytesused ? pl[2].bytesused : l0 / 4;
		apply_flat(planes[0], planes[1], planes[2], gw, (int)(l0 / gw), gh, l0, l1, l2);
		fwrite(g_fy, 1, l0, f); fwrite(g_fu, 1, l1, f); fwrite(g_fv, 1, l2, f);
	} else for (int p = 0; p < np; p++) fwrite(planes[p], 1, pl[p].bytesused ? pl[p].bytesused : (p ? l0 / 4 : l0), f);
}

int main(int argc, char **argv)
{
	pin_big_core();
	if (argc < 5) { fprintf(stderr, "usage: %s W H NFRAMES PREFIX\n", argv[0]); return 2; }
	int W = atoi(argv[1]), H = atoi(argv[2]), N = atoi(argv[3]);
	FILE *outf = stdout;
	if (getenv("A7Z_STDOUT")) { outf = fdopen(dup(1), "wb"); dup2(2, 1); }   // libisp printf()s to stdout: keep frames on a private fd
	const char *prefix = argv[4];
	int fd = open("/dev/video0", O_RDWR);
	if (fd < 0) { perror("open /dev/video0"); return 1; }

	unsigned char merge = 0;
	xioctl(fd, VIDIOC_SET_DMA_MERGE, &merge, "SET_DMA_MERGE");
	int input = 0;
	if (xioctl(fd, VIDIOC_S_INPUT, &input, "S_INPUT")) return 1;

	struct v4l2_streamparm parm;
	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
	parm.parm.capture.reserved[2] = getenv("A7Z_LARGE") ? atoi(getenv("A7Z_LARGE")) : 0;   // 3 = 2-in-1 dual-ISP mode for widths above 3264
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = getenv("A7Z_FPS") ? atoi(getenv("A7Z_FPS")) : 30;   // picks the sensor mode: the driver only considers modes whose fps_fixed matches
	if (xioctl(fd, VIDIOC_S_PARM, &parm, "S_PARM")) return 1;

	struct sensor_isp_cfg icfg = { .isp_wdr_mode = 0, .large_image = getenv("A7Z_LARGE") ? atoi(getenv("A7Z_LARGE")) : 0 };
	xioctl(fd, VIDIOC_SET_SENSOR_ISP_CFG, &icfg, "SET_SENSOR_ISP_CFG");

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = W;
	fmt.fmt.pix_mp.height = H;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt, "S_FMT")) return 1;
	if (xioctl(fd, VIDIOC_G_FMT, &fmt, "G_FMT")) return 1;
	int gw = fmt.fmt.pix_mp.width, gh = fmt.fmt.pix_mp.height, np = fmt.fmt.pix_mp.num_planes;
	fprintf(stderr, "[a7zcap] got %dx%d planes=%d\n", gw, gh, np);
	if (np > NPL) np = NPL;

	struct v4l2_requestbuffers rb;
	memset(&rb, 0, sizeof rb);
	rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &rb, "REQBUFS")) return 1;

	void *mp[NBUF][NPL]; size_t ml[NBUF][NPL];
	struct v4l2_buffer b; struct v4l2_plane pl[NPL];
	for (int i = 0; i < (int)rb.count && i < NBUF; i++) {
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.index = i; b.length = np; b.m.planes = pl;
		if (xioctl(fd, VIDIOC_QUERYBUF, &b, "QUERYBUF")) return 1;
		for (int p = 0; p < np; p++) {
			ml[i][p] = pl[p].length;
			mp[i][p] = mmap(NULL, pl[p].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[p].m.mem_offset);
			if (mp[i][p] == MAP_FAILED) { perror("mmap"); return 1; }
		}
		if (xioctl(fd, VIDIOC_QBUF, &b, "QBUF")) return 1;
	}

	if (getenv("A7Z_FLAT")) load_flat(getenv("A7Z_FLAT"));
	g_fd = fd; g_np = np; g_gw = gw; g_gh = gh;
	for (int i = 0; i < NBUF; i++) for (int p = 0; p < np; p++) g_mp[i][p] = mp[i][p];
	if (getenv("A7Z_AF")) { g_sfd = open("/dev/v4l-subdev0", O_RDWR); g_block_isp_lens = 1; }
	AWIspApi *api = CreateAWIspApi();
	int isp_id = api ? api->ispGetIspId(0) : -1;
	fprintf(stderr, "[a7zcap] isp_id=%d\n", isp_id);
	if (api && isp_id >= 0) {
		int r = api->ispStart(isp_id);
		fprintf(stderr, "[a7zcap] ispStart=%d\n", r);
	}

	if (getenv("A7Z_DUMP_PARAM")) dump_live_param();
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(fd, VIDIOC_STREAMON, &t, "STREAMON")) return 1;



	g_af_trace = getenv("A7Z_AF_TRACE") != NULL;
	if (getenv("A7Z_AF_LAT")) g_af_lat = atoi(getenv("A7Z_AF_LAT"));
	if (getenv("A7Z_AF_EXPDIV")) g_af_expdiv = atoi(getenv("A7Z_AF_EXPDIV"));
	if (getenv("A7Z_AF_GAINMAX")) g_af_gainmax = atoi(getenv("A7Z_AF_GAINMAX"));
	if (getenv("A7Z_AF_SKIP")) g_af_skip = atoi(getenv("A7Z_AF_SKIP"));
	if (getenv("A7Z_AF_PROBE")) af_probe();
	if (getenv("A7Z_AF")) autofocus(1);
	if (getenv("A7Z_FOCUS")) {                       // hold the lens at a fixed code for the whole capture
		int sfd2 = open("/dev/v4l-subdev0", O_RDWR);
		if (sfd2 >= 0) { struct actuator_ctrl ac2 = { .code = (unsigned)atoi(getenv("A7Z_FOCUS")) }; int rc2 = ioctl(sfd2, VIDIOC_VIN_ACT_SET_CODE, &ac2); fprintf(stderr, "[a7zcap] lens code %u rc=%d\n", ac2.code, rc2); close(sfd2); }
	}

	if (getenv("A7Z_HOLD")) {                       // set a lens code, then print sharpness for every following frame
		int hc = atoi(getenv("A7Z_HOLD")); int sfd3 = open("/dev/v4l-subdev0", O_RDWR);
		struct actuator_ctrl ac3 = { .code = (unsigned)hc }; int rc3 = sfd3 >= 0 ? ioctl(sfd3, VIDIOC_VIN_ACT_SET_CODE, &ac3) : -1;
		fprintf(stderr, "HOLD set code %d rc=%d\n", hc, rc3);
		for (int n = 0; n < 90; n++) {
			fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds); struct timeval tv = { 3, 0 };
			if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
			memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.length = np; b.m.planes = pl;
			if (xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF")) break;
			if (n % 5 == 0) fprintf(stderr, "HOLD frame %d sharp=%.3f\n", n, sharpness(mp[b.index][0], gw, gw / 8, gw * 7 / 8, gh / 8, gh * 7 / 8));
			xioctl(fd, VIDIOC_QBUF, &b, "QBUF");
		}
		close(sfd3);
	}
	if (getenv("A7Z_SWEEP")) {
		int c0 = 0, c1 = 4000, cs = 200; sscanf(getenv("A7Z_SWEEP"), "%d,%d,%d", &c0, &c1, &cs);
		int sfd = open("/dev/v4l-subdev0", O_RDWR);
		if (sfd < 0) { perror("open subdev0"); return 1; }
		int settle = getenv("A7Z_SETTLE") ? atoi(getenv("A7Z_SETTLE")) : 8;
		for (int code = c0; code <= c1; code += cs) {
			struct actuator_ctrl ac = { .code = (unsigned)code };
			int rc = ioctl(sfd, VIDIOC_VIN_ACT_SET_CODE, &ac);
			double sh = 0; int got = 0;
			for (int n = 0; n < settle + 3; n++) {
				fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds); struct timeval tv = { 3, 0 };
				if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
				memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
				b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.length = np; b.m.planes = pl;
				if (xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF")) break;
				if (n >= settle) { sh += sharpness(mp[b.index][0], gw, gw / 8, gw * 7 / 8, gh / 8, gh * 7 / 8); got++; }
				xioctl(fd, VIDIOC_QBUF, &b, "QBUF");
			}
			fprintf(stderr, "SWEEP code=%d rc=%d sharp=%.3f\n", code, rc, got ? sh / got : -1.0);
		}
		close(sfd);
	}
	for (int n = 0; n < N; n++) {
		fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
		struct timeval tv = { 3, 0 };
		int s = select(fd + 1, &fds, NULL, NULL, &tv);
		if (s <= 0) { fprintf(stderr, "[a7zcap] select timeout/err at frame %d\n", n); break; }
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP;
		b.length = np; b.m.planes = pl;
		if (xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF")) break;
		int every = getenv("A7Z_EVERY") ? atoi(getenv("A7Z_EVERY")) : 10;
		if (getenv("A7Z_STDOUT")) {            // live stream: raw planar YUV420 frames to stdout, every A7Z_SKIP-th frame
			int skip = getenv("A7Z_SKIP") ? atoi(getenv("A7Z_SKIP")) : 2;
			if (n % skip == 0) {
				emit_frame(outf, mp[b.index], pl, ml[b.index][0], np, gw, gh);
				fflush(outf);
			}
		} else if (n == N - 1 || n % every == every - 1) {
			char name[256];
			snprintf(name, sizeof name, "%s_%d_%dx%d.yuv", prefix, n, gw, gh);
			FILE *f = fopen(name, "wb");
			if (f) {
				emit_frame(f, mp[b.index], pl, ml[b.index][0], np, gw, gh);
				fclose(f);
				fprintf(stderr, "[a7zcap] wrote %s (planes bytesused %u %u %u)\n", name,
					pl[0].bytesused, pl[1].bytesused, pl[2].bytesused);
			}
		}
		if (g_block_isp_lens) {
			static int low = 0; static int since = 0; since++;
			double cur = sharpness(mp[b.index][0], gw, gw / 4, gw * 3 / 4, gh / 4, gh * 3 / 4);
			low = (cur < 0.7 * g_af_peak) ? low + 1 : 0;
			xioctl(fd, VIDIOC_QBUF, &b, "QBUF");
			if (low >= 30 && since > 60) { fprintf(stderr, "[a7zcap] AF: picture went soft (%.2f vs %.2f), refocusing\n", cur, g_af_peak); autofocus(0); low = 0; since = 0; }
			continue;
		}
		xioctl(fd, VIDIOC_QBUF, &b, "QBUF");
	}

	xioctl(fd, VIDIOC_STREAMOFF, &t, "STREAMOFF");
	if (api && isp_id >= 0) { api->ispStop(isp_id); }
	if (api) DestroyAWIspApi(api);
	close(fd);
	fprintf(stderr, "[a7zcap] done\n");
	return 0;
}
