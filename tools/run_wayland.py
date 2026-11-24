"""在隔离的无 X11 compositor 中执行发布检查；普通构建不会调用。"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compositor", choices=("weston", "sway"), default="weston")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.command:
        parser.error("specify a binary and its arguments")
    compositor_binary = shutil.which(args.compositor)
    if not compositor_binary:
        raise SystemExit("Pure Wayland acceptance requires " + args.compositor)
    binary = Path(args.command[0]).resolve()
    dependencies = subprocess.check_output(["ldd", str(binary)], text=True)
    forbidden = ("libX11.so", "libxcb.so", "libXext.so")
    if any(name in dependencies for name in forbidden):
        raise SystemExit("Pure Wayland binary links an X11 library:\n" + dependencies)
    environment = os.environ.copy()
    environment.pop("DISPLAY", None)
    environment["XDG_SESSION_TYPE"] = "wayland"
    with tempfile.TemporaryDirectory(prefix="aino-wayland-") as directory:
        root = Path(directory)
        root.chmod(0o700)
        environment["XDG_RUNTIME_DIR"] = directory
        socket_name = "aino-acceptance" if args.compositor == "weston" else "wayland-0"
        environment["WAYLAND_DISPLAY"] = socket_name
        environment["XDG_CONFIG_HOME"] = str(root / "config")
        environment["XDG_CACHE_HOME"] = str(root / "cache")
        if args.compositor == "sway":
            environment.update(WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1",
                               AINO_REQUIRE_LAYER_SHELL="1")
            config = root / "sway.conf"
            config.write_text("xwayland disable\noutput * mode 1024x768\nseat * fallback true\n", encoding="utf-8")
            command = [compositor_binary, "--debug", "--config", str(config)]
        else:
            command = [compositor_binary, "--backend=headless-backend.so", "--socket=" + socket_name, "--idle-time=0"]
        log = root / (args.compositor + ".log")
        with log.open("wb") as output:
            weston_environment = environment.copy()
            if args.compositor == "sway":
                weston_environment.pop("WAYLAND_DISPLAY", None)
            if hasattr(os, "geteuid") and os.geteuid() == 0:
                # RPM/Arch 发布容器默认 root；compositor 和应用共用独立非特权身份。
                os.chown(root, 65534, 65534)
                environment["HOME"] = directory
                weston_environment["HOME"] = directory
                identity = {"user": 65534, "group": 65534, "extra_groups": []}
            else:
                identity = {}
            compositor = subprocess.Popen(
                command,
                env=weston_environment, stdout=output, stderr=subprocess.STDOUT, **identity)
            try:
                deadline = time.monotonic() + 15
                while not (root / socket_name).is_socket():
                    if args.compositor == "sway":
                        sockets = [path for path in root.glob("wayland-*") if path.is_socket()]
                        if sockets:
                            environment["WAYLAND_DISPLAY"] = sockets[0].name
                            break
                    if compositor.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Wayland compositor did not start")
                    time.sleep(0.05)
                # 测试使用独立 session bus，避免向用户的媒体面板和托盘注册实例。
                launcher = ["dbus-run-session", "--"]
                subprocess.run([*launcher, str(binary), *args.command[1:]], env=environment,
                               check=True, timeout=90, **identity)
            except BaseException:
                print(log.read_text(errors="replace"))
                raise
            finally:
                compositor.terminate()
                try:
                    compositor.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    compositor.kill()
                    compositor.wait()


if __name__ == "__main__":
    run()
