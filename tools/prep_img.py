"""prep_img.py - 生成替换用头像图片（512 / 256 / 128）

用法:
    python prep_img.py --src 头像.jpg [--mask 原版照片.png] [--out 输出目录] [--flip none|v|h]

参数:
    --src   源图片路径（自动居中裁剪为正方形后缩放）
    --mask  可选：游戏原版照片，用于提取圆形遮罩 alpha，并把圆外区域置黑
    --out   输出目录（默认脚本所在目录）
    --flip  翻转方向：v = 上下翻转（默认），h = 左右翻转，none = 不翻转

输出:
    custom_512.png / custom_256.png / custom_128.png
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="源图片路径")
    ap.add_argument("--mask", default=None, help="原版照片路径（提取圆形遮罩）")
    ap.add_argument("--out", default=None, help="输出目录（默认脚本所在目录）")
    ap.add_argument("--flip", default="v", choices=("none", "v", "h"), help="翻转方向")
    args = ap.parse_args()

    out_dir = args.out or os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out_dir, exist_ok=True)

    img = Image.open(args.src)
    w, h = img.size
    side = min(w, h)
    box = ((w - side) // 2, (h - side) // 2, (w + side) // 2, (h + side) // 2)
    sq = img.crop(box)

    if args.flip == "v":
        sq = sq.transpose(Image.FLIP_TOP_BOTTOM)
    elif args.flip == "h":
        sq = sq.transpose(Image.FLIP_LEFT_RIGHT)

    base = sq.resize((512, 512), Image.LANCZOS).convert("RGBA")
    data = np.asarray(base).copy()

    if args.mask:
        orig = Image.open(args.mask).convert("RGBA")
        alpha = np.asarray(orig)[:, :, 3].copy()
        data[:, :, 3] = alpha
        outside = alpha == 0
        data[outside, 0] = 0
        data[outside, 1] = 0
        data[outside, 2] = 0
        print("mask alpha mean:", round(float(alpha.mean()), 1))

    for size in (512, 256, 128):
        out = Image.fromarray(data, "RGBA") if size == 512 else Image.fromarray(data, "RGBA").resize((size, size), Image.LANCZOS)
        path = os.path.join(out_dir, f"custom_{size}.png")
        out.save(path)
        print("saved", path)

    return 0


if __name__ == "__main__":
    sys.exit(main())