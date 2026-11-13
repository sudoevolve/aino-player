"""Publish existing, verified GitHub Release debs as a signed APT repository."""
import argparse
from datetime import datetime, timedelta, timezone
from email.utils import format_datetime
import gzip
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
REPOSITORY = "sudoevolve/aino-player"
BASE_URL = "https://sudoevolve.github.io/aino-player/apt"
KEY_NAME = "aino-player-archive-keyring.gpg"


def run(*args, **kwargs):
    return subprocess.check_output(list(args), **kwargs)


def fetch(url, limit, authenticated=False):
    headers = {"User-Agent": "aino-player-apt-publisher", "Accept": "application/vnd.github+json"}
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
        name = "aino-player_" + version + "_amd64.deb"
        assets = release["assets"]
        matches = [a for a in assets if a["name"] == name]
        if not matches:
            if not versions:
                raise ValueError("Latest stable Release has no compatible amd64 deb")
            continue
        checksums = [a for a in assets if a["name"] == "SHA256SUMS.txt"]
        if len(matches) != 1 or len(checksums) != 1:
            raise ValueError("Release must contain one deb and one SHA256SUMS.txt")
        prefix = "https://github.com/" + REPOSITORY + "/releases/download/" + release["tag_name"] + "/"
        for asset in (matches[0], checksums[0]):
            if asset["browser_download_url"] != prefix + asset["name"]:
                raise ValueError("Release asset URL does not match its repository/tag")
        checksum_bytes = fetch(checksums[0]["browser_download_url"], 128 * 1024)
        entries = [line.split() for line in checksum_bytes.decode("utf-8").splitlines()]
        expected = [parts[0] for parts in entries if len(parts) == 2 and parts[1] == name]
        if len(expected) != 1 or not re.fullmatch(r"[0-9a-f]{64}", expected[0]):
            raise ValueError("Missing or invalid deb checksum")
        asset = matches[0]
        data = fetch(asset["browser_download_url"], 64 * 1024**2)
        if len(data) != asset["size"] or digest(data) != expected[0]:
            raise ValueError("Release deb size or SHA-256 mismatch")
        if asset.get("digest") != "sha256:" + expected[0]:
            raise ValueError("GitHub asset digest does not match SHA256SUMS.txt")
        target = pool / name
        target.write_bytes(data)
        fields = {field: run("dpkg-deb", "--field", str(target), field).decode().strip()
                  for field in ("Package", "Version", "Architecture")}
        if fields != {"Package": "aino-player", "Version": version, "Architecture": "amd64"}:
            raise ValueError("Debian package identity does not match its Release")
        versions.append(version)
        print("Verified", name, expected[0], flush=True)
        if len(versions) == 3:
            break
    if not versions:
        raise ValueError("No stable deb packages found")
    return versions


def verify_apt(apt):
    # 只读取索引和候选版本；不安装、启动或测试播放器，也不修改机器的软件源。
    with tempfile.TemporaryDirectory(prefix="aino-apt-check-") as directory:
        state = Path(directory)
        (state / "lists/partial").mkdir(parents=True)
        (state / "cache/archives/partial").mkdir(parents=True)
        source = state / "aino-player.sources"
        source.write_text("Types: deb\nURIs: " + apt.as_uri() + "\nSuites: stable\nComponents: main\n"
                          "Architectures: amd64\nSigned-By: " + str(apt / KEY_NAME) + "\n", encoding="utf-8")
        options = ["-o", "Dir::Etc::sourcelist=" + str(source), "-o", "Dir::Etc::sourceparts=-",
                   "-o", "Dir::State::lists=" + str(state / "lists"), "-o", "Dir::Cache=" + str(state / "cache"),
                   "-o", "APT::Sandbox::User=" + __import__("getpass").getuser(), "-o", "Debug::NoLocking=1"]
        subprocess.run(["apt-get", *options, "update"], check=True)
        policy = run("apt-cache", *options, "policy", "aino-player").decode()
        print(policy, flush=True)
        if not re.search(r"Candidate: [0-9]", policy):
            raise ValueError("APT did not find an installable candidate")


