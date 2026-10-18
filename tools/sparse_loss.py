"""在【稀疏采样】的引擎日志里找"单帧丢失"。

探针每帧不一定采到（回读要 Map，忙时跳过），所以不能按帧号找相邻帧，
只能按"采样序列"判定：某个采样点与它前后两个采样点都明显不同、而前后两点彼此接近
=> 中间那次采样拍到了一次瞬态丢失。
"""
import re
import sys


def main(path, thr):
    seq = []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.search(r'PixelProbe\] f=(\d+) g=([0-9a-f,]+)', line)
        if not m:
            continue
        vals = [int(x, 16) for x in m.group(2).split(',')]
        mean = sum((v >> 16) & 0xff for v in vals) / len(vals)
        seq.append((int(m.group(1)), round(mean, 2)))
    print('采样点: %d   帧号范围 %s..%s' % (len(seq), seq[0][0] if seq else '-', seq[-1][0] if seq else '-'))
    if len(seq) < 3:
        return
    # 帧间采样间隔
    gaps = [seq[i][0] - seq[i - 1][0] for i in range(1, len(seq))]
    gaps_sorted = sorted(gaps)
    print('采样间隔: 中位数 %d 帧, 最大 %d 帧' % (gaps_sorted[len(gaps) // 2], gaps_sorted[-1]))
    hits = []
    for i in range(1, len(seq) - 1):
        a, b, c = seq[i - 1][1], seq[i][1], seq[i + 1][1]
        if abs(b - a) > thr and abs(b - c) > thr and abs(c - a) <= thr * 0.5:
            hits.append((seq[i][0], a, b, c))
    print('瞬态丢失: %d 处' % len(hits))
    for f, a, b, c in hits[:15]:
        print('   f=%-6d  %.2f -> %.2f -> %.2f' % (f, a, b, c))
    if len(hits) > 15:
        print('   ... 共 %d 处' % len(hits))
    vals = [v for _, v in seq]
    print('亮度: min=%.2f max=%.2f 中位=%.2f' % (min(vals), max(vals), sorted(vals)[len(vals) // 2]))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'Game.log', float(sys.argv[2]) if len(sys.argv) > 2 else 6.0)
