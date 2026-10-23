#!/usr/bin/env python3
# Flat-field map from averaged flat-white frames, robust to hot spots/bezel: mask bad pixels, fit a smooth 2-D polynomial per channel in linear light.
import numpy as np, glob, subprocess, sys, os
from PIL import Image, ImageFilter
D=sys.argv[1]; W,H,HP=1920,1080,1088
acc=None; n=0
for f in sorted(glob.glob(D+"/flat_*_1920x1080.yuv"))[2:]:        # skip first frames (AE settling)
    raw=np.fromfile(f,dtype=np.uint8)
    y=raw[:W*HP].reshape(HP,W)[:H].astype(np.float32)
    u=raw[W*HP:W*HP+W*HP//4].reshape(HP//2,W//2)[:H//2].astype(np.float32)
    v=raw[W*HP+W*HP//4:].reshape(HP//2,W//2)[:H//2].astype(np.float32)
    u=np.repeat(np.repeat(u,2,0),2,1)-128; v=np.repeat(np.repeat(v,2,0),2,1)-128
    r=y+1.402*v; g=y-0.344136*u-0.714136*v; b=y+1.772*u
    rgb=np.clip(np.stack([r,g,b],-1),0,255)
    acc=rgb if acc is None else acc+rgb; n+=1
A=acc/n; print("frames averaged",n)
mx=A.max(2); mn=A.min(2)
bad=(mx>=240)|(A.mean(2)<60)|((A[...,1]-np.maximum(A[...,0],A[...,2]))>25)          # clipped spot, dark bezel/table, green wall
bad=np.asarray(Image.fromarray((bad*255).astype(np.uint8)).filter(ImageFilter.MaxFilter(81)))>0
good=~bad; print("valid fraction",round(float(good.mean()),3))
L=(np.clip(A,1,255)/255.0)**2.2
yy,xx=np.mgrid[0:H,0:W]; xn=(xx/(W-1))*2-1; yn=(yy/(H-1))*2-1
ord_=4
terms=[(i,j) for i in range(ord_+1) for j in range(ord_+1-i)]
def design(x,y): return np.stack([x**i*y**j for i,j in terms],-1)
sel=np.flatnonzero(good.ravel())[::37]
Xs=design(xn.ravel()[sel],yn.ravel()[sel])
gy,gx=np.mgrid[0:36,0:64]; gxn=((gx+0.5)/64)*2-1; gyn=((gy+0.5)/36)*2-1
Xg=design(gxn.ravel(),gyn.ravel())
fit=np.zeros((36,64,3))
for c in range(3):
    coef,*_=np.linalg.lstsq(Xs,L[...,c].ravel()[sel],rcond=None)
    pred=Xs@coef; res=np.abs(pred-L[...,c].ravel()[sel])/np.maximum(pred,1e-3)
    print("ch",c,"median fit residual %.3f  p95 %.3f"%(np.median(res),np.percentile(res,95)))
    fit[...,c]=(Xg@coef).reshape(36,64)
centre=fit[16:20,30:34].reshape(-1,3).mean(0)
gain=np.clip(centre/np.maximum(fit,1e-3),0.7,3.0)
print("gain range per channel:",[(round(float(gain[...,c].min()),2),round(float(gain[...,c].max()),2)) for c in range(3)])
print("corner gains TL,TR,BL,BR (R,G,B):",gain[0,0].round(2),gain[0,-1].round(2),gain[-1,0].round(2),gain[-1,-1].round(2))
np.stack([gain[...,0],gain[...,1],gain[...,2]]).astype("<f4").tofile("flatmap.bin"); print("wrote flatmap.bin")
Image.fromarray((np.stack([good]*3,-1)*255).astype(np.uint8)).save(D+"/mask.png")
