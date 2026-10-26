// enctest.c — does the Allwinner hardware video engine (cedar) encode H.264 on this board?
// Feeds a synthetic moving pattern through libvencoder and writes an Annex-B .h264 file. No camera involved: this isolates the encoder.
// build: gcc -O2 -o enctest enctest.c -lvencoder -lMemAdapter -lVE -lvenc_base -lcdc_base -lpthread -lm
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vencoder.h>
#include <memoryAdapter.h>
#include <veAdapter.h>
#include <sc_interface.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv)
{
	int W = argc > 1 ? atoi(argv[1]) : 1920, H = argc > 2 ? atoi(argv[2]) : 1080;
	int N = argc > 3 ? atoi(argv[3]) : 60, fps = 30, kbps = argc > 4 ? atoi(argv[4]) : 8000;
	const char *out = argc > 5 ? argv[5] : "/tmp/enc.h264";

	const char *cs = getenv("CODEC") ? getenv("CODEC") : "h264";
	VENC_CODEC_TYPE ct = !strcmp(cs, "h265") ? VENC_CODEC_H265 : !strcmp(cs, "jpeg") ? VENC_CODEC_JPEG : VENC_CODEC_H264;
	VideoEncoder *enc = VideoEncCreate(ct);
	if (!enc) { fprintf(stderr, "VideoEncCreate failed\n"); return 1; }

	int v = kbps * 1000;      VideoEncSetParameter(enc, VENC_IndexParamBitrate, &v);
	int f = fps;              VideoEncSetParameter(enc, VENC_IndexParamFramerate, &f);
	VencH264Param h264; memset(&h264, 0, sizeof h264);
	h264.bEntropyCodingCABAC = 1; h264.nBitrate = v; h264.nFramerate = fps;
	h264.nCodingMode = VENC_FRAME_CODING; h264.nMaxKeyInterval = fps;
	h264.sProfileLevel.nProfile = VENC_H264ProfileMain; h264.sProfileLevel.nLevel = VENC_H264Level41;
	h264.sQPRange.nMinqp = 10; h264.sQPRange.nMaxqp = 45;
	if (ct == VENC_CODEC_H264) VideoEncSetParameter(enc, VENC_IndexParamH264Param, &h264);

	unsigned int vbv = getenv("VBV") ? (unsigned)atoi(getenv("VBV")) : 0;      // bitstream buffer; the default is too small for frames above 1080p
	if (vbv) VideoEncSetParameter(enc, VENC_IndexParamSetVbvSize, &vbv);
	VencBaseConfig cfg; memset(&cfg, 0, sizeof cfg);
	cfg.nInputWidth = W; cfg.nInputHeight = H; cfg.nDstWidth = W; cfg.nDstHeight = H;
	cfg.bEncH264Nalu = getenv("NALU0") ? 0 : 1;                                    // emit Annex-B start codes, otherwise the bitstream has no NAL delimiters
	cfg.nStride = W; cfg.eInputFormat = VENC_PIXEL_YUV420SP;   // set below          // planar Y, U, V — what the ISP gives us
	cfg.eInputFormat = getenv("NV12") ? VENC_PIXEL_YUV420SP : VENC_PIXEL_YUV420P;
	cfg.memops = MemAdapterGetOpsS();
	if (!cfg.memops) { fprintf(stderr, "MemAdapterGetOpsS failed\n"); return 1; }
	if (CdcMemOpen(cfg.memops) != 0) { fprintf(stderr, "CdcMemOpen failed\n"); return 1; }   // without this the VE gets unmapped addresses: "Bug is in VE_ENC module, invalid address"
	if (!getenv("NOVEOPS")) cfg.veOpsS = GetVeOpsS(VE_OPS_TYPE_NORMAL);
	int nv12 = getenv("NV12") != NULL;
	if (VideoEncInit(enc, &cfg) != 0) { fprintf(stderr, "VideoEncInit failed\n"); return 1; }

	VencAllocateBufferParam ab; memset(&ab, 0, sizeof ab);
	ab.nBufferNum = 4; ab.nSizeY = W * H; ab.nSizeC = W * H / 2;
	if (AllocInputBuffer(enc, &ab) != 0) { fprintf(stderr, "AllocInputBuffer failed\n"); return 1; }

	FILE *fo = fopen(out, "wb");
	int hdr_done = (ct == VENC_CODEC_JPEG);                  // SPS/PPS (VPS/SPS/PPS) must precede the frames; H.264 only fills them after the first frame is encoded
	long bytes = 0; double t0 = now(), tenc = 0;
	for (int n = 0; n < N; n++) {
		VencInputBuffer ib; memset(&ib, 0, sizeof ib);
		if (GetOneAllocInputBuffer(enc, &ib) != 0) { fprintf(stderr, "GetOneAllocInputBuffer failed at %d\n", n); break; }
		// synthetic frame: moving diagonal ramp, so the bitstream is not trivially empty
		for (int y = 0; y < H; y++) memset(ib.pAddrVirY + (size_t)y * W, (unsigned char)(y + n * 4), W);
		for (int y = 0; y < H / 2; y++) {
			memset(ib.pAddrVirC + (size_t)y * (W / 2), (unsigned char)(128 + 40 * ((n / 10) & 1)), W / 2);
			memset(ib.pAddrVirC + (size_t)(H / 2) * (W / 2) + (size_t)y * (W / 2), 128, W / 2);
		}
		ib.nPts = (long long)n * 1000000 / fps;
		FlushCacheAllocInputBuffer(enc, &ib);
		AddOneInputBuffer(enc, &ib);

		double te = now();
		int rc = VideoEncodeOneFrame(enc);
		tenc += now() - te;
		if (rc != 0) fprintf(stderr, "VideoEncodeOneFrame rc=%d at %d\n", rc, n);

		AlreadyUsedInputBuffer(enc, &ib);
		ReturnOneAllocInputBuffer(enc, &ib);

		if (!hdr_done && fo) {
			VencHeaderData hd; memset(&hd, 0, sizeof hd);
			int idx = (ct == VENC_CODEC_H265) ? VENC_IndexParamH265Header : VENC_IndexParamH264SPSPPS;
			if (VideoEncGetParameter(enc, idx, &hd) == 0 && hd.nLength && hd.pBuffer) { fwrite(hd.pBuffer, 1, hd.nLength, fo); printf("header %u bytes\n", hd.nLength); }
			hdr_done = 1;
		}
		VencOutputBuffer ob; memset(&ob, 0, sizeof ob);
		if (GetOneBitstreamFrame(enc, &ob) == 0) {
			if (fo) { fwrite(ob.pData0, 1, ob.nSize0, fo); if (ob.nSize1) fwrite(ob.pData1, 1, ob.nSize1, fo); }
			bytes += ob.nSize0 + ob.nSize1;
			FreeOneBitStreamFrame(enc, &ob);
		}
	}
	double wall = now() - t0;
	if (fo) fclose(fo);
	ReleaseAllocInputBuffer(enc); VideoEncUnInit(enc); VideoEncDestroy(enc); CdcMemClose(cfg.memops);
	printf("%dx%d %d frames: %ld bytes (%.2f Mbit/s at %d fps), encode %.2f ms/frame, wall %.2f s = %.1f fps -> %s\n",
	       W, H, N, bytes, bytes * 8.0 * fps / N / 1e6, fps, tenc * 1000 / N, wall, N / wall, out);
	return 0;
}
