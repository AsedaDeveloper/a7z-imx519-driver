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
static int alloc_(size_t len){int h=open("/dev/dma_heap/system",O_RDWR);struct dma_heap_allocation_data a={.len=len,.fd_flags=O_RDWR|O_CLOEXEC};if(h<0||ioctl(h,DMA_HEAP_IOCTL_ALLOC,&a)<0){perror("alloc");exit(1);}close(h);return a.fd;}
static void sy(int fd,int f){struct dma_buf_sync s={.flags=f};ioctl(fd,DMA_BUF_IOCTL_SYNC,&s);}
int main(int c,char**v){
  int SW=1920,SH=1080,DW=640,DH=360; unsigned rot=c>1?strtoul(v[1],0,0):0; int fmt=c>2?atoi(v[2]):0;
  int g=open("/dev/g2d",O_RDWR); int sfd=alloc_(SW*SH*3/2), dfd=alloc_(DW*DH*4);
  unsigned char*s=mmap(0,SW*SH*3/2,PROT_READ|PROT_WRITE,MAP_SHARED,sfd,0); unsigned char*d=mmap(0,DW*DH*4,PROT_READ|PROT_WRITE,MAP_SHARED,dfd,0);
  for(int y=0;y<SH;y++)for(int x=0;x<SW;x++) s[y*SW+x]=x*255/SW;          /* Y: left dark -> right bright */
  for(int i=0;i<SW*SH/4;i++){ s[SW*SH+2*i]=90; s[SW*SH+2*i+1]=200; }      /* NV12 chroma: U=90 V=200 */
  memset(d,0xAA,DW*DH*4); sy(sfd,DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE); sy(dfd,DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE);
  g2d_blt_h b; memset(&b,0,sizeof b); b.flag_h=rot;
  g2d_image_enh *S=&b.src_image_h,*D=&b.dst_image_h;
  S->format= fmt? G2D_FORMAT_YUV420UVC_V1U1V0U0 : G2D_FORMAT_YUV420UVC_U1V1U0V0; D->format=G2D_FORMAT_ARGB8888; S->fd=sfd; D->fd=dfd; S->bbuff=D->bbuff=1;
  int ow=(rot&0x100)?DH:DW, oh=(rot&0x100)?DW:DH;
  S->width=SW;S->height=SH;D->width=ow;D->height=oh; S->clip_rect=(g2d_rect){0,0,SW,SH}; D->clip_rect=(g2d_rect){0,0,ow,oh};
  S->mode=D->mode=G2D_PIXEL_ALPHA; S->alpha=D->alpha=255; S->color_range=D->color_range=COLOR_RANGE_0_255; S->gamut=D->gamut=G2D_BT601;
  int rc=ioctl(g,G2D_CMD_BITBLT_H,&b); sy(dfd,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ);
  unsigned*dd=(unsigned*)d; int n=0; for(int i=0;i<ow*oh;i++) n+=dd[i]!=0xAAAAAAAA;
  printf("rot=0x%x fmt=%d rc=%d out %dx%d changed %d/%d\n",rot,fmt,rc,ow,oh,n,ow*oh);
  #define P(x,y) dd[(y)*ow+(x)]
  printf("  left-mid %08x  centre %08x  right-mid %08x  top-mid %08x  bottom-mid %08x\n",P(2,oh/2),P(ow/2,oh/2),P(ow-3,oh/2),P(ow/2,2),P(ow/2,oh-3));
  return 0;}
