# APT 软件源

适用于 Ubuntu 22.04+ / Debian 12+，目前提供 amd64。使用项目自己的签名软件源，尚未收录于 Ubuntu 官方仓库。

## 首次安装

需要 `curl` 和 `ca-certificates`。添加一次源，以后由 APT 管理安装和更新：

```bash
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://sudoevolve.github.io/aino-player/apt/aino-player-archive-keyring.gpg | sudo tee /etc/apt/keyrings/aino-player-archive-keyring.gpg > /dev/null
curl -fsSL https://sudoevolve.github.io/aino-player/apt/aino-player.sources | sudo tee /etc/apt/sources.list.d/aino-player.sources > /dev/null
sudo apt update
sudo apt install aino-player
```

软件源地址：[sudoevolve.github.io/aino-player/apt](https://sudoevolve.github.io/aino-player/apt/)。已从 deb 安装过播放器也可以直接添加本源，无需卸载。

公钥指纹：`6BC11916DDE36B879AD6AC8A18681C67CA6020FE`。核对命令：

```bash
gpg --show-keys --with-fingerprint /etc/apt/keyrings/aino-player-archive-keyring.gpg
```

使用独立 keyring 和 `Signed-By`，不使用 `apt-key` 或 `trusted=yes`。

## 更新与移除

```bash
sudo apt update
sudo apt upgrade
```

也可以用系统的软件更新器。软件源与 GitHub Release 提供相同的 deb，升级保留配置、收藏、歌单和下载音乐。当前播放器已有的手动更新入口继续可用；从软件源安装后可统一交给 APT 更新。

卸载应用：`sudo apt remove aino-player`。停止使用此软件源：

```bash
sudo rm /etc/apt/sources.list.d/aino-player.sources
sudo rm /etc/apt/keyrings/aino-player-archive-keyring.gpg
sudo apt update
```

移除软件源不会删除应用或用户资料。

## 发布维护

工作流 [apt-repository.yml](../.github/workflows/apt-repository.yml) 仅为已有 Release 生成软件源，不编译、安装或运行播放器，不运行 CTest、音频、浏览器或界面回归。

- 正式 Release 发布、`release` 工作流成功完成、每周定时或手动触发时刷新。两种 Release 触发用于兼容 GitHub 对工作流令牌事件的限制。
- 从正式 `vX.Y.Z` Release 选择最高的三个兼容 deb 版本，拒绝草稿、预发行、错误包身份、附件大小或校验不符。新版本必须提供真实 deb 和 `SHA256SUMS.txt`。
- 同时比对 GitHub 附件 digest 和 `SHA256SUMS.txt`，再以 `dpkg-deb` 检查包名、版本和架构。
- 通过 `apt-ftparchive` 生成索引，包含按 SHA-256 寻址的索引副本；保留上一代索引，降低部署时客户端读取不一致的概率。
- 专用 Ed25519 公钥位于 `packaging/apt/`；私钥仅存 GitHub Actions secret `APT_SIGNING_PRIVATE_KEY` 及维护者本机受限目录，不能提交到 Git。签名固定到仓库内的完整公钥指纹。
- `InRelease` / `Release.gpg` 使用 SHA-256；索引有效期 30 天，每周重新签发。GitHub 若因仓库长期不活跃而停用定时工作流，需要重新启用并手动运行；签名过期时不应关闭 APT 校验。
- 部署前用 `gpgv` 验签，并在临时 APT 配置中执行索引读取，确认有安装候选；不改 runner 的软件源，也不安装播放器。
- GitHub Pages 使用 Actions 部署。没有改播放器版本、移动已发布标签或替换 Release 附件。

私钥备份位置为维护者本机 `C:/Users/admin/.codex/keys/aino-player-apt/`，包含导出的私钥和撤销证书，应保留至完成有计划的密钥轮换。公钥有效期至 2029-11-12；轮换时需保留旧密钥到用户取得新 keyring，不能直接重生成并静默替换指纹。

手动重新部署：GitHub → Actions → **Publish APT repository** → Run workflow，选择 `master`。
