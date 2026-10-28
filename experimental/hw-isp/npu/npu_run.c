// Run a network binary (.nb) on the NPU: npu_run model.nb input.u8 out_prefix
// input.u8 is the raw uint8 input tensor (YOLOv5: 640x640x3, planar RGB). Every output tensor is written, dequantised to float32, as <out_prefix><n>.f32.
// Uses the awnn wrapper from Allwinner's ai-sdk (awnn_lib.c / awnn_quantize.c, not copied into this repo) over libVIPhal / libNBGlinker v2.0.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <awnn_lib.h>
int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: %s model.nb input.u8 out_prefix [n_outputs=3] [size0,size1,... floats]\n", argv[0]); return 2; }
	FILE *f = fopen(argv[2], "rb"); if (!f) { perror("input"); return 1; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	unsigned char *in = malloc(n); if (fread(in, 1, n, f) != (size_t)n) return 1; fclose(f);
	int nout = argc > 4 ? atoi(argv[4]) : 3;
	awnn_init();
	Awnn_Context_t *ctx = awnn_create(argv[1]); if (!ctx) { fprintf(stderr, "awnn_create failed\n"); return 1; }
	void *bufs[] = { in }; awnn_set_input_buffers(ctx, bufs);
	struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
	awnn_run(ctx);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	fprintf(stderr, "inference %.1f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6);
	float **out = awnn_get_output_buffers(ctx);
	long sizes[8] = { 3L*80*80*85, 3L*40*40*85, 3L*20*20*85 };   // YOLOv5s COCO heads (stride 8, 16, 32); other networks: change here
	for (int i = 0; i < nout && i < 3; i++) {
		char name[512]; snprintf(name, sizeof name, "%s%d.f32", argv[3], i);
		FILE *o = fopen(name, "wb"); fwrite(out[i], 4, sizes[i], o); fclose(o);
	}
	awnn_destroy(ctx); awnn_uninit(); return 0;
}
