// a7zdual: capture through the A733 "2-in-1" (large image) mode, which lifts the single-ISP limit of 3264 pixels per line.
// The CSI parser splits one sensor stream into two overlapping halves, one per ISP, and both DMAs write into one buffer.
// What makes it work (all three are required):
//   1. A7Z_MERGE=1: SET_DMA_MERGE=1 and open ONLY /dev/video1. The driver forwards S_INPUT/S_FMT/S_PARM to video0 itself
//      (forcing large_image=3) and streams both pipelines. Opening both nodes yourself makes them fight over the media links.
//   2. A7Z_FPS=<fps_fixed of the sensor mode>: the sensor-mode selector only considers modes with that frame rate.
//   3. libisp must be told it is a large image: ispSetIspLargeImage(id) on BOTH ISP instances, before ispStart.
//      Without it the pipeline delivers 1-2 frames and then stalls ("tdm0 rx0 buffer full").
// autofocus in the live preview: A7Z_AF=1 (needs A7Z_STREAM; the preview freezes ~10 s while the first scan runs)
// live preview: A7Z_STREAM=4 [A7Z_STREAM_SKIP=2] ... a7zdual 4656 3496 100000000 x   (frames on stdout, decimated by 4; see emit_decimated)
// usage: A7Z_MERGE=1 A7Z_FPS=9 a7zdual 4656 3496 NFRAMES PREFIX   (writes the NFRAMES-th frame: PREFIX_n0_<n>_<w>x<h>.yuv)
// The file is raw planar 4:2:0 with padded plane heights (e.g. 3504 luma rows for 3496), exactly as bytesused reports.
// A7Z_MERGE=0 keeps the old two-node experiment, which does not work.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <math.h>
#include <linux/videodev2.h>
#include "sunxi_camera_v2.h"
#include "AWIspApi.h"
#define NBUF 4
#define NPL 3
struct node { double t5, tN; int fd, np, w, h; void *mp[NBUF][NPL]; size_t ml[NBUF][NPL]; int frames; };
static int xi(int fd, unsigned long r, void *a, const char *n) { int x = ioctl(fd, r, a); if (x < 0) fprintf(stderr, "[dual] fd%d %s failed: %s\n", fd, n, strerror(errno)); return x; }

