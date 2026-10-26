// v4l2cap.c — a plain V4L2 capture client. No libisp, no vendor ioctls: only what ffmpeg, GStreamer or OpenCV would issue.
// If this produces ISP-processed frames while ispd runs, then a standard app can use the camera (checklist B1/B19).
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

#define NB 4
int main(int argc, char **argv)
{
	int W = argc > 1 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080;
	int N = argc > 3 ? atoi(argv[3]) : 10;
	const char *out = argc > 4 ? argv[4] : "/tmp/plain.yuv";
	const char *dev = getenv("V4L2_DEV") ? getenv("V4L2_DEV") : "/dev/video0";

	int fd = open(dev, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }

	struct v4l2_capability cap; memset(&cap, 0, sizeof cap);
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return 1; }
	int mplane = !!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
	enum v4l2_buf_type type = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
	printf("[v4l2cap] %s driver=%s mplane=%d\n", dev, cap.driver, mplane);

	struct v4l2_format fmt; memset(&fmt, 0, sizeof fmt);
	fmt.type = type;
	if (mplane) {
		fmt.fmt.pix_mp.width = W; fmt.fmt.pix_mp.height = H;
		fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M; fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	} else {
		fmt.fmt.pix.width = W; fmt.fmt.pix.height = H;
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420; fmt.fmt.pix.field = V4L2_FIELD_NONE;
	}
	if (getenv("SKIP_FMT")) {                                 // the vendor driver refuses S_FMT while the ISP owns the pipeline; fall back to whatever is set
		if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) { perror("G_FMT"); return 1; }
	} else if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	int np = mplane ? fmt.fmt.pix_mp.num_planes : 1;
	printf("[v4l2cap] format %ux%u planes=%d\n", mplane ? fmt.fmt.pix_mp.width : fmt.fmt.pix.width,
	       mplane ? fmt.fmt.pix_mp.height : fmt.fmt.pix.height, np);

	struct v4l2_requestbuffers rb; memset(&rb, 0, sizeof rb);
	rb.count = NB; rb.type = type; rb.memory = V4L2_MEMORY_MMAP;
	if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) { perror("REQBUFS"); return 1; }

	void *mp[NB][3]; size_t ml[NB][3];
	for (unsigned i = 0; i < rb.count && i < NB; i++) {
		struct v4l2_buffer b; struct v4l2_plane pl[3];
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = type; b.memory = V4L2_MEMORY_MMAP; b.index = i;
		if (mplane) { b.length = np; b.m.planes = pl; }
		if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { perror("QUERYBUF"); return 1; }
		for (int p = 0; p < np; p++) {
			size_t len = mplane ? pl[p].length : b.length;
			off_t off = mplane ? pl[p].m.mem_offset : b.m.offset;
			ml[i][p] = len;
			mp[i][p] = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
			if (mp[i][p] == MAP_FAILED) { perror("mmap"); return 1; }
		}
		if (ioctl(fd, VIDIOC_QBUF, &b) < 0) { perror("QBUF"); return 1; }
	}

	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
	printf("[v4l2cap] streaming\n");

	FILE *f = fopen(out, "wb");
	int got = 0;
	for (int n = 0; n < N; n++) {
		fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
		struct timeval tv = { 3, 0 };
		if (select(fd + 1, &fds, NULL, NULL, &tv) <= 0) { fprintf(stderr, "[v4l2cap] timeout at frame %d\n", n); break; }
		struct v4l2_buffer b; struct v4l2_plane pl[3];
		memset(&b, 0, sizeof b); memset(pl, 0, sizeof pl);
		b.type = type; b.memory = V4L2_MEMORY_MMAP;
		if (mplane) { b.length = np; b.m.planes = pl; }
		if (ioctl(fd, VIDIOC_DQBUF, &b) < 0) { perror("DQBUF"); break; }
		if (f && n == N - 1) for (int p = 0; p < np; p++) fwrite(mp[b.index][p], 1, mplane ? (pl[p].bytesused ? pl[p].bytesused : ml[b.index][p]) : b.bytesused, f);
		got++;
		ioctl(fd, VIDIOC_QBUF, &b);
	}
	if (f) fclose(f);
	ioctl(fd, VIDIOC_STREAMOFF, &type);
	close(fd);
	printf("[v4l2cap] got %d/%d frames -> %s\n", got, N, out);
	return got ? 0 : 1;
}
