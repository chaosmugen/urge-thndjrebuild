#!/bin/bash
# 接缝/罗盘闪烁的一次测量：设备录屏 -> 拉回 -> 抽帧 -> 分块三帧判据。
# 只读，不改任何游戏状态。输出：闪烁最多的区块与次数。
set -e
D=192.168.1.5:5555
TAG=${1:-m0}
SEC=${2:-25}
cd "$(dirname "$0")/.."

sleep 4
MSYS_NO_PATHCONV=1 adb -s $D shell "rm -f /sdcard/$TAG.mp4; screenrecord --time-limit $SEC --bit-rate 8000000 --size ${SW:-320}x${SH:-180} /sdcard/$TAG.mp4"
rm -f "tmp_ana/$TAG.mp4"
MSYS_NO_PATHCONV=1 adb -s $D pull "/sdcard/$TAG.mp4" "tmp_ana/$TAG.mp4" >/dev/null

rm -rf "tmp_ana/p$TAG"; mkdir -p "tmp_ana/p$TAG"
FF=$(python -c "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())")
"$FF" -hide_banner -loglevel error -i "tmp_ana/$TAG.mp4" -vsync 0 "tmp_ana/p$TAG/f_%05d.png" 2>/dev/null

python -c "
import glob, numpy as np
from PIL import Image
fs=sorted(glob.glob('tmp_ana/p$TAG/f_*.png'))
a=np.stack([np.asarray(Image.open(f).convert('L'),dtype=np.float32) for f in fs])
H,W=a.shape[1],a.shape[2]; bh,bw=H//9,W//16
res=[]
for r in range(9):
    for c in range(16):
        blk=a[:, r*bh:(r+1)*bh, c*bw:(c+1)*bw].mean(axis=(1,2))
        n=sum(1 for i in range(1,len(blk)-1)
              if abs(blk[i]-blk[i-1])>4 and abs(blk[i]-blk[i+1])>4 and abs(blk[i+1]-blk[i-1])<1.6)
        if n>0: res.append((n,c*bw,r*bh,bw,bh))
res.sort(reverse=True)
print('帧数 %d  有瞬态的区块 %d' % (len(fs), len(res)))
if len(fs) < 400:
    print('  !! 帧数过少，本次数据不可用（screenrecord 被占用或中断）')
    raise SystemExit
if res:
    tot=sum(n for n,_,_,_,_ in res)
    n,x,y,w,h=res[0]
    print('  最强区块 %d 次 (全分辨率 %d,%d)-(%d,%d)' % (n,x*3,y*3,(x+w)*3,(y+h)*3))
    print('  各区块次数合计 %d  => 每帧 %.2f%%' % (tot, 100.0*tot/len(fs)))
else:
    print('  无瞬态')
"