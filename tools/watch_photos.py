"""watch_photos.py - 监视游戏照片目录，把新照片替换为自定义图片

用途:
    游戏拍照后会在本地写入照片文件；本脚本监听到新照片后立即用自定义图片
    替换它，并把原始文件备份一次（*.watcherbak），同时记录服务器资源更新。

用法:
    python watch_photos.py --self-dir <照片目录> [--profile-dir <资源目录>] \\
        [--custom512 a.png] [--custom256 b.png] [--custom128 c.png] [--log watch.log]

提示:
    如果注入本身已生效（照片数据已在渲染阶段被替换），此脚本是可选辅助，
    仅用于让本地预览文件也显示自定义图片。
"""
import argparse
import hashlib
import logging
import os
import shutil
import sys
import time


def logprint(log, *parts):
    msg = " ".join(str(x) for x in parts)
    print(msg)
    if log:
        log.info(msg)


def sha(path):
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[:16]
    except Exception:
        return "?"


def replace_once(path, custom):
    bak = path + ".watcherbak"
    if not os.path.exists(bak):
        try:
            shutil.copy2(path, bak)
        except Exception as exc:
            logprint(None, "backup fail", path, exc)
            return False
    try:
        shutil.copy2(custom, path)
        return True
    except Exception:
        try:
            shutil.copyfile(custom, path)
            return True
        except Exception as exc:
            logprint(None, "replace fail", path, exc)
            return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-dir", required=True, help="游戏照片目录")
    ap.add_argument("--profile-dir", default=None, help="服务器资源目录（仅记录日志）")
    ap.add_argument("--custom512", default=None)
    ap.add_argument("--custom256", default=None)
    ap.add_argument("--custom128", default=None)
    ap.add_argument("--log", default=None, help="日志文件路径")
    ap.add_argument("--timeout-hours", type=float, default=8.0, help="运行时长上限")
    args = ap.parse_args()

    log = None
    if args.log:
        logging.basicConfig(filename=args.log, level=logging.INFO,
                            format="%(asctime)s %(message)s", encoding="utf-8")
        log = logging.getLogger("watcher")

    customs = {}
    for size, path in (("512", args.custom512), ("256", args.custom256), ("128", args.custom128)):
        if path:
            customs[size] = os.path.abspath(path)

    seen = set()
    deadline = time.time() + args.timeout_hours * 3600
    logprint(log, "watching", args.self_dir, "customs:", customs)

    while time.time() < deadline:
        try:
            for name in os.listdir(args.self_dir):
                full = os.path.join(args.self_dir, name)
                if not os.path.isfile(full):
                    continue
                st = os.stat(full)
                key = (name, st.st_size, int(st.st_mtime))
                if key in seen:
                    continue
                seen.add(key)
                if name.startswith("NormalSize"):
                    for size, custom in customs.items():
                        if size in name or size == "512":
                            ok = replace_once(full, custom)
                            logprint(log, "NEW photo:", name, st.st_size,
                                     "-> replaced" if ok else "replace failed")
                            break
                elif name.startswith("ThumbnailSize"):
                    custom = customs.get("128") or customs.get("256")
                    if custom:
                        ok = replace_once(full, custom)
                        logprint(log, "NEW thumb:", name, st.st_size,
                                 "-> replaced" if ok else "replace failed")
                elif args.profile_dir and name != "index.bytes":
                    logprint(log, "SERVER RESOURCE:", name, st.st_size, "sha", sha(full))
        except OSError as exc:
            logprint(log, "scan error:", exc)
        time.sleep(1)

    logprint(log, "timeout, exit")
    return 0


if __name__ == "__main__":
    sys.exit(main())