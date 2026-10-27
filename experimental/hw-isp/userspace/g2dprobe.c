#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <bsp/linux/sunxi-g2d.h>
static int alloc_(size_t len){int h=open("/dev/dma_heap/system",O_RDWR);struct dma_heap_allocation_data a={.len=len,.fd_flags=O_RDWR|O_CLOEXEC};if(h<0||ioctl(h,DMA_HEAP_IOCTL_ALLOC,&a)<0){perror("alloc");exit(1);}close(h);return a.fd;}
static void sy(int fd,int f){struct dma_buf_sync s={.flags=f};ioctl(fd,DMA_BUF_IOCTL_SYNC,&s);}
int main(int c,char**v){
  int SW=atoi(v[1]),SH=atoi(v[2]),DW=atoi(v[3]),DH=atoi(v[4]); int variant=c>5?atoi(v[5]):0;
  int g=open("/dev/g2d",O_RDWR); int sfd=alloc_(SW*SH*4), dfd=alloc_(DW*DH*4);
  unsigned*s=mmap(0,SW*SH*4,PROT_READ|PROT_WRITE,MAP_SHARED,sfd,0); unsigned char*d=mmap(0,DW*DH*4,PROT_READ|PROT_WRITE,MAP_SHARED,dfd,0);
  for(int i=0;i<SW*SH;i++) s[i]=0xFF000000|((i%SW)*255/SW)<<16|((i/SW)*255/SH)<<8|0x40;
  memset(d,0xAA,DW*DH*4); sy(sfd,DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE); sy(dfd,DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE);
  g2d_blt_h b; memset(&b,0,sizeof b); b.flag_h=G2D_BLT_NONE_H;
  g2d_image_enh *S=&b.src_image_h,*D=&b.dst_image_h;
  S->format=D->format=G2D_FORMAT_ARGB8888; S->fd=sfd; D->fd=dfd; S->width=SW;S->height=SH;D->width=DW;D->height=DH;
  S->clip_rect=(g2d_rect){0,0,SW,SH}; D->clip_rect=(g2d_rect){0,0,DW,DH};
  S->mode=D->mode=G2D_PIXEL_ALPHA; S->alpha=D->alpha=255; S->color_range=D->color_range=COLOR_RANGE_0_255;
  if(variant&1){ S->gamut=D->gamut=G2D_BT601; }
  if(variant&2){ S->bpremul=D->bpremul=0; S->align[0]=SW*4; D->align[0]=DW*4; }   /* explicit stride */
  if(variant&4){ S->resize=(g2d_size){DW,DH}; }
  if(variant&8){ D->coor=(g2d_coor){0,0}; D->resize=(g2d_size){DW,DH}; }
  if(variant&256){ b.flag_h=G2D_ROT_90; D->width=DH; D->height=DW; D->clip_rect=(g2d_rect){0,0,DH,DW}; }
  if(variant&128){ S->bbuff=1; D->bbuff=1; }
  if(variant&16){ g2d_fillrect_h f; memset(&f,0,sizeof f); f.dst_image_h=*S; f.dst_image_h.color=0xFF336699; int fr=ioctl(g,G2D_CMD_FILLRECT_H,&f); printf("  src filled by G2D rc=%d\n",fr); }
  if(variant&32){ S->mode=G2D_GLOBAL_ALPHA; D->mode=G2D_GLOBAL_ALPHA; }
  if(variant&64){ S->mode=G2D_PIXEL_ALPHA; D->mode=G2D_GLOBAL_ALPHA; }
  int rc=ioctl(g,G2D_CMD_BITBLT_H,&b);
  if(getenv("TIME")){ struct rusage r0; getrusage(RUSAGE_SELF,&r0); struct timespec a,z; clock_gettime(CLOCK_MONOTONIC,&a); int N=50; for(int i=0;i<N;i++) rc=ioctl(g,G2D_CMD_BITBLT_H,&b); clock_gettime(CLOCK_MONOTONIC,&z); struct rusage ru; getrusage(RUSAGE_SELF,&ru); double cpu=((ru.ru_utime.tv_sec+ru.ru_stime.tv_sec)-(r0.ru_utime.tv_sec+r0.ru_stime.tv_sec))*1e3+((ru.ru_utime.tv_usec+ru.ru_stime.tv_usec)-(r0.ru_utime.tv_usec+r0.ru_stime.tv_usec))/1e3; printf("  %.2f ms/call wall, process cpu total %.1f ms over %d calls = %.2f ms/call\n",((z.tv_sec-a.tv_sec)*1e3+(z.tv_nsec-a.tv_nsec)/1e6)/N,cpu,N,cpu/N); }
  sy(dfd,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ);
  unsigned*dd=(unsigned*)d; long nz=0; if(variant&256){ int ow=DH; printf("  rot: top-left %08x top-right %08x bottom-left %08x bottom-right %08x\n",dd[0],dd[ow-1],dd[(DW-1)*ow],dd[DW*ow-1]); } for(int i=0;i<DW*DH;i++) nz+=(dd[i]!=0xAAAAAAAA);
  if(!(variant&256)){ long se=0; int mx=0; sy(dfd,DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ); for(int y=0;y<DH;y++)for(int x=0;x<DW;x++){ unsigned o=dd[y*DW+x]; int r=(o>>16)&255,gch=(o>>8)&255; int sx=(int)((x+0.5)*SW/DW), sy_=(int)((y+0.5)*SH/DH); int er=(sx*255/SW), eg=(sy_*255/SH); int e=abs(r-er)>abs(gch-eg)?abs(r-er):abs(gch-eg); se+=e; if(e>mx)mx=e; } printf("  gradient error vs ideal: mean %.2f max %d (of 255)\n",(double)se/(DW*DH),mx); }
  printf("variant %2d  %dx%d->%dx%d  rc=%d  changed px %ld/%d  first %08x mid %08x\n",variant,SW,SH,DW,DH,rc,nz,DW*DH,dd[0],dd[DW*DH/2+DW/2]);
  return 0;}
