#include <stdbool.h>
#include <stdint.h>
// G2D scale test: 1920x1080 Y8 -> 640x360 Y8 via dma-buf fds, compared with a CPU box downscale.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <bsp/linux/sunxi-g2d.h>

static int heap_alloc(size_t len) {
    int h = open((getenv("HEAP") ? getenv("HEAP") : "/dev/dma_heap/system"), O_RDWR);
    if (h < 0) { perror("heap"); exit(1); }
    struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
    if (ioctl(h, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { perror("alloc"); exit(1); }
    close(h);
    return a.fd;
}
static void bufsync(int fd, int f){ struct dma_buf_sync s={.flags=f}; ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
    const int SW = 1920, SH = 1080, DW = 640, DH = 360; const int BPP = getenv("ARGB") ? 4 : 1;
    int g = open("/dev/g2d", O_RDWR);
    if (g < 0) { perror("g2d"); return 1; }
    int sfd = heap_alloc(SW * SH * BPP), dfd = heap_alloc(DW * DH * BPP);
    unsigned char *s = mmap(0, SW * SH * BPP, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, 0);
    unsigned char *d = mmap(0, DW * DH * BPP, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) s[(y * SW + x) * BPP] = (x / 8 + y / 8) & 1 ? 200 : 40 + (x * 100 / SW);
    memset(d, 0, DW * DH * BPP);
    bufsync(sfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE); bufsync(dfd, DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE);

    g2d_blt_h b; memset(&b, 0, sizeof b);
    b.flag_h = G2D_BLT_NONE_H;
    b.src_image_h.format = getenv("ARGB") ? G2D_FORMAT_ARGB8888 : G2D_FORMAT_Y8; b.src_image_h.fd = sfd; b.src_image_h.use_phy_addr = 0;
    b.src_image_h.width = SW; b.src_image_h.height = SH; b.src_image_h.align[0] = 0;
    b.src_image_h.clip_rect = (g2d_rect){0, 0, SW, SH};
    b.src_image_h.mode = G2D_PIXEL_ALPHA; b.src_image_h.alpha = 255; b.src_image_h.color_range = COLOR_RANGE_0_255;
    b.src_image_h.gamut = G2D_BT601; b.src_image_h.bpremul = 0;
    b.src_image_h.bbuff = 1;                    /* 0 = layer is the constant `color`, not the buffer: the reason every earlier attempt wrote black/nothing */
    if (getenv("FULL")) { b.src_image_h.resize = (g2d_size){SW, SH}; b.src_image_h.coor = (g2d_coor){0, 0}; }
    b.dst_image_h = b.src_image_h;
    b.dst_image_h.fd = dfd; b.dst_image_h.width = DW; b.dst_image_h.height = DH;
    b.dst_image_h.clip_rect = (g2d_rect){0, 0, DW, DH};

    if (getenv("FULL")) b.dst_image_h.resize = (g2d_size){DW, DH};
    if (getenv("VER")) { struct g2d_hardware_version v = {0}; int r = ioctl(g, G2D_CMD_QUERY_VERSION, &v); printf("QUERY_VERSION rc=%d g2d=0x%x chip=0x%x\n", r, v.g2d_version, v.chip_version); }
    double t0 = now(); int rc = 0, N = 100;
    const char *mode = getenv("MODE") ? getenv("MODE") : "blt";
    if (!strcmp(mode, "task")) {
        mixer_para mp; memset(&mp, 0, sizeof mp);
        mp.op_flag = OP_BITBLT; mp.flag_h = G2D_BLT_NONE_H; mp.src_image_h = b.src_image_h; mp.dst_image_h = b.dst_image_h;
        int id = ioctl(g, G2D_CMD_CREATE_TASK, &mp); printf("CREATE_TASK -> %d\n", id);
        if (id < 0) perror("CREATE_TASK");
        unsigned int uid = id;
        for (int i = 0; i < N && !rc; i++) rc = ioctl(g, G2D_CMD_TASK_APPLY, &uid);
        printf("TASK_APPLY rc=%d\n", rc);
        ioctl(g, G2D_CMD_TASK_DESTROY, &uid);
    } else if (!strcmp(mode, "mixer")) {
        mixer_para mp; memset(&mp, 0, sizeof mp);
        mp.op_flag = OP_BITBLT; mp.flag_h = G2D_BLT_NONE_H; mp.src_image_h = b.src_image_h; mp.dst_image_h = b.dst_image_h;
        unsigned long args[2] = {(unsigned long)&mp, 1};
        for (int i = 0; i < N && !rc; i++) rc = ioctl(g, G2D_CMD_MIXER_TASK, &mp);
        printf("MIXER_TASK(&mixer_para) rc=%d\n", rc);
        (void)args;
    } else
    for (int i = 0; i < N && !rc; i++) rc = ioctl(g, G2D_CMD_BITBLT_H, &b);
    double dt = (now() - t0) / N;
    printf("ioctl rc=%d  %.2f ms/frame\n", rc, dt * 1e3);
    if (rc) perror("BITBLT_H");

    bufsync(dfd, DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ);
    long err = 0, nz = 0;
    for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) {
        int sum = 0; for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) sum += s[((y * 3 + j) * SW + x * 3 + i) * BPP];
        int ref = sum / 9, got = d[(y * DW + x) * BPP]; err += abs(ref - got); nz += got != 0;
    }
    printf("nonzero dst px %ld/%d, mean |G2D - box3x3| = %.2f\n", nz, DW * DH, (double)err / (DW * DH));
    t0 = now();
    for (int k = 0; k < 20; k++) for (int y = 0; y < DH; y++) for (int x = 0; x < DW; x++) {
        int sum = 0; for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) sum += s[((y * 3 + j) * SW + x * 3 + i) * BPP];
        d[(y * DW + x) * BPP] = sum / 9; }
    printf("CPU box 3x3 (1 core, naive) %.2f ms/frame\n", (now() - t0) / 20 * 1e3);
    return rc != 0;
}
