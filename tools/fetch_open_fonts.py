#!/usr/bin/env python3
"""Fetch OFL-licensed open fonts used to generate e-paper bitmap fonts."""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

OFL_NOTICE = """SIL Open Font License 1.1

The generated firmware bitmap fonts are derived from open fonts.
Official license text and sources:

- https://github.com/lxgw/975maru
- https://github.com/notofonts/noto-fonts/blob/main/LICENSE
- https://openfontlicense.org/

If the build host already has these open fonts installed, this script may copy
those local font files into the configured cache directory to avoid unreliable
large downloads. Do not replace them with proprietary OS-bundled fonts for public
firmware releases.
"""


FILES = {
    "LXGW975YuanSC-400W.ttf": {
        "url": "https://raw.githubusercontent.com/lxgw/975maru/26.06.20/TTF/LXGW975YuanSC-400W.ttf",
        "fallback_urls": [
            "https://cdn.jsdelivr.net/gh/lxgw/975maru@26.06.20/TTF/LXGW975YuanSC-400W.ttf",
            "https://fastly.jsdelivr.net/gh/lxgw/975maru@26.06.20/TTF/LXGW975YuanSC-400W.ttf",
            "https://gcore.jsdelivr.net/gh/lxgw/975maru@26.06.20/TTF/LXGW975YuanSC-400W.ttf",
        ],
        "min_size": 13 * 1024 * 1024,
    },
    "NotoSansMono-Regular.ttf": {
        "url": "https://raw.githubusercontent.com/notofonts/noto-fonts/main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
        "fallback_urls": [
            "https://cdn.jsdelivr.net/gh/notofonts/noto-fonts@main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
            "https://fastly.jsdelivr.net/gh/notofonts/noto-fonts@main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
            "https://gcore.jsdelivr.net/gh/notofonts/noto-fonts@main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf",
        ],
        "min_size": 300 * 1024,
    },
    "LICENSE-Noto-Fonts.txt": {
        "url": "https://raw.githubusercontent.com/notofonts/noto-fonts/main/LICENSE",
        "fallback_urls": [
            "https://cdn.jsdelivr.net/gh/notofonts/noto-fonts@main/LICENSE",
            "https://fastly.jsdelivr.net/gh/notofonts/noto-fonts@main/LICENSE",
            "https://gcore.jsdelivr.net/gh/notofonts/noto-fonts@main/LICENSE",
        ],
        "min_size": 80,
    },
    "LICENSE-LXGW975Yuan.txt": {
        "url": "https://raw.githubusercontent.com/lxgw/975maru/26.06.20/OFL.txt",
        "fallback_urls": [
            "https://cdn.jsdelivr.net/gh/lxgw/975maru@26.06.20/OFL.txt",
            "https://fastly.jsdelivr.net/gh/lxgw/975maru@26.06.20/OFL.txt",
            "https://gcore.jsdelivr.net/gh/lxgw/975maru@26.06.20/OFL.txt",
        ],
        "min_size": 80,
    },
}

LOCAL_FONT_FALLBACKS = {
    "LXGW975YuanSC-400W.ttf": [
        "C:/Windows/Fonts/LXGW975YuanSC-400W.ttf",
    ],
    "NotoSansMono-Regular.ttf": [
        "C:/Windows/Fonts/NotoSansMono-Regular.ttf",
        "C:/Windows/Fonts/NotoSans-Regular.ttf",
    ],
    "LICENSE-Noto-Fonts.txt": [],
    "LICENSE-LXGW975Yuan.txt": [],
}


def configured_source_dirs(extra_dirs: list[str] | None = None) -> list[Path]:
    dirs: list[Path] = []
    for key in ("MIAOO_FONT_SOURCE_DIR", "MIAOO_FONT_CACHE"):
        value = os.environ.get(key, "")
        for part in value.split(os.pathsep):
            if part.strip():
                dirs.append(Path(part.strip()))
    for part in extra_dirs or []:
        if part.strip():
            dirs.append(Path(part.strip()))
    dirs.append(Path.home() / ".miaooaim" / "fonts")

    out: list[Path] = []
    seen: set[str] = set()
    for d in dirs:
        try:
            key = str(d.resolve())
        except OSError:
            key = str(d)
        if key not in seen:
            seen.add(key)
            out.append(d)
    return out