// ---- autofocus (A7Z_AF=1 together with A7Z_STREAM): libisp's own AF never focuses on this module, so its lens commands are swallowed
//      (this program exports its own ioctl(); link with --export-dynamic) and the lens is moved by a contrast scan instead.
//      Sharpness is scored on every d-th pixel of the central half of the frame, i.e. on the same decimated picture the preview shows.
static volatile int g_block_isp_lens = 0;
static __thread int g_my_lens_call = 0;
int ioctl(int fd, unsigned long req, ...)
{
	static int (*real_ioctl)(int, unsigned long, ...) = NULL;
	if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
	va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
	if (g_block_isp_lens && !g_my_lens_call && (req == VIDIOC_VIN_ACT_SET_CODE || req == VIDIOC_VIN_ACT_INIT)) return 0;
	return real_ioctl(fd, req, arg);
}
static int g_sfd = -1, g_d = 4; static struct node *g_n;
static double g_af_peak = 0; static int g_af_code = 0;
static void set_lens(int code)
{
	if (g_sfd < 0) return;
	struct actuator_ctrl ac = { .code = (unsigned)code };
	g_my_lens_call = 1; ioctl(g_sfd, VIDIOC_VIN_ACT_SET_CODE, &ac); g_my_lens_call = 0;
}
static double sharp_dec(const unsigned char *y, int stride, int w, int h, int d)
{
	double acc = 0; long n = 0;
	for (int r = h / 4; r < h * 3 / 4; r += d)
		for (int c = w / 4; c < w * 3 / 4; c += d) {
			int v = 4 * y[r * stride + c] - y[(r - d) * stride + c] - y[(r + d) * stride + c] - y[r * stride + c - d] - y[r * stride + c + d];
			acc += v < 0 ? -v : v; n++;
		}
	return acc / (double)n;
}
static double grab_sharp(int skip, int avg)
{
	double sh = 0; int got = 0;
	for (int k = 0; k < skip + avg; k++) {
		fd_set fds; FD_ZERO(&fds); FD_SET(g_n->fd, &fds); struct timeval tv = { 3, 0 };
		if (select(g_n->fd + 1, &fds, NULL, NULL, &tv) <= 0) break;
		struct v4l2_buffer bb; struct v4l2_plane pp[NPL]; memset(&bb, 0, sizeof bb); memset(pp, 0, sizeof pp);
		bb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; bb.memory = V4L2_MEMORY_MMAP; bb.length = g_n->np; bb.m.planes = pp;
		if (ioctl(g_n->fd, VIDIOC_DQBUF, &bb) < 0) break;
		if (k >= skip) { sh += sharp_dec(g_n->mp[bb.index][0], g_n->w, g_n->w, g_n->h, g_d); got++; }
		ioctl(g_n->fd, VIDIOC_QBUF, &bb);
	}
	return got ? sh / got : -1.0;
}
static void af_scan(int lo, int hi, int step)
{
	double best = -1; int bc = lo;
	for (int c = lo; c <= hi; c += step) { set_lens(c); double sh = grab_sharp(5, 1); if (sh > best) { best = sh; bc = c; } }
	g_af_peak = best; g_af_code = bc;
}
static void autofocus(int full)
{
	int c0 = 0, c1 = 3000, st = 200;
	if (!full) { c0 = g_af_code - 400; c1 = g_af_code + 400; st = 100; if (c0 < 0) c0 = 0; if (c1 > 4000) c1 = 4000; }
	af_scan(c0, c1, st);
	int c = g_af_code; af_scan(c - st < 0 ? 0 : c - st, c + st > 4000 ? 4000 : c + st, st / 5 > 10 ? st / 5 : 10);
	set_lens(g_af_code); double v = grab_sharp(5, 3);
	fprintf(stderr, "[dual] AF: best lens code %d, sharpness %.2f\n", g_af_code, v); g_af_peak = v;
}


// --- A7Z_STREAM=D: live preview. The sensor, ISP and capture stay at full resolution; only the copy sent over the USB link is decimated by D
//     (every D-th pixel and row), written as planar 4:2:0 of (W/D)x(H/D) to stdout. D=4 -> 1164x874 (~1.5 MB/frame, ~23 MB/s at 15 fps).
static void emit_decimated(FILE *out, struct node *nd, int bi, int W, int H, int D, unsigned char *buf)
{
	int ow = W / D, oh = H / D, cw = W / 2, o = 0;
	unsigned char *y = nd->mp[bi][0], *u = nd->mp[bi][1], *v = nd->mp[bi][2];
	for (int j = 0; j < oh; j++) { const unsigned char *r = y + (size_t)(j * D) * W; for (int i = 0; i < ow; i++) buf[o++] = r[i * D]; }
	for (int p = 1; p <= 2; p++) {
		unsigned char *pl = p == 1 ? u : v;
		for (int j = 0; j < oh / 2; j++) { const unsigned char *r = pl + (size_t)(j * D) * cw; for (int i = 0; i < ow / 2; i++) buf[o++] = r[i * D]; }
	}
	fwrite(buf, 1, o, out); fflush(out);
}

