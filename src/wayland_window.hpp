#pragma once
#include "aino.hpp"

#ifdef AINO_PLATFORM_WAYLAND
namespace player {
/** @brief UI 线程控制主表面的映射；隐藏后 aino 仍派发业务帧，不提交新图像。
 * @note 此处集中使用 aino v0.5.4 内部协议接口；升级库时须对照原生窗口状态检查。
 */
inline void set_wayland_visibility(aino::Window &window, bool visible) {
    auto *state = static_cast<aino::detail::WaylandState *>(window.impl->native);
    if (!state || visible == state->visible || state->failed)
        return;
    namespace wl = aino::detail::wl;
    if (!visible) {
        wl::local_destroy(state->frame);
        wl::request(state->surface, 1, static_cast<wl::Proxy *>(nullptr), 0, 0);
        wl::request(state->surface, 6);
        state->visible = false;
        state->completed_frame = false;
    } else {
        // 部分 compositor 在复用旧角色时不再 configure；重建角色后走完整初始序列。
        wl::destroy(state->decoration);
        wl::destroy(state->toplevel);
        wl::destroy(state->xdg_surface);
        state->visible = true;
        state->configured = false;
        aino::detail::wayland_create_role(state);
        if (!state->xdg_surface || !state->toplevel) {
            aino::detail::wayland_fail(state, "cannot recreate the player window role");
            return;
        }
        wl::request(state->surface, 6);
    }
    aino::detail::wayland_flush(state);
    window.wake();
}
} // namespace player
#endif