def build(output, fingerprint, check):
    if output.exists() and any(output.iterdir()):
        raise ValueError("Output directory must be empty; use a fresh staging directory")
    key = ROOT / "packaging/apt" / KEY_NAME
    expected = (ROOT / "packaging/apt/signing-key-fingerprint.txt").read_text(encoding="ascii").strip()
    if fingerprint != expected or not re.fullmatch(r"[A-F0-9]{40}", fingerprint):
        raise ValueError("Signing key does not match the committed repository trust root")
    apt = output / "apt"
    pool = apt / "pool/main/a/aino-player"
    pool.mkdir(parents=True)
    versions = release_packages(pool)
    packages = apt / "dists/stable/main/binary-amd64/Packages"
    packages.parent.mkdir(parents=True)
    # apt-ftparchive emits pool/... relative paths, so clients stay within this source.
    data = run("apt-ftparchive", "packages", "pool", cwd=apt)
    packages.write_bytes(data)
    compressed = gzip.compress(data, mtime=0)
    packages.with_suffix(".gz").write_bytes(compressed)
    by_hash = packages.parent / "by-hash/SHA256"
    by_hash.mkdir(parents=True)
    # Retain one previous index generation during a Pages deployment to reduce fetch races.
    for name in ("Packages", "Packages.gz"):
        try:
            old = fetch(BASE_URL + "/dists/stable/main/binary-amd64/" + name, 1024**2)
            (by_hash / digest(old)).write_bytes(old)
        except urllib.error.HTTPError as error:
            if error.code != 404:
                raise
    for value in (data, compressed):
        (by_hash / digest(value)).write_bytes(value)
    now = datetime.now(timezone.utc)
    lines = ["Origin: Aino Player", "Label: Aino Player", "Suite: stable", "Codename: stable",
             "Architectures: amd64", "Components: main", "Acquire-By-Hash: yes",
             "Description: Aino Player official project APT repository",
             "Date: " + format_datetime(now, usegmt=True),
             "Valid-Until: " + format_datetime(now + timedelta(days=30), usegmt=True), "SHA256:"]
    for name, value in (("Packages", data), ("Packages.gz", compressed)):
        lines.append(" " + digest(value) + " " + str(len(value)) + " main/binary-amd64/" + name)
    release = apt / "dists/stable/Release"
    release.write_text("\n".join(lines) + "\n", encoding="utf-8")
    for flags, filename in ((["--clearsign"], "InRelease"), (["--armor", "--detach-sign"], "Release.gpg")):
        subprocess.run(["gpg", "--batch", "--yes", "--pinentry-mode", "loopback", "--passphrase", "",
                        "--local-user", fingerprint, "--digest-algo", "SHA256", "--output", str(release.parent / filename),
                        *flags, str(release)], check=True)
    shutil.copy2(key, apt / KEY_NAME)
    shutil.copy2(ROOT / "packaging/apt/signing-key-fingerprint.txt", apt / "signing-key-fingerprint.txt")
    shutil.copy2(ROOT / "packaging/apt/aino-player.sources", apt / "aino-player.sources")
    subprocess.run(["gpgv", "--keyring", str(apt / KEY_NAME), str(release.parent / "InRelease")], check=True)
    if check:
        verify_apt(apt)
    html = (ROOT / "packaging/apt/index.html").read_text(encoding="utf-8")
    html = html.replace("@FINGERPRINT@", fingerprint).replace("@VERSION@", versions[0])
    (output / "index.html").write_text(html, encoding="utf-8")
    (apt / "index.html").write_text(html, encoding="utf-8")
    (output / ".nojekyll").touch()
    print("APT source ready:", BASE_URL, "candidate", versions[0], flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--fingerprint", required=True)
    parser.add_argument("--verify-apt", action="store_true")
    args = parser.parse_args()
    build(args.output.resolve(), args.fingerprint, args.verify_apt)
