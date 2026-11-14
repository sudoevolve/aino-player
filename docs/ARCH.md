# Arch 软件源

适用于 Arch Linux 及衍生发行版，目前提供 x86_64。这是项目自己的签名 pacman 仓库，不是 AUR 包，也不收录于 Arch 官方仓库。

## 首次安装

一键添加签名软件源并安装：

```bash
curl -fsSL https://sudoevolve.github.io/aino-player/install-arch.sh | bash
```

脚本在请求权限前核对公钥指纹和配置摘要，使用独立的 `/etc/pacman.d/aino-player.conf`，重复运行不会重复添加仓库；遇到已有的手动配置会提示使用现有源。安装执行完整的 `pacman -Syu --noconfirm --needed aino-player`，会同步升级系统以避免 Arch 局部升级。需要 `curl`、`gnupg` 和已配置的软件包签名环境。

也可手动安装。

先导入并本地签名项目公钥：

```bash
curl -fsSL https://sudoevolve.github.io/aino-player/arch/aino-player.asc | sudo pacman-key --add -
sudo pacman-key --lsign-key 6BC11916DDE36B879AD6AC8A18681C67CA6020FE
```

公钥指纹：`6BC11916DDE36B879AD6AC8A18681C67CA6020FE`。核对命令：

```bash
sudo pacman-key --finger 6BC11916DDE36B879AD6AC8A18681C67CA6020FE
```

把软件源加入 `/etc/pacman.conf`：

```bash
sudo tee -a /etc/pacman.conf > /dev/null <<'EOF'
[aino-player]
Server = https://sudoevolve.github.io/aino-player/arch/x86_64/
SigLevel = Required
EOF
sudo pacman -Syu aino-player
```

软件源地址：[sudoevolve.github.io/aino-player/arch](https://sudoevolve.github.io/aino-player/arch/)。已手动安装过播放器也可以直接添加本源，无需卸载。

仓库数据库和软件包均由此密钥签名；`SigLevel = Required` 要求两种签名都存在且有效。

## 更新与移除

```bash
sudo pacman -Syu
```

软件源与 GitHub Release 提供相同的包，升级保留配置、收藏、歌单和下载音乐。当前播放器已有的手动更新入口继续可用；从软件源安装后可统一交给 pacman 更新。

卸载应用：`sudo pacman -Rs aino-player`。停止使用此软件源：删除 `/etc/pacman.conf` 中的 `[aino-player]` 段落；一键安装用户删除 `Include = /etc/pacman.d/aino-player.conf` 行和对应配置文件。然后移除密钥：

```bash
sudo pacman-key --delete 6BC11916DDE36B879AD6AC8A18681C67CA6020FE
```

移除软件源不会删除应用或用户资料。

## 发布维护

工作流 [repositories.yml](../.github/workflows/repositories.yml) 的 arch job 仅为已有 Release 生成软件源，不编译、安装或运行播放器，不运行 CTest、音频、浏览器或界面回归。

- 正式 Release 发布、`release` 工作流成功完成、每周定时或手动触发时刷新。两种 Release 触发用于兼容 GitHub 对工作流令牌事件的限制。
- 从正式 `vX.Y.Z` Release 选择最高的三个兼容版本，拒绝草稿、预发行、错误包身份、附件大小或校验不符。新版本必须提供真实 Arch 包（`aino-player-<版本>-1-x86_64.pkg.tar.zst`）和 `SHA256SUMS.txt`。
- 同时比对 GitHub 附件 digest 和 `SHA256SUMS.txt`，再以 `pacman -Qp` 检查包名、版本和发布号。
- `repo-add --sign` 生成并签名数据库（含 `.files`），每个包的签名记录在数据库中；与 APT / RPM 源共用同一把 Ed25519 密钥，公钥以 `aino-player.asc` 形式发布在仓库目录。私钥只在 GitHub Actions secret `APT_SIGNING_PRIVATE_KEY` 与维护者备份中，不能提交到 Git。
- 部署前在隔离的 `RootDir`/`GPGDir` 中导入密钥并执行 `pacman -Sy`、`-Si` 与 `-Sw`，验证候选及实际下载的包签名；不改 runner 的软件源，也不安装播放器。
- arch job 运行在 Arch 容器内（需要 `pacman` 自带的 `repo-add`、`gnupg`、`python`），与 APT 和 RPM 仓库树合流进同一次 GitHub Pages 部署。
- 手动重新部署：GitHub → Actions → **Publish package repositories** → Run workflow，选择 `master`。Arch 源在首个包含 Arch 包附件的正式版发布后才有内容；此前手动触发会在 arch job 明确报错。
