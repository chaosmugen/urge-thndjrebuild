"""单帧丢失检测（对动画免疫）。

原理:
  某一帧把一块区域"丢"了, 它就会与前一帧、后一帧都不同, 而前后两帧彼此相似 ——
  这与"画面在动"(前后帧本来就不同)可以区分开, 也不需要参照平台。

用法:
    python tools/frame_loss.py <帧目录> [--box x0,y0,x1,y1] [--thr 12]

输出: 每个"瞬态帧"的编号、差异像素数、以及该帧异常区域的包围盒。
"""
import sys, glob, os
from PIL import Image, ImageChops


def diff_count(a, b, thr):
    d = ImageChops.difference(a, b).convert('L')
    n = 0
    bbox = None
    for v in d.get_flattened_data():
        if v > thr:
            n += 1
    if n:
        bbox = d.point(lambda v: 255 if v > thr else 0).getbbox()
    return n, bbox


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    d = argv[1]
    box = None
    thr = 12
    for i, a in enumerate(argv):
        if a == '--box':
            box = tuple(int(x) for x in argv[i + 1].split(','))
        if a == '--thr':
            thr = int(argv[i + 1])
    fs = sorted(glob.glob(os.path.join(d, '*.png')))
    if len(fs) < 3:
        raise SystemExit('帧数不足')
    ims = []
    for f in fs:
        im = Image.open(f).convert('RGB')
        ims.append(im.crop(box) if box else im)
    w, h = ims[0].size
    print('帧数 %d   区域 %dx%d%s' % (len(ims), w, h, ('  box=%s' % (box,)) if box else ''))
    losses = []
    for i in range(1, len(ims) - 1):
        d_prev, _ = diff_count(ims[i - 1], ims[i], thr)
        d_next, _ = diff_count(ims[i], ims[i + 1], thr)
        d_pn, _ = diff_count(ims[i - 1], ims[i + 1], thr)
        # 瞬态: 与前后都不同, 但前后彼此相似
        if d_prev > 200 and d_next > 200 and d_pn < min(d_prev, d_next) * 0.35:
            _, bb = diff_count(ims[i - 1], ims[i], thr)
            losses.append((i + 1, d_prev, d_next, d_pn, bb))
    print('瞬态帧(单帧丢失)数量: %d / %d' % (len(losses), len(ims)))
    for n, a, b, c, bb in losses[:12]:
        print('  帧%-4d 与前 %-6d 与后 %-6d 前后互差 %-5d  异常框 %s' % (n, a, b, c, bb))
    if len(losses) > 12:
        print('  ... 共 %d 帧' % len(losses))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
