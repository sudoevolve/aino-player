# RPM 软件源

适用于 Fedora 44+，目前提供 x86_64。使用项目自己的签名 DNF 软件源，尚未收录于 Fedora 官方仓库。构建固定在 Fedora 44，RPM 自动记录 ELF 库版本依赖，避免在旧系统上安装无法运行的二进制。openSUSE 的包名与 ABI 尚未验证，不属于当前支持范围。

## 首次安装

需要 `curl` 和 `ca-certificates`。一键添加签名源并安装：

```bash
curl -fsSL https://sudoevolve.github.io/aino-player/install-rpm.sh | bash
```

脚本先验证架构、DNF、公钥指纹和源配置的固定 SHA-256，再请求 sudo 权限，添加源并通过 dnf 安装。

如果缺少工具，先执行 `sudo dnf install curl ca-certificates gnupg2`。也可手动添加一次源，以后由包管理器管理安装和更新。

Fedora（dnf 5）：

```bash
sudo dnf config-manager addrepo --from-repofile=https://sudoevolve.github.io/aino-player/rpm/aino-player.repo
sudo rpm --import https://sudoevolve.github.io/aino-player/rpm/aino-player.asc
sudo dnf install aino-player
```

较旧的 dnf 4 直接放置仓库文件：

```bash
sudo curl -fsSL https://sudoevolve.github.io/aino-player/rpm/aino-player.repo -o /etc/yum.repos.d/aino-player.repo
sudo rpm --import https://sudoevolve.github.io/aino-player/rpm/aino-player.asc
sudo dnf install aino-player
```

软件源地址：[sudoevolve.github.io/aino-player/rpm](https://sudoevolve.github.io/aino-player/rpm/)。已从 rpm 安装过播放器也可以直接添加本源，无需卸载。

公钥指纹：`6BC11916DDE36B879AD6AC8A18681C67CA6020FE`。导入前核对命令：

```bash
gpg --show-keys --with-fingerprint aino-player.asc
```

导入后用 `rpm -qi gpg-pubkey-ca6020fe-*` 查看。仓库元数据（`repo_gpgcheck`）和每个 rpm 都由此密钥签名；缺少或损坏的签名会让安装失败。

## 更新与移除

```bash
sudo dnf upgrade aino-player      # Fedora
```

也可以用系统的软件更新器。软件源基于 GitHub Release 的 rpm 添加包内签名，签名后重新生成索引；播放器内容相同，文件摘要随包签名变化。升级保留配置、收藏、歌单和下载音乐。从软件源安装后统一交给包管理器更新。

卸载应用：`sudo dnf remove aino-player`。停止使用此软件源：

```bash
sudo rm /etc/yum.repos.d/aino-player.repo        # Fedora
sudo rpm -e gpg-pubkey-ca6020fe-*
```

移除软件源不会删除应用或用户资料。

## 发布维护

工作流 [repositories.yml](../.github/workflows/repositories.yml) 的 rpm job 仅为已有 Release 生成软件源，不编译、安装或运行播放器，不运行 CTest、音频、浏览器或界面回归。

- 正式 Release 发布、`release` 工作流成功完成、每周定时或手动触发时刷新。两种 Release 触发用于兼容 GitHub 对工作流令牌事件的限制。
- 从正式 `vX.Y.Z` Release 选择最高的三个兼容 rpm 版本，拒绝草稿、预发行、错误包身份、附件大小或校验不符。新版本必须提供真实 rpm（`aino-player-<版本>-1.x86_64.rpm`）和 `SHA256SUMS.txt`。
- 同时比对 GitHub 附件 digest 和 `SHA256SUMS.txt`，再以 `rpm -qp` 检查包名、版本、发布号和架构。
- 先以 `rpmsign` 签每个包，再执行 `createrepo_c`，核对 primary 索引中的 SHA-256 与签名后的文件一致，最后对 `repomd.xml` 分离签名（`repo_gpgcheck`）。
- 与 APT 源共用同一把 Ed25519 密钥；公钥以 `aino-player.asc` 形式发布在仓库目录，私钥只在 GitHub Actions secret `APT_SIGNING_PRIVATE_KEY` 与维护者备份中，不能提交到 Git。签名固定到仓库内的完整公钥指纹。
- 部署前用 `gpgv` 验签 `repomd.xml`，并在隔离 installroot 中执行 `dnf makecache`，确认有安装候选；不改 runner 的软件源，也不安装播放器。
- rpm job 运行在 Fedora 容器内（需要 `createrepo_c`、`rpm-sign`、`gnupg2`），与 APT 和 Arch 仓库树合流进同一次 GitHub Pages 部署。
- 手动重新部署：GitHub → Actions → **Publish package repositories** → Run workflow，选择 `master`。RPM 源在首个包含 rpm 附件的正式版发布后才有内容；此前手动触发会在 rpm job 明确报错。
