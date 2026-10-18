"""接缝"相对深度"判据 —— 跨平台可比的正确性度量。

用法:
    python tools/seam_metric.py <截图...>

背景（务必先读）:
  接缝 = 两个半透明窗口层重叠的那几行。窗口边缘本身是半透明的，所以那条带的最终
  颜色 = 面板自身 × 背后的东西。判断它"对不对"不能看绝对亮度（背景不同、昼夜不同、
  面板体亮度也不同），必须:
    1. 与**参照平台**比 —— 参照值来自 PJZ110(Adreno) 实测: 相对深度 ≈ 0.10
    2. 取**竖直剖面**而不是单个像素 —— 单点会被面板上的文字、圆角、装饰带偏
    3. 用**相对深度**(最暗行 / 面板体亮度)归一化 —— 这样背景差异被除掉
  电视(Mali/Vulkan) 实测存在两档: 0.098(对) 与 0.210(错, 深一倍, 即重叠区被多合成一次)。

判据: 所有帧的相对深度都应落在参照附近(≈0.10)。出现 0.2x 那一档即为复现。
"""
import sys
from PIL import Image

# 面板内部的横向取样段（避开文字与圆角）与竖直搜索范围，按设备分辨率给出
REGIONS = {
    # 1920x1080: 游戏 800x600 以 1.8 倍居中, x 偏移 240
    (1920, 1080): dict(x0=1560, x1=1700, y0=955, y1=1005, body_y=955, body_y1=966),
    # 3168x1440: 手机(参照)
    (3168, 1440): dict(x0=2340, x1=2520, y0=1270, y1=1340, body_y=1270, body_y1=1295),
}


def seam_metric(path):
    im = Image.open(path).convert('L')
    r = REGIONS.get(im.size)
    if r is None:
        raise SystemExit('没有为 %s 配置取样区域，请先量一次坐标再补进 REGIONS' % (im.size,))
    rows = [(y, sum(im.getpixel((x, y)) for x in range(r['x0'], r['x1'])) // (r['x1'] - r['x0']))
            for y in range(r['y0'], r['y1'])]
    body_rows = [v for y, v in rows if r['body_y'] <= y <= r['body_y1']]
    body = sum(body_rows) / len(body_rows)
    dark_y, dark_v = min(rows, key=lambda t: t[1])
    return dark_v, body, dark_v / body, dark_y


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    print('   截图            最暗R   面板体   相对深度  最暗行')
    vals = []
    for p in argv[1:]:
        d, b, ratio, y = seam_metric(p)
        vals.append(ratio)
        flag = 'OK' if ratio < 0.15 else 'BAD(叠加了两次?)'
        print('  %-22s %4d  %6.1f   %.3f   y=%-5d %s' % (p.split('/')[-1], d, b, ratio, y, flag))
    if vals:
        print()
        print('相对深度: min=%.3f max=%.3f 中位数=%.3f   参照 ≈0.10' % (min(vals), max(vals), sorted(vals)[len(vals) // 2]))
        bad = [v for v in vals if v >= 0.15]
        if bad:
            print('BAD 帧: %d / %d  (%.0f%%)' % (len(bad), len(vals), 100.0 * len(bad) / len(vals)))
        else:
            print('全部落在参照档内 (OK)')


if __name__ == '__main__':
    main(sys.argv)
