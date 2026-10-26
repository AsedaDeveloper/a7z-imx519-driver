// ispd.c — run libisp's 3A on the camera WITHOUT owning the capture stream, so an ordinary V4L2 app (v4l2-ctl, ffmpeg, GStreamer, OpenCV) can open /dev/video0
// and get ISP-processed frames. Sets the sensor mode and format, starts the ISP, then idles until killed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "sunxi_camera_v2.h"
#include "AWIspApi.h"

static volatile int run = 1;
static void stop(int s) { (void)s; run = 0; }

int main(int argc, char **argv)
{
	int W = argc > 1 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080;
	int fps = argc > 3 ? atoi(argv[3]) : 30;
	int hold = getenv("ISPD_HOLD") != NULL;      // keep /dev/video0 open while idling (vs. closing it so the consumer has it to itself)

	int fd = open("/dev/video0", O_RDWR);
	if (fd < 0) { perror("open /dev/video0"); return 1; }

	unsigned char merge = 0;
	ioctl(fd, VIDIOC_SET_DMA_MERGE, &merge);
	int input = 0;
	if (ioctl(fd, VIDIOC_S_INPUT, &input) < 0) { perror("S_INPUT"); return 1; }

	struct v4l2_streamparm parm;
	memset(&parm, 0, sizeof parm);
	parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	parm.parm.capture.capturemode = V4L2_MODE_VIDEO;
	parm.parm.capture.timeperframe.numerator = 1;
	parm.parm.capture.timeperframe.denominator = fps;
	if (ioctl(fd, VIDIOC_S_PARM, &parm) < 0) { perror("S_PARM"); return 1; }

	struct sensor_isp_cfg icfg = { .isp_wdr_mode = 0, .large_image = 0 };
	ioctl(fd, VIDIOC_SET_SENSOR_ISP_CFG, &icfg);

	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = W;
	fmt.fmt.pix_mp.height = H;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420M;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
	printf("[ispd] format %ux%u\n", fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);

	AWIspApi *api = CreateAWIspApi();
	if (!api) { fprintf(stderr, "[ispd] CreateAWIspApi failed\n"); return 1; }
	api->ispApiInit();
	int id = api->ispGetIspId(0);
	printf("[ispd] isp id %d\n", id);
	if (id < 0) return 1;
	if (api->ispStart(id) != 0) { fprintf(stderr, "[ispd] ispStart failed\n"); return 1; }
	printf("[ispd] ISP running%s. Open /dev/video0 from any V4L2 app.\n", hold ? " (holding fd)" : " (fd closed)");
	fflush(stdout);

	if (!hold) close(fd);
	signal(SIGINT, stop); signal(SIGTERM, stop);
	while (run) sleep(1);

	api->ispStop(id);
	api->ispWaitToExit(id);
	api->ispApiUnInit();
	DestroyAWIspApi(api);
	printf("[ispd] stopped\n");
	return 0;
}