static int setup(struct node *nd, int idx, int W, int H, int fps, int large, int merge)
{
	char dev[32]; snprintf(dev, sizeof dev, "/dev/video%d", idx);
	nd->fd = open(dev, O_RDWR); if (nd->fd < 0) { perror(dev); return -1; }
	unsigned char mg = merge; xi(nd->fd, VIDIOC_SET_DMA_MERGE, &mg, "SET_DMA_MERGE");
	int input = 0; if (xi(nd->fd, VIDIOC_S_INPUT, &input, "S_INPUT")) return -1;
	struct v4l2_streamparm p; memset(&p, 0, sizeof p);
	p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; p.parm.capture.capturemode = V4L2_MODE_VIDEO;
	p.parm.capture.timeperframe.numerator = 1; p.parm.capture.timeperframe.denominator = fps;
	p.parm.capture.reserved[2] = large;
	if (xi(nd->fd, VIDIOC_S_PARM, &p, "S_PARM")) return -1;
	struct sensor_isp_cfg ic = { .isp_wdr_mode = 0, .large_image = large };
	xi(nd->fd, VIDIOC_SET_SENSOR_ISP_CFG, &ic, "SET_SENSOR_ISP_CFG");
	struct v4l2_format f; memset(&f, 0, sizeof f);
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; f.fmt.pix_mp.width = W; f.fmt.pix_mp.height = H;
	f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M; f.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (xi(nd->fd, VIDIOC_S_FMT, &f, "S_FMT") || xi(nd->fd, VIDIOC_G_FMT, &f, "G_FMT")) return -1;
	nd->w = f.fmt.pix_mp.width; nd->h = f.fmt.pix_mp.height; nd->np = f.fmt.pix_mp.num_planes; if (nd->np > NPL) nd->np = NPL;
	fprintf(stderr, "[dual] node %d got %dx%d planes=%d\n", idx, nd->w, nd->h, nd->np);
	struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
	rb.count = NBUF; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; rb.memory = V4L2_MEMORY_MMAP;
	if (xi(nd->fd, VIDIOC_REQBUFS, &rb, "REQBUFS")) return -1;
	for (int i = 0; i < (int)rb.count && i < NBUF; i++) {
		struct v4l2_buffer b; struct v4l2_plane pl[NPL]; memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.index = i; b.length = nd->np; b.m.planes = pl;
		if (xi(nd->fd, VIDIOC_QUERYBUF, &b, "QUERYBUF")) return -1;
		for (int q = 0; q < nd->np; q++) {
			nd->ml[i][q] = pl[q].length;
			nd->mp[i][q] = mmap(NULL, pl[q].length, PROT_READ | PROT_WRITE, MAP_SHARED, nd->fd, pl[q].m.mem_offset);
			if (nd->mp[i][q] == MAP_FAILED) { perror("mmap"); return -1; }
		}
		if (xi(nd->fd, VIDIOC_QBUF, &b, "QBUF")) return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 5) { fprintf(stderr, "usage: %s W H N PREFIX\n", argv[0]); return 2; }
	int W = atoi(argv[1]), H = atoi(argv[2]), N = atoi(argv[3]);
	int fps = getenv("A7Z_FPS") ? atoi(getenv("A7Z_FPS")) : 30, large = getenv("A7Z_LARGE") ? atoi(getenv("A7Z_LARGE")) : 3;
	int stream = getenv("A7Z_STREAM") ? atoi(getenv("A7Z_STREAM")) : 0, sskip = getenv("A7Z_STREAM_SKIP") ? atoi(getenv("A7Z_STREAM_SKIP")) : 1;
	FILE *outf = stdout; unsigned char *sbuf = NULL;
	if (stream > 0) { outf = fdopen(dup(1), "wb"); dup2(2, 1); sbuf = malloc((size_t)W * H * 3 / 2); }   // keep libisp's printf() off the frame stream
	int merge = getenv("A7Z_MERGE") ? atoi(getenv("A7Z_MERGE")) : 0;   // 1 = intended 2-in-1 path: open ONLY video1, driver drives video0 too
	int nodes = merge ? 1 : (getenv("A7Z_NODES") ? atoi(getenv("A7Z_NODES")) : 2);
	int first = merge ? 1 : 0;
	struct node nd[2]; memset(nd, 0, sizeof nd);
	for (int i = 0; i < nodes; i++) if (setup(&nd[i], first + i, W, H, fps, large, merge)) return 1;
	int af = getenv("A7Z_AF") && stream > 0;
	if (af) { g_sfd = open("/dev/v4l-subdev0", O_RDWR); g_block_isp_lens = 1; g_n = &nd[0]; g_d = stream; }
	AWIspApi *api = CreateAWIspApi();
	int nisp = merge ? 2 : nodes;                       // merge mode needs both ISP instances running
	for (int i = 0; i < nisp; i++) {
		int id = api ? api->ispGetIspId(i) : -1; fprintf(stderr, "[dual] isp slot %d isp_id=%d\n", i, id);
		if (api && id >= 0 && merge && !getenv("A7Z_NOLARGEIMG")) fprintf(stderr, "[dual] ispSetIspLargeImage(%d)=%d\n", id, api->ispSetIspLargeImage(id));   // tell libisp this is a 2-in-1 large image
		if (api && id >= 0) fprintf(stderr, "[dual] ispStart(%d)=%d\n", id, api->ispStart(id));
	}
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	for (int i = 0; i < nodes; i++) if (xi(nd[i].fd, VIDIOC_STREAMON, &t, "STREAMON")) return 1;
	if (af) autofocus(1);
	int af_low = 0, af_since = 0;
	int done = 0;
	for (int iter = 0; iter < N * 10 && done < nodes; iter++) {
		fd_set fds; FD_ZERO(&fds); int mx = 0;
		for (int i = 0; i < nodes; i++) { FD_SET(nd[i].fd, &fds); if (nd[i].fd > mx) mx = nd[i].fd; }
		struct timeval tv = { 3, 0 };
		if (select(mx + 1, &fds, NULL, NULL, &tv) <= 0) { fprintf(stderr, "[dual] select timeout (frames so far: %d / %d)\n", nd[0].frames, nodes > 1 ? nd[1].frames : 0); break; }
		for (int i = 0; i < nodes; i++) {
			if (!FD_ISSET(nd[i].fd, &fds)) continue;
			struct v4l2_buffer b; struct v4l2_plane pl[NPL]; memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
			b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.length = nd[i].np; b.m.planes = pl;
			if (xi(nd[i].fd, VIDIOC_DQBUF, &b, "DQBUF")) continue;
			nd[i].frames++;
			{ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); double now = ts.tv_sec + ts.tv_nsec * 1e-9;
			  if (nd[i].frames == 5) nd[i].t5 = now; if (nd[i].frames == N) nd[i].tN = now; }
			if (stream > 0 && (nd[i].frames % (sskip > 0 ? sskip : 1)) == 0) emit_decimated(outf, &nd[i], b.index, nd[i].w, nd[i].h, stream, sbuf);
			if (nd[i].frames == N) {
				char name[256]; snprintf(name, sizeof name, "%s_n%d_%d_%dx%d.yuv", argv[4], i, nd[i].frames, nd[i].w, nd[i].h);
				FILE *f = fopen(name, "wb");
				if (f) { for (int q = 0; q < nd[i].np; q++) fwrite(nd[i].mp[b.index][q], 1, pl[q].bytesused, f); fclose(f); }
				fprintf(stderr, "[dual] wrote %s (bytesused %u %u %u)\n", name, pl[0].bytesused, pl[1].bytesused, pl[2].bytesused);
				done++;
			}
			double cur = -1;
			if (af && (nd[i].frames % 3) == 0) cur = sharp_dec(nd[i].mp[b.index][0], nd[i].w, nd[i].w, nd[i].h, g_d);
			xi(nd[i].fd, VIDIOC_QBUF, &b, "QBUF");
			if (cur >= 0) {                       // refocus when the picture has been soft for ~30 checks
				af_since++; af_low = (cur < 0.7 * g_af_peak) ? af_low + 1 : 0;
				if (af_low >= 30 && af_since > 40) { fprintf(stderr, "[dual] AF: picture went soft (%.2f vs %.2f), refocusing\n", cur, g_af_peak); autofocus(0); af_low = 0; af_since = 0; }
			}
		}
	}
	for (int i = 0; i < nodes; i++) xi(nd[i].fd, VIDIOC_STREAMOFF, &t, "STREAMOFF");
	if (api) { for (int i = 0; i < nisp; i++) { int id = api->ispGetIspId(i); if (id >= 0) api->ispStop(id); } DestroyAWIspApi(api); }
	if (nd[0].frames >= N && N > 5) fprintf(stderr, "[dual] measured %.2f fps over frames 5..%d\n", (N - 5) / (nd[0].tN - nd[0].t5), N);
	fprintf(stderr, "[dual] done frames: n0=%d n1=%d\n", nd[0].frames, nodes > 1 ? nd[1].frames : 0);
	return 0;
}
