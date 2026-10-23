import numpy as np, sys, os, json
W,H,HP=1920,1080,1088
f=sys.argv[1]; verbose=len(sys.argv)>2
d=np.fromfile(f,dtype=np.uint8)
y=d[:W*HP].reshape(HP,W)[:H].astype(float)
u=np.repeat(np.repeat(d[W*HP:W*HP+W*HP//4].reshape(HP//2,W//2)[:H//2].astype(float),2,0),2,1)
v=np.repeat(np.repeat(d[W*HP+W*HP//4:W*HP+W*HP//2].reshape(HP//2,W//2)[:H//2].astype(float),2,0),2,1)
rgb=np.stack([np.clip(y+1.402*(v-128),0,255),np.clip(y-0.344*(u-128)-0.714*(v-128),0,255),np.clip(y+1.772*(u-128),0,255)],-1)
small=rgb.reshape(540,2,960,2,3).mean((1,3))                         # 960x540
ref=np.load(os.environ.get("CHART_REF", "chart_ref.npy")).reshape(24,3)
G=2.2; lin=lambda a:(np.clip(a,0,255)/255.0)**G; enc=lambda l:np.clip(l,0,1)**(1/G)*255
ii=np.pad(small,((1,0),(1,0),(0,0))).cumsum(0).cumsum(1)
def box(cx,cy,r):
    x0,x1,y0,y1=int(cx-r),int(cx+r),int(cy-r),int(cy+r)
    if x0<0 or y0<0 or x1>=960 or y1>=540: return None
    return (ii[y1,x1]-ii[y0,x1]-ii[y1,x0]+ii[y0,x0])/((x1-x0)*(y1-y0))
Lt=lin(ref); Ct=Lt/np.maximum(Lt.sum(1,keepdims=True),1e-6)
Lobs=lin(small)                                   # 960x540 linear
Lh=Lobs.reshape(270,2,480,2,3).mean((1,3))        # 480x270 for the search
Xs,Ys=np.meshgrid(np.arange(480)*2+1.0,np.arange(270)*2+1.0)   # pixel centres in 960-scale coords
def to_grid(g,X,Y):
    x0,y0,px,py,th=g
    ct,st=np.cos(th),np.sin(th)
    dx,dy=X-x0,Y-y0
    gu=(dx*ct+dy*st)/px+0.5; gv=(-dx*st+dy*ct)/py+0.5
    return gu,gv
def fit_err(g,full=False):
    gu,gv=to_grid(g,Xs,Ys)
    cc=np.floor(gu).astype(int); rr=np.floor(gv).astype(int)
    fx=gu%1; fy=gv%1
    m=(cc>=0)&(cc<6)&(rr>=0)&(rr<4)&(np.abs(fx-.5)<.30)&(np.abs(fy-.5)<.30)
    if m.sum()<2000: return 1e9
    T=Lt[(rr*6+cc)[m]]; O=Lh[m]
    gains=(T*O).sum(0)/np.maximum((O*O).sum(0),1e-9)
    return float(((O*gains-T)**2).sum()/m.sum())
best=(1e9,None)
for th in (-0.15,-0.1,-0.05,0.0,0.05,0.1,0.15):
 for x0 in range(190,340,10):
  for y0 in range(110,280,10):
    for px in (60,64,68,72):
      for py in (60,64,68,72):
        o=fit_err((x0,y0,px,py,th))
        if o<best[0]: best=(o,(x0,y0,px,py,th))
g=list(best[1])
for step in (3,1.5,0.75):
    improved=True
    while improved:
        improved=False
        for k in range(5):
            for sg in (-step,step):
                q=list(g); q[k]+=sg*(0.01 if k==4 else 1); o=fit_err(q)
                if o<best[0]-1e-9: best=(o,tuple(q)); g=q; improved=True
def grid_means(x0,y0,px,py,th,r=12):
    m=[]
    ct,st=np.cos(th),np.sin(th)
    for rr in range(4):
        for cc in range(6):
            bb=box(x0+cc*px*ct-rr*py*st,y0+cc*px*st+rr*py*ct,r)
            if bb is None: return None
            m.append(bb)
    return np.array(m)
x0,y0,px,py,th=g
meas=grid_means(x0,y0,px,py,th,r=14)
tgt=ref.copy()
keep=(meas.max(1)<236)&(tgt.max(1)>30)
gi=[i for i in (18,19,20,21) if meas[i].max()<236]
Lg=lin(meas[gi]); Tg=lin(tgt[gi]); gains=np.array([(Tg[:,c]*Lg[:,c]).sum()/(Lg[:,c]**2).sum() for c in range(3)]); gains/=gains[1]
if os.environ.get("WBNORM","1")=="1": meas=enc(lin(meas)*gains)
Lm=lin(meas); s=(Lm[keep]*Lt[keep]).sum()/(Lm[keep]**2).sum()
e=np.sqrt(((enc(Lm*s)-tgt)**2).sum(1))
cm=Lm*s; cm=cm/np.maximum(cm.sum(1,keepdims=True),1e-6); ct=Lt/np.maximum(Lt.sum(1,keepdims=True),1e-6); ce=np.sqrt(((cm-ct)**2).sum(1))*100
np.save("/tmp/patches_meas.npy",meas); np.save("/tmp/patches_tgt.npy",tgt); np.save("/tmp/patches_keep.npy",keep)
print("SCORE %.1f (brightness-matched RGB err, %d patches)  chroma err %.1f   grid=[%.2f, %.2f, %.2f, %.2f, %.4f] match=%.4f"%(e[keep].mean(),keep.sum(),ce[keep].mean(),*g,best[0]))
if verbose:
    for i in range(24): print((i//6,i%6),[int(a) for a in tgt[i]],[int(a) for a in meas[i]],"err %3d chroma %4.1f"%(e[i],ce[i]))
