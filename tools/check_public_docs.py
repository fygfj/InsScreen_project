#!/usr/bin/env python3
"""Check public repository documentation against current project files.

This script is intentionally read-only. It verifies the facts that are easy to
let drift in README/release documents before publishing to Gitee.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

EXPECTED = {
    "project_version": "2.3.4",
    "http_routes": 73,
    "web_html": 12,
    "web_png": 1,
    "release_bin_sha256": "A7EB0494BAD50D5D297A9AD67F24CE224664E9F05A0189D9A73F11A3210301EC",
    "release_zip_sha256": "749C81BAD63712F403AF4616DCDD9671143F9C426CBE8CAB4BFD02E848D8D664",
}

EXPECTED_PARTITIONS = {
    "nvs": ("0x9000", "0xC000"),
    "otadata": ("0x15000", "0x2000"),
    "phy_init": ("0x17000", "0x1000"),
    "factory": ("0x20000", "0x300000"),
    "ota_0": ("0x320000", "0x300000"),
    "ota_1": ("0x620000", "0x300000"),
    "coredump": ("0x920000", "0x20000"),
    "fontfs": ("0x940000", "0x2E0000"),
    "spiffs": ("0xC20000", "0x3E0000"),
}

PUBLIC_TEXT_EXTENSIONS = {
    ".csv",
    ".html",
    ".json",
    ".md",
    ".ps1",
    ".py",
    ".txt",
    ".yml",
    ".yaml",
}

FORBIDDEN_SCAN_EXCLUDES = {
    "tools/check_public_docs.py",
}

FORBIDDEN_PATTERNS = [
    r"本次会话",
    r"working tree",
    r"COM82",
    r"monitor 进程",
    r"AppData",
    r"wxid_",
    r"Tencent Files",
    r"C:\\Users",
    r"D:/wx",
    r"v2\.3\.1",
    r"v2\.3\.2",
    r"v2\.3\.8",
    r"墨鱼",
    r"墨水鱼",
    r"Moy",
    r"sk-[A-Za-z0-9]",
    r"AIza",
    r"hf_[A-Za-z0-9]",
    r"BEGIN (RSA|OPENSSH|PRIVATE) KEY",
]


def run_git(*args: str) -> str:
    return subprocess.check_output(
        ["git", "-c", "core.quotePath=false", *args],
        cwd=ROOT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def read_text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with (ROOT / path).open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def check(condition: bool, label: str, failures: list[str], detail: str = "") -> None:
    if condition:
        print(f"OK  {label}")
    else:
        suffix = f": {detail}" if detail else ""
        print(f"ERR {label}{suffix}")
        failures.append(f"{label}{suffix}")


def check_project_version(failures: list[str]) -> None:
    cmake = read_text("CMakeLists.txt")
    match = re.search(r'set\(PROJECT_VER\s+"([^"]+)"\)', cmake)
    version = match.group(1) if match else None
    check(version == EXPECTED["project_version"], f"PROJECT_VER is {EXPECTED['project_version']}", failures, str(version))


def check_web_embed(failures: list[str]) -> None:
    cmake = read_text("main/CMakeLists.txt")
    block = re.search(r"EMBED_FILES(.*?)PRIV_REQUIRES", cmake, re.S)
    embedded = re.findall(r"\$\{PROJECT_DIR\}/web/([^\"\s]+)", block.group(1) if block else "")
    html = [p for p in embedded if p.endswith(".html")]
    png = [p for p in embedded if p.endswith(".png")]
    check(len(html) == EXPECTED["web_html"], "embedded HTML count is 12", failures, str(html))
    check(len(png) == EXPECTED["web_png"], "embedded PNG count is 1", failures, str(png))
    for name in embedded:
        check((ROOT / "web" / name).exists(), f"embedded web resource exists: {name}", failures)


def check_http_routes(failures: list[str]) -> None:
    text = read_text("main/http_app.c")
    try:
        start = text.index("const httpd_uri_t uris[] = {")
        end = text.index("    };", start)
    except ValueError:
        check(False, "HTTP route table found", failures)
        return
    body = text[start:end]
    entries = re.findall(r'\{\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)\s*,', body)
    check(len(entries) == EXPECTED["http_routes"], "HTTP route count is 73", failures, str(len(entries)))


def check_partitions(failures: list[str]) -> None:
    rows = {}
    for raw in read_text("partitions.csv").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.rstrip(",").split(",")]
        if len(cols) >= 5:
            rows[cols[0]] = (cols[3], cols[4])
    for name, expected in EXPECTED_PARTITIONS.items():
        check(rows.get(name) == expected, f"partition {name} is {expected[0]} {expected[1]}", failures, str(rows.get(name)))


def check_release_hashes(failures: list[str]) -> None:
    bin_hash = sha256_file("release/epaper_uploader_full_16MB.bin")
    zip_hash = sha256_file("release/epaper_uploader_full_16MB.zip")
    check(bin_hash == EXPECTED["release_bin_sha256"], "release bin SHA256 matches docs", failures, bin_hash)
    check(zip_hash == EXPECTED["release_zip_sha256"], "release zip SHA256 matches docs", failures, zip_hash)


def check_markdown_links(failures: list[str]) -> None:
    tracked = tracked_files()
    missing: list[str] = []
    for path in sorted(p for p in tracked if p.lower().endswith(".md")):
        text = read_text(path)
        for match in re.finditer(r"!{0,1}\[[^\]]*\]\(([^)]+)\)", text):
            target = match.group(1).strip()
            if target.startswith("<") and target.endswith(">"):
                target = target[1:-1]
            target = target.split("#", 1)[0].strip()
            if not target or re.match(r"^[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                continue
            norm = os.path.normpath(os.path.join(os.path.dirname(path), target.replace("\\", "/"))).replace("\\", "/")
            if norm.startswith("../"):
                continue
            if norm not in tracked and not any(x.startswith(norm.rstrip("/") + "/") for x in tracked):
                line = text.count("\n", 0, match.start()) + 1
                missing.append(f"{path}:{line} -> {target}")
    check(not missing, "Markdown local links resolve", failures, "; ".join(missing[:10]))


def check_forbidden_text(failures: list[str]) -> None:
    hits: list[str] = []
    for path in sorted(public_text_files()):
        if path in FORBIDDEN_SCAN_EXCLUDES:
            continue
        text = read_text(path)
        for pattern in FORBIDDEN_PATTERNS:
            regex = re.compile(pattern)
            for match in regex.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                hits.append(f"{path}:{line}:{pattern}")
    check(not hits, "public docs have no banned local/private markers", failures, "; ".join(hits[:10]))


def tracked_files() -> set[str]:
    return {p.replace("\\", "/") for p in run_git("ls-files").splitlines()}


def public_text_files() -> list[str]:
    files: list[str] = []
    for path in tracked_files():
        suffix = Path(path).suffix.lower()
        if suffix in PUBLIC_TEXT_EXTENSIONS:
            files.append(path)
    return files


def main() -> int:
    failures: list[str] = []
    check_project_version(failures)
    check_web_embed(failures)
    check_http_routes(failures)
    check_partitions(failures)
    check_release_hashes(failures)
    check_markdown_links(failures)
    check_forbidden_text(failures)

    if failures:
        print("\nFAILED")
        for item in failures:
            print(f"- {item}")
        return 1

    print("\nPublic documentation checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
