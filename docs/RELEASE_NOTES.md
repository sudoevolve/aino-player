## aino player v0.5.6

- Linux 安装包默认使用纯 Wayland，主窗口和桌面模块不再链接 X11，也不要求安装 XWayland；X11 后端仍可单独构建。
- 悬浮歌词使用原生 Wayland：桌面支持 layer-shell 时提供透明置顶、锁定穿透、拖动和屏幕顶边停靠；其他桌面使用普通原生歌词窗口。
- 全局快捷键接入桌面 GlobalShortcuts Portal，异步申请授权，并显示实际绑定结果或失败原因；系统媒体控制继续使用 MPRIS。
- 托盘、媒体控制、文件选择与单实例交接保留 D-Bus 原生路径，文件拖放接入 Wayland；修复隐藏到托盘后恢复窗口的配置流程。
- 发布验收增加无 DISPLAY 的 Weston、layer-shell 歌词及 Portal 契约检查，并拒绝链接 X11 的原生构建。
- 保留 aino UI 库 v0.5.4 和现有输入法路径，补齐原生 Wayland 构建依赖与桌面协议说明。

GNOME 未提供 layer-shell 时，歌词位置与堆叠由桌面管理，不能保证置顶或覆盖顶栏。托盘需要桌面提供 host，全局快捷键需要 Portal 支持并经用户授权。不同桌面的输入法和多屏交互仍需分别实际确认。
