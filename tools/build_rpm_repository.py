"""Publish existing, verified GitHub Release rpms as a signed DNF/Zypper repository.

写入 --output 指定目录下的 rpm/ 子树；根目录主页、图标和 .nojekyll 由
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
BASE_URL = "https://sudoevolve.github.io/aino-player/rpm"
KEY_NAME = "aino-player.asc"


def run(*args, **kwargs):
    return subprocess.check_output(list(args), **kwargs)


def fetch(url, limit, authenticated=False):
    headers = {"User-Agent": "aino-player-rpm-publisher", "Accept": "application/vnd.github+json"}
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
        name = "aino-player-" + version + "-1.x86_64.rpm"
        assets = release["assets"]
        matches = [a for a in assets if a["name"] == name]
        if not matches:
            if not versions:
                raise ValueError("Latest stable Release has no compatible x86_64 rpm; "
                                 "build one with the updated release workflow first")
            continue
        checksums = [a for a in assets if a["name"] == "SHA256SUMS.txt"]
        if len(matches) != 1 or len(checksums) != 1:
            raise ValueError("Release must contain one rpm and one SHA256SUMS.txt")
        prefix = "https://github.com/" + REPOSITORY + "/releases/download/" + release["tag_name"] + "/"
        for asset in (matches[0], checksums[0]):
            if asset["browser_download_url"] != prefix + asset["name"]:
                raise ValueError("Release asset URL does not match its repository/tag")
        checksum_bytes = fetch(checksums[0]["browser_download_url"], 128 * 1024)
        entries = [line.split() for line in checksum_bytes.decode("utf-8").splitlines()]
        expected = [parts[0] for parts in entries if len(parts) == 2 and parts[1] == name]
        if len(expected) != 1 or not re.fullmatch(r"[0-9a-f]{64}", expected[0]):
            raise ValueError("Missing or invalid rpm checksum")
        asset = matches[0]
        data = fetch(asset["browser_download_url"], 64 * 1024**2)
        if len(data) != asset["size"] or digest(data) != expected[0]:
            raise ValueError("Release rpm size or SHA-256 mismatch")
        if asset.get("digest") != "sha256:" + expected[0]:
            raise ValueError("GitHub asset digest does not match SHA256SUMS.txt")
        target = pool / name
        target.write_bytes(data)
        fields = run("rpm", "-qp", "--qf", "%{NAME}|%{VERSION}|%{RELEASE}|%{ARCH}\\n", str(target)).decode().strip()
        if fields != "aino-player|" + version + "|1|x86_64":
            raise ValueError("RPM identity does not match its Release: " + fields)
        versions.append(version)
        print("Verified", name, expected[0], flush=True)
        if len(versions) == 3:
            break
    if not versions:
        raise ValueError("No stable rpm packages found")
    return versions


def verify_dnf(tree, key, fingerprint):
    # 只读取索引和候选版本；不安装、启动或测试播放器，也不修改机器的软件源。
    with tempfile.TemporaryDirectory(prefix="aino-dnf-check-") as directory:
        state = Path(directory)
        (state / "etc/yum.repos.d").mkdir(parents=True)
        (state / "var/lib/rpm").mkdir(parents=True)
        gnupg = state / "gnupg"
        gnupg.mkdir(mode=0o700)
        # gpgv 默认读取 trustedkeys.kbx，导入时直接写进该 keyring。
        subprocess.run(["gpg", "--homedir", str(gnupg), "--no-default-keyring", "--keyring", "trustedkeys.kbx",
                        "--batch", "--quiet", "--import", str(key)], check=True)
        subprocess.run(["gpgv", "--homedir", str(gnupg), str(tree / "repodata/repomd.xml.asc"),
                        str(tree / "repodata/repomd.xml")], check=True)
        subprocess.run(["rpm", "--root", str(state), "--quiet", "--import", str(key)], check=True)
        # 包签名用导入公钥后的隔离 rpmdb 核验；NOKEY 说明包没有有效签名。
        for rpm_path in sorted(tree.glob("packages/*.rpm")):
            result = subprocess.run(["rpm", "--root", str(state), "--checksig", str(rpm_path)],
                                    capture_output=True, text=True, check=True)
            if "NOKEY" in result.stdout + result.stderr:
                raise ValueError("RPM signature could not be verified with the imported key: " + rpm_path.name)
        repo = state / "etc/yum.repos.d/aino-player.repo"
        repo.write_text("[aino-player]\nname=Aino Player check\nbaseurl=" + tree.as_uri() + "\n"
                        "enabled=1\ngpgcheck=1\nrepo_gpgcheck=1\ngpgkey=" + key.as_uri() + "\n", encoding="utf-8")
        releasever = ""
        for line in Path("/etc/os-release").read_text(encoding="utf-8").splitlines():
            if line.startswith("VERSION_ID="):
                releasever = line.split("=", 1)[1].strip('"')
        options = ["--installroot", str(state), "--releasever", releasever, "--assumeyes", "--quiet"]
        subprocess.run(["dnf5", *options, "makecache"], check=True)
        listing = run("dnf5", *options, "repoquery", "--repoid", "aino-player", "aino-player").decode()
        print(listing.strip(), flush=True)
        if not re.search(r"aino-player-[0-9]", listing):
            raise ValueError("DNF did not find an installable candidate")


def sign(output, fingerprint):
    keydir = Path(os.environ.get("GNUPGHOME", ""))
    if not keydir.is_dir():
        raise ValueError("Set GNUPGHOME to a private keyring home with the signing key")
    for rpm in sorted(output.glob("packages/*.rpm")):
        # rpm 6 起移除了 --keyId 选项；_gpg_name 是各版本通用的签名密钥指定方式。
        subprocess.run(["rpmsign", "--addsign", "--define", "_gpg_name " + fingerprint, str(rpm)], check=True)
    repomd = output / "repodata/repomd.xml"
    subprocess.run(["gpg", "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase", "",
                    "--local-user", fingerprint, "--digest-algo", "SHA256", "--armor", "--detach-sign",
                    "--output", str(repomd) + ".asc", str(repomd)], check=True)
    key = output / KEY_NAME
    key.write_bytes(run("gpg", "--armor", "--export", fingerprint))
    return key


def build(output, fingerprint, check):
    tree = output / "rpm"
    if tree.exists():
        raise ValueError("rpm/ subtree already exists; use a fresh staging directory")
    expected = (ROOT / "packaging/signing-key-fingerprint.txt").read_text(encoding="ascii").strip()
    if fingerprint != expected or not re.fullmatch(r"[A-F0-9]{40}", fingerprint):
        raise ValueError("Signing key does not match the committed repository trust root")
    pool = tree / "packages"
    pool.mkdir(parents=True)
    versions = release_packages(pool)
    # dnf 客户端按 repodata 相对路径访问，包放在 packages/ 里保持仓库地址内自包含。
    # 新版 createrepo_c 用 --general-compress-type 才能覆盖 primary/filelists；旧版回退旧选项名。
    for flag in ("--general-compress-type", "--compress-type"):
        result = subprocess.run(["createrepo_c", flag, "gz", "--workers", "2", str(tree)])
        if result.returncode == 0:
            break
        if flag == "--compress-type":
            raise RuntimeError("createrepo_c failed to index the repository")
    key = sign(tree, fingerprint)
    shutil.copy2(ROOT / "packaging/rpm/aino-player.repo", tree / "aino-player.repo")
    installer = ROOT / "packaging/rpm/install.sh"
    subprocess.run(["bash", "-n", str(installer)], check=True)
    shutil.copy2(installer, tree / "install-rpm.sh")
    template = (ROOT / "packaging/rpm/index.html").read_text(encoding="utf-8")
    (tree / "index.html").write_text(template.replace("@FINGERPRINT@", fingerprint)
                                     .replace("@VERSION@", versions[0]), encoding="utf-8")
    if check:
        verify_dnf(tree, key, fingerprint)
    print("RPM source ready:", BASE_URL, "candidate", versions[0], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("--verify-dnf", action="store_true")
    args = parser.parse_args()
    build(args.output.resolve(), args.fingerprint, args.verify_dnf)
