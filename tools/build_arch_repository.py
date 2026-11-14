"""Publish existing, verified GitHub Release Arch packages as a signed pacman repository.

写入 --output 指定目录下的 arch/ 子树；根目录主页、图标和 .nojekyll 由
build_apt_repository.py 统一渲染，三棵仓库树在同一工作流里一次性部署。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
# Actions 环境自动指向当前仓库，fork 与改名场景无需修改脚本。
REPOSITORY = os.environ.get("GITHUB_REPOSITORY") or "sudoevolve/aino-player"
BASE_URL = "https://sudoevolve.github.io/aino-player/arch"
KEY_NAME = "aino-player.asc"


def run(*args, **kwargs):
    return subprocess.check_output(list(args), **kwargs)


def fetch(url, limit, authenticated=False):
    headers = {"User-Agent": "aino-player-arch-publisher", "Accept": "application/vnd.github+json"}
    if authenticated and os.environ.get("GH_TOKEN"):
        if not url.startswith("https://api.github.com/repos/" + REPOSITORY + "/"):
            raise ValueError("GitHub credentials may only be sent to this repository's API")
        headers["Authorization"] = "Bearer " + os.environ["GH_TOKEN"]
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError("Download exceeded the repository size limit")
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def release_packages(pool):
    releases = json.loads(fetch("https://api.github.com/repos/" + REPOSITORY + "/releases?per_page=100", 4 * 1024**2, True))
    stable = [r for r in releases if not r["draft"] and not r["prerelease"] and
              re.fullmatch(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", r["tag_name"])]
    stable.sort(key=lambda r: tuple(map(int, r["tag_name"][1:].split("."))), reverse=True)
    versions = []
    for release in stable:
        version = release["tag_name"][1:]
        name = "aino-player-" + version + "-1-x86_64.pkg.tar.zst"
        assets = release["assets"]
        matches = [a for a in assets if a["name"] == name]
        if not matches:
            if not versions:
                raise ValueError("Latest stable Release has no compatible x86_64 Arch package; "
                                 "build one with the updated release workflow first")
            continue
        checksums = [a for a in assets if a["name"] == "SHA256SUMS.txt"]
        if len(matches) != 1 or len(checksums) != 1:
            raise ValueError("Release must contain one Arch package and one SHA256SUMS.txt")
        prefix = "https://github.com/" + REPOSITORY + "/releases/download/" + release["tag_name"] + "/"
        for asset in (matches[0], checksums[0]):
            if asset["browser_download_url"] != prefix + asset["name"]:
                raise ValueError("Release asset URL does not match its repository/tag")
        checksum_bytes = fetch(checksums[0]["browser_download_url"], 128 * 1024)
        entries = [line.split() for line in checksum_bytes.decode("utf-8").splitlines()]
        expected = [parts[0] for parts in entries if len(parts) == 2 and parts[1] == name]
        if len(expected) != 1 or not re.fullmatch(r"[0-9a-f]{64}", expected[0]):
            raise ValueError("Missing or invalid Arch package checksum")
        asset = matches[0]
        data = fetch(asset["browser_download_url"], 64 * 1024**2)
        if len(data) != asset["size"] or digest(data) != expected[0]:
            raise ValueError("Release package size or SHA-256 mismatch")
        if asset.get("digest") != "sha256:" + expected[0]:
            raise ValueError("GitHub asset digest does not match SHA256SUMS.txt")
        target = pool / name
        target.write_bytes(data)
        identity = run("pacman", "-Qp", str(target)).decode().strip()
        if identity != "aino-player " + version + "-1":
            raise ValueError("Arch package identity does not match its Release: " + identity)
        versions.append(version)
        print("Verified", name, expected[0], flush=True)
        if len(versions) == 3:
            break
    if not versions:
        raise ValueError("No stable Arch packages found")
    return versions


def verify_pacman(tree, key, fingerprint):
    # 只读取索引和候选版本；不安装、启动或测试播放器，也不修改机器的软件源。
    with tempfile.TemporaryDirectory(prefix="aino-pacman-check-") as directory:
        state = Path(directory)
        for name in ("root", "db", "cache", "gnupg"):
            (state / name).mkdir()
        conf = state / "pacman.conf"
        conf.write_text("[options]\nRootDir = " + str(state / "root") + "\nDBPath = " + str(state / "db") +
                        "\nCacheDir = " + str(state / "cache") + "\nGPGDir = " + str(state / "gnupg") +
                        "\nLogFile = " + str(state / "log") + "\nHoldPkg = pacman glibc\n"
                        "[aino-player]\nServer = " + tree.as_uri() + "\nSigLevel = Required\n", encoding="utf-8")
        gnupg = "--gpgdir " + str(state / "gnupg")
        subprocess.run(["pacman-key", *gnupg.split(), "--init"], check=True)
        subprocess.run(["pacman-key", *gnupg.split(), "--add", str(key)], check=True)
        subprocess.run(["pacman-key", *gnupg.split(), "--lsign-key", fingerprint], check=True)
        subprocess.run(["pacman", "--config", str(conf), "--noconfirm", "-Sy"], check=True)
        info = run("pacman", "--config", str(conf), "-Si", "aino-player").decode()
        print(info, flush=True)
        if not re.search(r"Version\s*:\s*[0-9]", info):
            raise ValueError("pacman did not find an installable candidate")
        # -Sw 真正下载包并按 SigLevel 校验数据库与包签名，比 -Sp 更接近真实安装路径。
        subprocess.run(["pacman", "--config", str(conf), "--noconfirm", "-Sw", "aino-player"], check=True)
        if not list((state / "cache").glob("aino-player-*.pkg.tar.zst")):
            raise ValueError("pacman did not download the package into the isolated cache")


def sign(output, fingerprint):
    keydir = Path(os.environ.get("GNUPGHOME", ""))
    if not keydir.is_dir():
        raise ValueError("Set GNUPGHOME to a private keyring home with the signing key")
    packages = sorted(output.glob("*.pkg.tar.zst"))
    # 用户的 SigLevel = Required 要求每个包都有签名：先给包生成 .sig，
    # repo-add 才能把 %PGPSIG% 写进数据库；数据库随后由 --key 签名。
    for pkg in packages:
        subprocess.run(["gpg", "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase", "",
                        "--detach-sign", "--output", str(pkg) + ".sig", str(pkg)], check=True)
    database = output / "aino-player.db.tar.gz"
    subprocess.run(["repo-add", "--key", fingerprint, str(database), *(str(p) for p in packages)], check=True)
    # pacman 请求规范名 <repo>.db 与 <repo>.db.sig，且签名请求不做扩展名回退；
    # 在仓库目录补放规范名副本，让 SigLevel = Required 也能取到数据库签名。
    database_sig = output / "aino-player.db.tar.gz.sig"
    if not database_sig.exists():
        database_sig = output / "aino-player.db.sig"
    if not database_sig.exists():
        raise ValueError("repo-add did not sign the database")
    shutil.copy2(database, output / "aino-player.db")
    shutil.copy2(database_sig, output / "aino-player.db.sig")
    key = output.parent / KEY_NAME
    key.write_bytes(run("gpg", "--armor", "--export", fingerprint))
    return key


def build(output, fingerprint, check):
    arch = output / "arch"
    if arch.exists():
        raise ValueError("arch/ subtree already exists; use a fresh staging directory")
    expected = (ROOT / "packaging/signing-key-fingerprint.txt").read_text(encoding="ascii").strip()
    if fingerprint != expected or not re.fullmatch(r"[A-F0-9]{40}", fingerprint):
        raise ValueError("Signing key does not match the committed repository trust root")
    tree = arch / "x86_64"
    tree.mkdir(parents=True)
    versions = release_packages(tree)
    key = sign(tree, fingerprint)
    template = (ROOT / "packaging/arch/index.html").read_text(encoding="utf-8")
    (arch / "index.html").write_text(template.replace("@FINGERPRINT@", fingerprint)
                                     .replace("@VERSION@", versions[0]), encoding="utf-8")
    if check:
        verify_pacman(tree, key, fingerprint)
    print("Arch source ready:", BASE_URL, "candidate", versions[0], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("--verify-pacman", action="store_true")
    args = parser.parse_args()
    build(args.output.resolve(), args.fingerprint, args.verify_pacman)
