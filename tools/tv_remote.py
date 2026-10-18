"""电视远程画面 + 远程操作（无需安装 scrcpy）。

原理：
  画面 - `adb exec-out screenrecord --output-format=h264 -` 持续输出 H.264 流，
         交给 ffmpeg 解成裸帧，窗口里显示。
  操作 - 游戏的 `input keyevent` 通道无效（实测被丢弃），只能走引擎的调试钩子：
         把 scancode 写进 urge_debug/key.txt，游戏每帧消费一个。

用法：
    python tools/tv_remote.py                    # 默认 192.168.1.5:5555
    python tools/tv_remote.py 192.168.1.8:5555   # 换设备（手机等）

按键映射（SDL scancode，与 key.txt 一致）：
    ← → ↑ ↓     80 79 81 82
    Enter       40        Esc       41
    Z X C       44 45 46   A S D     29 31 32
    Q W         20 22      F G L     9 10 15
    F4 F5       61 62

已知限制（重要）：
  - 画面走的是 SurfaceFlinger 合成输出；有些只出现在扫描输出（硬件叠加层）上的
    单帧异常，这条路径拍不到。验收这类问题仍需肉眼直接看面板。
  - screenrecord 单次最长 180 秒，脚本会自动续接。
"""

import os
import queue
import subprocess
import sys
import threading
import tkinter as tk

from PIL import Image, ImageTk

DEVICE = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.5:5555"

KEYBOX = ("/storage/emulated/0/Android/data/com.admenri.urge/files/"
          "urge_debug/key.txt")

# 窗口显示尺寸（越小越省 CPU；解码尺寸由 --size 决定）
DECODE_W, DECODE_H = 640, 360   # 1920x1080 / 3，都是偶数：奇数会让编码器改尺寸，按固定尺寸切帧就会滚动

# tkinter 按键名 -> SDL scancode
KEYMAP = {
    # SDL scancodes（SDL2/SDL3 一致的 HID 用码），取自 content/input/keyboard_controller.cc 的绑定：
    #   B(取消) = X / ESC / KP_0      C(确认) = Z / C / SPACE / ENTER
    #   L = Q                        R = W
    "x": 27, "X": 27,
    "z": 29, "Z": 29,   # 未绑定，仅占位
    "c": 6, "C": 6,
    "a": 4, "A": 4,
    "s": 22, "S": 22,
    "d": 7, "D": 7,
    "q": 20, "Q": 20,
    "w": 26, "W": 26,
    "f": 9, "F": 9,
    "g": 10, "G": 10,
    "l": 15, "L": 15,
    "Return": 40, "KP_Enter": 40,
    "space": 44,
    "Left": 80, "Right": 79, "Up": 81, "Down": 82,
    "F4": 63, "F5": 64,
}

frames = queue.Queue(maxsize=2)
stop = threading.Event()

# 每帧把"罗盘区亮度"写进 csv，外部（AI）可以边看边分析；用户只管操作。
METRICS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "..", "tmp_ana", "tv_metrics.csv")
COMPASS_BOX = (485, 287, 540, 340)   # 640x360 下罗盘所在的小框
_metric_frame = 0
_metric_prev = None
_metric_file = None
_ring = []            # 最近若干帧，尖峰时落盘
_SPIKE_DELTA = 5.0    # 罗盘框均值跳变阈值


def _dump_spike():
    """把环形缓冲存成图片，尖峰前后各若干帧，供离线对比。"""
    try:
        out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "..", "tmp_ana")
        tag = _metric_frame
        for i, im in enumerate(_ring):
            im.save(os.path.join(out, "spike_%06d_%02d.png" % (tag, i)))
    except Exception:
        pass


def record_metric(img):
    global _metric_frame, _metric_prev, _metric_file
    try:
        if _metric_file is None:
            _metric_file = open(METRICS_PATH, "w", buffering=1)
            _metric_file.write("frame,box_mean,delta\n")
        v = sum(img.convert("L").crop(COMPASS_BOX).get_flattened_data())
        n = (COMPASS_BOX[2] - COMPASS_BOX[0]) * (COMPASS_BOX[3] - COMPASS_BOX[1])
        mean = v / float(n)
        d = 0.0 if _metric_prev is None else mean - _metric_prev
        _metric_prev = mean
        _metric_frame += 1
        _metric_file.write("%d,%.3f,%.3f\n" % (_metric_frame, mean, d))
        # 尖峰：把前后帧一起存下来（环形缓冲只有当前帧之前的历史，故每帧都存一份最新的）
        _ring.append(img.copy())
        if len(_ring) > 12:
            _ring.pop(0)
        if abs(d) > _SPIKE_DELTA:
            _dump_spike()
    except Exception:
        pass


def adb(*args):
    return subprocess.Popen(["adb", "-s", DEVICE, *args],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)


def video_thread():
    """把 screenrecord 的 H.264 流解成 rgb24 帧，放进 frames。"""
    ffmpeg = None
    try:
        import imageio_ffmpeg
        ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        ffmpeg = "ffmpeg"

    frame_bytes = DECODE_W * DECODE_H * 3
    while not stop.is_set():
        rec = adb("exec-out", "screenrecord",
                  "--output-format=h264",
                  "--bit-rate", "8000000",
                  "--size", "%dx%d" % (DECODE_W, DECODE_H),
                  "--time-limit", "180", "-")
        dec = subprocess.Popen(
            [ffmpeg, "-hide_banner", "-loglevel", "error",
             "-f", "h264", "-i", "pipe:0",
             "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"],
            stdin=rec.stdout, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        try:
            while not stop.is_set():
                buf = dec.stdout.read(frame_bytes)
                if len(buf) < frame_bytes:
                    break
                img = Image.frombytes("RGB", (DECODE_W, DECODE_H), buf)
                if frames.full():
                    try:
                        frames.get_nowait()
                    except queue.Empty:
                        pass
                frames.put(img)
        finally:
            for p in (dec, rec):
                try:
                    p.kill()
                except Exception:
                    pass


def send_key(code):
    """写 scancode；游戏每帧消费一个。"""
    tmp = KEYBOX + ".tmp"
    subprocess.run(["adb", "-s", DEVICE, "shell",
                    "printf '%d\\n' > %s && mv %s %s" % (code, tmp, tmp, KEYBOX)],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    t = threading.Thread(target=video_thread, daemon=True)
    t.start()

    root = tk.Tk()
    root.title("TV %s  (方向键/Enter/Z/X/C…  Esc 退出)" % DEVICE)
    root.configure(bg="black")
    label = tk.Label(root, bg="black")
    label.pack()

    status = tk.Label(root, text="等待按键", bg="black", fg="yellow")

    def on_key(event):
        if event.keysym == "Escape" and not event.char.isalpha():
            stop.set()
            root.destroy()
            return
        code = KEYMAP.get(event.keysym)
        if code is None and event.char:
            code = KEYMAP.get(event.char)
        if code is not None:
            status.config(text="已送出 scancode %d (键 %s)" % (code, event.keysym))
            threading.Thread(target=send_key, args=(code,), daemon=True).start()
        else:
            status.config(text="未映射的键: %s" % event.keysym)

    root.bind("<Key>", on_key)

    def pump():
        try:
            img = frames.get_nowait()
            record_metric(img)
            photo = ImageTk.PhotoImage(img)
            label.configure(image=photo)
            label.image = photo
        except queue.Empty:
            pass
        if not stop.is_set():
            root.after(16, pump)

    status.pack()
    pump()
    root.after(200, lambda: root.focus_force())
    root.mainloop()
    stop.set()


if __name__ == "__main__":
    main()
