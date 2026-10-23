// a7zdual: capture through the A733 "2-in-1" (large image) mode, which lifts the single-ISP limit of 3264 pixels per line.
// The CSI parser splits one sensor stream into two overlapping halves, one per ISP, and both DMAs write into one buffer.
// What makes it work (all three are required):
//   1. A7Z_MERGE=1: SET_DMA_MERGE=1 and open ONLY /dev/video1. The driver forwards S_INPUT/S_FMT/S_PARM to video0 itself
//      (forcing large_image=3) and streams both pipelines. Opening both nodes yourself makes them fight over the media links.
//   2. A7Z_FPS=<fps_fixed of the sensor mode>: the sensor-mode selector only considers modes with that frame rate.
//   3. libisp must be told it is a large image: ispSetIspLargeImage(id) on BOTH ISP instances, before ispStart.
//      Without it the pipeline delivers 1-2 frames and then stalls ("tdm0 rx0 buffer full").
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
#include <linux/videodev2.h>
#include "sunxi_camera_v2.h"
#include "AWIspApi.h"
#define NBUF 4
#define NPL 3
struct node { double t5, tN; int fd, np, w, h; void *mp[NBUF][NPL]; size_t ml[NBUF][NPL]; int frames; };
static int xi(int fd, unsigned long r, void *a, const char *n) { int x = ioctl(fd, r, a); if (x < 0) fprintf(stderr, "[dual] fd%d %s failed: %s\n", fd, n, strerror(errno)); return x; }

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
	int merge = getenv("A7Z_MERGE") ? atoi(getenv("A7Z_MERGE")) : 0;   // 1 = intended 2-in-1 path: open ONLY video1, driver drives video0 too
	int nodes = merge ? 1 : (getenv("A7Z_NODES") ? atoi(getenv("A7Z_NODES")) : 2);
	int first = merge ? 1 : 0;
	struct node nd[2]; memset(nd, 0, sizeof nd);
	for (int i = 0; i < nodes; i++) if (setup(&nd[i], first + i, W, H, fps, large, merge)) return 1;
	AWIspApi *api = CreateAWIspApi();
	int nisp = merge ? 2 : nodes;                       // merge mode needs both ISP instances running
	for (int i = 0; i < nisp; i++) {
		int id = api ? api->ispGetIspId(i) : -1; fprintf(stderr, "[dual] isp slot %d isp_id=%d\n", i, id);
		if (api && id >= 0 && merge && !getenv("A7Z_NOLARGEIMG")) fprintf(stderr, "[dual] ispSetIspLargeImage(%d)=%d\n", id, api->ispSetIspLargeImage(id));   // tell libisp this is a 2-in-1 large image
		if (api && id >= 0) fprintf(stderr, "[dual] ispStart(%d)=%d\n", id, api->ispStart(id));
	}
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	for (int i = 0; i < nodes; i++) if (xi(nd[i].fd, VIDIOC_STREAMON, &t, "STREAMON")) return 1;
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
			if (nd[i].frames == N) {
				char name[256]; snprintf(name, sizeof name, "%s_n%d_%d_%dx%d.yuv", argv[4], i, nd[i].frames, nd[i].w, nd[i].h);
				FILE *f = fopen(name, "wb");
				if (f) { for (int q = 0; q < nd[i].np; q++) fwrite(nd[i].mp[b.index][q], 1, pl[q].bytesused, f); fclose(f); }
				fprintf(stderr, "[dual] wrote %s (bytesused %u %u %u)\n", name, pl[0].bytesused, pl[1].bytesused, pl[2].bytesused);
				done++;
			}
			xi(nd[i].fd, VIDIOC_QBUF, &b, "QBUF");
		}
	}
	for (int i = 0; i < nodes; i++) xi(nd[i].fd, VIDIOC_STREAMOFF, &t, "STREAMOFF");
	if (api) { for (int i = 0; i < nisp; i++) { int id = api->ispGetIspId(i); if (id >= 0) api->ispStop(id); } DestroyAWIspApi(api); }
	if (nd[0].frames >= N && N > 5) fprintf(stderr, "[dual] measured %.2f fps over frames 5..%d\n", (N - 5) / (nd[0].tN - nd[0].t5), N);
	fprintf(stderr, "[dual] done frames: n0=%d n1=%d\n", nd[0].frames, nodes > 1 ? nd[1].frames : 0);
	return 0;
}
