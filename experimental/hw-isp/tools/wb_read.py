import numpy as np,sys
W,H,HP=1920,1080,1088
def rgb(f):
    d=np.fromfile(f,np.uint8); y=d[:W*HP].reshape(HP,W)[:H].astype(np.float32)
    u=d[W*HP:W*HP+W*HP//4].reshape(HP//2,W//2).astype(np.float32); v=d[W*HP+W*HP//4:W*HP+W*HP//2].reshape(HP//2,W//2).astype(np.float32)
    sl=(slice(300,780),slice(600,1320)); Y=y[sl].mean(); U=u[150:390,300:660].mean()-128; V=v[150:390,300:660].mean()-128
    R=Y+1.402*V; G=Y-0.344*U-0.714*V; B=Y+1.772*U
    lin=np.array([R,G,B]).clip(1,255)/255.0; lin=lin**2.2
    return Y,R,G,B,lin[0]/lin[1],lin[2]/lin[1]
for f in sys.argv[1:]:
    Y,R,G,B,rg,bg=rgb(f); print("%s luma %.0f  RGB %.0f %.0f %.0f  linear R/G %.3f B/G %.3f  -> light entry r=%d b=%d"%(f,Y,R,G,B,rg,bg,round(rg*256),round(bg*256)))