def url_with_prefixes(url: str) -> list[str]:
    urls = [url]
    prefix_value = os.environ.get("MIAOO_FONT_URL_PREFIX", "")
    for prefix in prefix_value.split(os.pathsep):
        prefix = prefix.strip()
        if not prefix:
            continue
        if not prefix.endswith("/"):
            prefix += "/"
        urls.insert(0, prefix + url)
    return urls


def fetch(meta: dict[str, object], dst: Path, min_size: int, timeout: int) -> None:
    if dst.exists() and dst.stat().st_size >= min_size:
        return
    if str(meta.get("url") or "") == "local-ofl-notice":
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(OFL_NOTICE, encoding="utf-8")
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(dst.suffix + ".tmp")
    last_error: Exception | None = None
    urls: list[str] = []
    ordered_urls = [str(url) for url in meta.get("fallback_urls", []) or []]
    ordered_urls.append(str(meta["url"]))
    for url in ordered_urls:
        urls.extend(url_with_prefixes(url))
    for url in urls:
        host = urllib.parse.urlparse(url).netloc or url
        request = urllib.request.Request(url, headers={
            "User-Agent": "epaper-uploader-font-fetch/1.0",
        })
        for attempt in range(1, 4):
            print(f"Downloading {dst.name} from {host} ({attempt}/3) ...")
            try:
                with urllib.request.urlopen(request, timeout=timeout) as response:
                    with tmp.open("wb") as f:
                        while True:
                            chunk = response.read(1024 * 256)
                            if not chunk:
                                break
                            f.write(chunk)
                last_error = None
                break
            except Exception as exc:
                last_error = exc
                tmp.unlink(missing_ok=True)
                time.sleep(2 * attempt)
        if last_error is None:
            break
    if last_error:
        raise last_error
    if tmp.stat().st_size < min_size:
        tmp.unlink(missing_ok=True)
        raise RuntimeError(f"{dst.name} is too small; download likely failed")
    tmp.replace(dst)


def copy_local_fallback(name: str, dst: Path, min_size: int,
                        source_dirs: list[Path]) -> bool:
    if name.startswith("LICENSE-"):
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(OFL_NOTICE, encoding="utf-8")
        return True
    for source_dir in source_dirs:
        src = source_dir / name
        if src.exists() and src.stat().st_size >= min_size:
            dst.parent.mkdir(parents=True, exist_ok=True)
            print(f"Using cached open font {src} -> {dst.name}")
            shutil.copy2(src, dst)
            return True
    for candidate in LOCAL_FONT_FALLBACKS.get(name, []):
        src = Path(candidate)
        if src.exists() and src.stat().st_size >= min_size:
            dst.parent.mkdir(parents=True, exist_ok=True)
            print(f"Using local open font {src} -> {dst.name}")
            shutil.copy2(src, dst)
            return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", default="tools/fonts/noto")
    parser.add_argument("--source-dir", action="append", default=[],
                        help="Optional directory containing already downloaded open font files")
    parser.add_argument("--timeout", type=int,
                        default=int(os.environ.get("MIAOO_FONT_FETCH_TIMEOUT", "300")),
                        help="Per network read timeout in seconds")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    source_dirs = configured_source_dirs(args.source_dir)
    try:
        for name, meta in FILES.items():
            dst = out_dir / name
            if copy_local_fallback(name, dst, int(meta["min_size"]), source_dirs):
                continue
            try:
                fetch(meta, dst, int(meta["min_size"]), args.timeout)
            except Exception:
                if not copy_local_fallback(name, dst, int(meta["min_size"]), source_dirs):
                    raise
    except Exception as exc:
        print(f"open font download failed: {exc}", file=sys.stderr)
        print("You can retry later, or copy the open font cache into tools/fonts/noto.", file=sys.stderr)
        print("Expected files: LXGW975YuanSC-400W.ttf and NotoSansMono-Regular.ttf.", file=sys.stderr)
        print("Alternative: set MIAOO_FONT_SOURCE_DIR to a directory containing those files.", file=sys.stderr)
        return 1

    marker = out_dir / ".fonts-ready"
    marker.write_text("Open fonts downloaded. Sources are listed in tools/fetch_open_fonts.py\n",
                      encoding="utf-8")
    print(f"Open fonts ready: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
