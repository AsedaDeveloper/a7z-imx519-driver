#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <bsp/linux/sunxi-g2d.h>
static int alloc_(const char*hp,size_t len){int h=open(hp,O_RDWR);struct dma_heap_allocation_data a={.len=len,.fd_flags=O_RDWR|O_CLOEXEC};if(h<0||ioctl(h,DMA_HEAP_IOCTL_ALLOC,&a)<0){perror("alloc");exit(1);}close(h);return a.fd;}
int main(int c,char**v){
  const char*hp=c>1?v[1]:"/dev/dma_heap/system"; int W=640,H=360;
  int g=open("/dev/g2d",O_RDWR); int fd=alloc_(hp,W*H*4);
  unsigned char*d=mmap(0,W*H*4,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); memset(d,0xAA,W*H*4);
  struct dma_buf_sync s={.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE}; ioctl(fd,DMA_BUF_IOCTL_SYNC,&s);
  g2d_fillrect_h f; memset(&f,0,sizeof f);
  f.dst_image_h.format=G2D_FORMAT_ARGB8888; f.dst_image_h.fd=fd; f.dst_image_h.width=W; f.dst_image_h.height=H;
  f.dst_image_h.clip_rect=(g2d_rect){0,0,W,H}; f.dst_image_h.mode=G2D_PIXEL_ALPHA; f.dst_image_h.alpha=255; f.dst_image_h.color=0xFF336699; f.dst_image_h.color_range=COLOR_RANGE_0_255;
  int rc=ioctl(g,G2D_CMD_FILLRECT_H,&f); printf("FILLRECT_H rc=%d\n",rc);
  s.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ; ioctl(fd,DMA_BUF_IOCTL_SYNC,&s);
  printf("first px %02x%02x%02x%02x  last px %02x%02x%02x%02x  (0xAA = untouched)\n",d[0],d[1],d[2],d[3],d[W*H*4-4],d[W*H*4-3],d[W*H*4-2],d[W*H*4-1]);
  unsigned char*d2=mmap(0,W*H*4,PROT_READ,MAP_SHARED,fd,0); printf("fresh mmap first px %02x%02x%02x%02x\n",d2[0],d2[1],d2[2],d2[3]);
  return 0;}
