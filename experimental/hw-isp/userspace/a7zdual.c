// a7zdual: experiment - drive BOTH vin nodes in the A733 "2-in-1" (large_image=3) mode so a sensor mode wider than the
// single-ISP limit (3264) is split by the CSI parser into two overlapping halves, one per ISP.
// usage: A7Z_FPS=18 a7zdual WIDTH HEIGHT NFRAMES PREFIX   (writes PREFIX_<node>_<n>_<w>x<h>.yuv for the last frame of each node)
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
#include <linux/videodev2.h>
#include "sunxi_camera_v2.h"
#include "AWIspApi.h"
#define NBUF 4
#define NPL 3
struct node { int fd, np, w, h; void *mp[NBUF][NPL]; size_t ml[NBUF][NPL]; int frames; };
static int xi(int fd, unsigned long r, void *a, const char *n) { int x = ioctl(fd, r, a); if (x < 0) fprintf(stderr, "[dual] fd%d %s failed: %s\n", fd, n, strerror(errno)); return x; }

static int setup(struct node *nd, int idx, int W, int H, int fps, int large)
{
	char dev[32]; snprintf(dev, sizeof dev, "/dev/video%d", idx);
	nd->fd = open(dev, O_RDWR); if (nd->fd < 0) { perror(dev); return -1; }
	unsigned char merge = 0; xi(nd->fd, VIDIOC_SET_DMA_MERGE, &merge, "SET_DMA_MERGE");
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
	int nodes = getenv("A7Z_NODES") ? atoi(getenv("A7Z_NODES")) : 2;
	struct node nd[2]; memset(nd, 0, sizeof nd);
	for (int i = 0; i < nodes; i++) if (setup(&nd[i], i, W, H, fps, large)) return 1;
	AWIspApi *api = CreateAWIspApi();
	for (int i = 0; i < nodes; i++) {
		int id = api ? api->ispGetIspId(i) : -1; fprintf(stderr, "[dual] node %d isp_id=%d\n", i, id);
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
	if (api) { for (int i = 0; i < nodes; i++) { int id = api->ispGetIspId(i); if (id >= 0) api->ispStop(id); } DestroyAWIspApi(api); }
	fprintf(stderr, "[dual] done frames: n0=%d n1=%d\n", nd[0].frames, nodes > 1 ? nd[1].frames : 0);
	return 0;
}
