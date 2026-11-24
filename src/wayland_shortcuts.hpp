#pragma once
#include "platform.hpp"
#include <dbus/dbus.h>

namespace platform {
/** @brief 桌面 worker 独占的 GlobalShortcuts Portal 会话；授权期间不阻塞 UI 或媒体控制。 */
class WaylandShortcuts {
  public:
    explicit WaylandShortcuts(std::function<void(MediaEvent)> publish) : publish_(std::move(publish)) {}
    ~WaylandShortcuts() { close(); }
    void configure(const ShortcutSettings &settings) {
        close();
        settings_ = settings;
        if (!settings.enabled ||
            std::all_of(settings.keys.begin(), settings.keys.end(), [](int key) { return key == 14; })) {
            publish_({MediaAction::ShortcutStatus, 0, "全局快捷键已关闭"});
            return;
        }
        publish_({MediaAction::ShortcutStatus, 0, "正在等待桌面授权全局快捷键…"});
        bus_ = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
        if (!bus_) {
            fail("无法连接桌面会话");
            return;
        }
        dbus_connection_set_exit_on_disconnect(bus_, false);
        dbus_bus_add_match(
            bus_, "type='signal',sender='org.freedesktop.portal.Desktop',interface='org.freedesktop.portal.Request'",
            nullptr);
        dbus_bus_add_match(
            bus_,
            "type='signal',sender='org.freedesktop.portal.Desktop',interface='org.freedesktop.portal.GlobalShortcuts'",
            nullptr);
        dbus_bus_add_match(
            bus_, "type='signal',sender='org.freedesktop.portal.Desktop',interface='org.freedesktop.portal.Session'",
            nullptr);
        auto *request = method("CreateSession");
        if (!request) {
            fail("无法创建快捷键请求");
            return;
        }
        DBusMessageIter args, options;
        dbus_message_iter_init_append(request, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
        token_ = "aino" + std::to_string(getpid()) + "_" + std::to_string(++serial_);
        option(options, "handle_token", token_);
        option(options, "session_handle_token", token_ + "s");
        dbus_message_iter_close_container(&args, &options);
        send(request, Stage::Creating);
    }
    void poll() {
        if (!bus_)
            return;
        dbus_connection_read_write(bus_, 0);
        if (!dbus_connection_get_is_connected(bus_)) {
            fail("桌面会话已断开");
            return;
        }
        if (pending_ && dbus_pending_call_get_completed(pending_)) {
            auto *reply = dbus_pending_call_steal_reply(pending_);
            dbus_pending_call_unref(pending_);
            pending_ = nullptr;
            const char *path = nullptr;
            const bool valid = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
                               dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
            if (valid)
                request_path_ = path;
            if (reply)
                dbus_message_unref(reply);
            if (!valid) {
                fail("桌面未提供 GlobalShortcuts Portal，或拒绝了请求");
                return;
            }
        }
        while (bus_) {
            auto *message = dbus_connection_pop_message(bus_);
            if (!message)
                break;
            handle(message);
            dbus_message_unref(message);
        }
        if (bus_ && stage_ != Stage::Bound && ticks() >= deadline_)
            fail("快捷键授权等待超时，可重新开启快捷键重试");
    }
    void close() {
        if (pending_) {
            dbus_pending_call_cancel(pending_);
            dbus_pending_call_unref(pending_);
            pending_ = nullptr;
        }
        if (bus_) {
            close_object(request_path_, "org.freedesktop.portal.Request");
            close_object(session_, "org.freedesktop.portal.Session");
            dbus_connection_flush(bus_);
            dbus_connection_close(bus_);
            dbus_connection_unref(bus_);
            bus_ = nullptr;
        }
        request_path_.clear();
        session_.clear();
        stage_ = Stage::Idle;
    }

  private:
    enum class Stage { Idle, Creating, Binding, Bound } stage_ = Stage::Idle;
    DBusConnection *bus_ = nullptr;
    DBusPendingCall *pending_ = nullptr;
    std::function<void(MediaEvent)> publish_;
    ShortcutSettings settings_;
    std::string token_, request_path_, session_;
    unsigned serial_ = 0;
    unsigned long long deadline_ = 0;
    static constexpr const char *service = "org.freedesktop.portal.Desktop";
    static constexpr const char *interface = "org.freedesktop.portal.GlobalShortcuts";
    void fail(const std::string &reason) {
        close();
        publish_({MediaAction::ShortcutError, 0, "Wayland 全局快捷键：" + reason + "；系统媒体控制仍可使用"});
    }
    static void text(DBusMessageIter &it, const std::string &value, int type = DBUS_TYPE_STRING) {
        const char *data = value.c_str();
        dbus_message_iter_append_basic(&it, type, &data);
    }
    static void option(DBusMessageIter &it, const char *key, const std::string &value) {
        DBusMessageIter row, variant;
        dbus_message_iter_open_container(&it, DBUS_TYPE_DICT_ENTRY, nullptr, &row);
        dbus_message_iter_append_basic(&row, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&row, DBUS_TYPE_VARIANT, "s", &variant);
        text(variant, value);
        dbus_message_iter_close_container(&row, &variant);
        dbus_message_iter_close_container(&it, &row);
    }
    DBusMessage *method(const char *name) {
        return dbus_message_new_method_call(service, "/org/freedesktop/portal/desktop", interface, name);
    }
    void close_object(const std::string &path, const char *iface) {
        if (path.empty())
            return;
        auto *request = dbus_message_new_method_call(service, path.c_str(), iface, "Close");
        if (!request)
            return;
        dbus_message_set_no_reply(request, true);
        dbus_connection_send(bus_, request, nullptr);
        dbus_message_unref(request);
    }
    void send(DBusMessage *request, Stage stage) {
        stage_ = stage;
        deadline_ = ticks() + 120000;
        // Response 可能早于方法回包，按规范提前订阅并预测路径。
        std::string sender = dbus_bus_get_unique_name(bus_);
        if (!sender.empty() && sender.front() == ':')
            sender.erase(0, 1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        request_path_ = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token_;
        const bool sent = dbus_connection_send_with_reply(bus_, request, &pending_, 5000);
        dbus_message_unref(request);
        if (!sent || !pending_) {
            fail("发送授权请求失败");
            return;
        }
        dbus_connection_flush(bus_);
    }
    void bind() {
        auto *request = method("BindShortcuts");
        if (!request) {
            fail("无法创建快捷键绑定");
            return;
        }
        DBusMessageIter args, shortcuts, options;
        dbus_message_iter_init_append(request, &args);
        text(args, session_, DBUS_TYPE_OBJECT_PATH);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "(sa{sv})", &shortcuts);
        const char *ids[]{"toggle", "previous", "next", "lyrics"};
        const char *descriptions[]{"播放 / 暂停", "上一首", "下一首", "悬浮歌词"};
        const char *keys[]{"space", "Left", "Right", "l", "p", "b", "n", "d", "F7", "F8", "F9", "F10", "F11", "F12"};
        const char *modifiers[]{"CTRL+ALT+", "CTRL+SHIFT+", "ALT+SHIFT+", "CTRL+ALT+SHIFT+"};
        for (size_t i = 0; i < 4; ++i) {
            const int key = settings_.keys[i], modifier = settings_.modifiers[i];
            if (key < 0 || key >= 14 || modifier < 0 || modifier >= 4)
                continue;
            DBusMessageIter row, properties;
            dbus_message_iter_open_container(&shortcuts, DBUS_TYPE_STRUCT, nullptr, &row);
            text(row, ids[i]);
            dbus_message_iter_open_container(&row, DBUS_TYPE_ARRAY, "{sv}", &properties);
            option(properties, "description", descriptions[i]);
            option(properties, "preferred_trigger", std::string(modifiers[modifier]) + keys[key]);
            dbus_message_iter_close_container(&row, &properties);
            dbus_message_iter_close_container(&shortcuts, &row);
        }
        dbus_message_iter_close_container(&args, &shortcuts);
        text(args, ""); // Portal 可无父窗口授权；不传 X11 window ID。
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
        token_ += "b";
        option(options, "handle_token", token_);
        dbus_message_iter_close_container(&args, &options);
        send(request, Stage::Binding);
    }
    void handle(DBusMessage *message) {
        const auto *path = dbus_message_get_path(message);
        if (dbus_message_is_signal(message, "org.freedesktop.portal.Session", "Closed") && path && session_ == path) {
            fail("快捷键会话已由桌面关闭");
            return;
        }
        if (dbus_message_is_signal(message, interface, "Activated")) {
            DBusMessageIter args;
            if (!dbus_message_iter_init(message, &args) ||
                dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_OBJECT_PATH)
                return;
            const char *session = nullptr, *id = nullptr;
            dbus_message_iter_get_basic(&args, &session);
            dbus_message_iter_next(&args);
            if (!session || session_ != session || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
                return;
            dbus_message_iter_get_basic(&args, &id);
            const char *ids[]{"toggle", "previous", "next", "lyrics"};
            const MediaAction actions[]{MediaAction::Toggle, MediaAction::Previous, MediaAction::Next,
                                        MediaAction::ToggleLyrics};
            for (size_t i = 0; i < 4; ++i)
                if (id && std::string(id) == ids[i])
                    publish_({actions[i]});
            return;
        }
        if (!path || request_path_ != path ||
            !dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response"))
            return;
        DBusMessageIter args, values;
        uint32_t code = 2;
        if (!dbus_message_iter_init(message, &args) || dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32)
            return;
        dbus_message_iter_get_basic(&args, &code);
        request_path_.clear();
        if (code != 0) {
            fail(code == 1 ? "用户取消了快捷键授权" : "桌面拒绝了快捷键请求");
            return;
        }
        if (stage_ == Stage::Creating) {
            dbus_message_iter_next(&args);
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
                fail("会话回包无效");
                return;
            }
            dbus_message_iter_recurse(&args, &values);
            while (dbus_message_iter_get_arg_type(&values) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter row, variant;
                dbus_message_iter_recurse(&values, &row);
                const char *key = nullptr;
                dbus_message_iter_get_basic(&row, &key);
                dbus_message_iter_next(&row);
                if (key && std::string(key) == "session_handle" &&
                    dbus_message_iter_get_arg_type(&row) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&row, &variant);
                    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING ||
                        dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_OBJECT_PATH) {
                        const char *session = nullptr;
                        dbus_message_iter_get_basic(&variant, &session);
                        if (session && dbus_validate_path(session, nullptr))
                            session_ = session;
                    }
                }
                dbus_message_iter_next(&values);
            }
            if (session_.empty()) {
                fail("未返回快捷键会话");
                return;
            }
            if (pending_) {
                dbus_pending_call_cancel(pending_);
                dbus_pending_call_unref(pending_);
                pending_ = nullptr;
            }
            bind();
        } else if (stage_ == Stage::Binding) {
            dbus_message_iter_next(&args);
            if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
                fail("快捷键绑定回包无效");
                return;
            }
            dbus_message_iter_recurse(&args, &values);
            std::string summary;
            while (dbus_message_iter_get_arg_type(&values) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter row, variant;
                dbus_message_iter_recurse(&values, &row);
                const char *key = nullptr;
                dbus_message_iter_get_basic(&row, &key);
                dbus_message_iter_next(&row);
                if (key && std::string(key) == "shortcuts" &&
                    dbus_message_iter_get_arg_type(&row) == DBUS_TYPE_VARIANT) {
                    dbus_message_iter_recurse(&row, &variant);
                    if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
                        DBusMessageIter shortcuts;
                        dbus_message_iter_recurse(&variant, &shortcuts);
                        while (dbus_message_iter_get_arg_type(&shortcuts) == DBUS_TYPE_STRUCT) {
                            DBusMessageIter item, properties;
                            dbus_message_iter_recurse(&shortcuts, &item);
                            const char *id = nullptr;
                            if (dbus_message_iter_get_arg_type(&item) != DBUS_TYPE_STRING)
                                break;
                            dbus_message_iter_get_basic(&item, &id);
                            dbus_message_iter_next(&item);
                            const std::string label = !id                             ? ""
                                                      : std::string(id) == "toggle"   ? "播放 / 暂停"
                                                      : std::string(id) == "previous" ? "上一首"
                                                      : std::string(id) == "next"     ? "下一首"
                                                      : std::string(id) == "lyrics"   ? "悬浮歌词"
                                                                                      : "";
                            std::string trigger = "已授权";
                            if (dbus_message_iter_get_arg_type(&item) == DBUS_TYPE_ARRAY) {
                                dbus_message_iter_recurse(&item, &properties);
                                while (dbus_message_iter_get_arg_type(&properties) == DBUS_TYPE_DICT_ENTRY) {
                                    DBusMessageIter property, value;
                                    dbus_message_iter_recurse(&properties, &property);
                                    const char *name = nullptr;
                                    dbus_message_iter_get_basic(&property, &name);
                                    dbus_message_iter_next(&property);
                                    if (name && std::string(name) == "trigger_description" &&
                                        dbus_message_iter_get_arg_type(&property) == DBUS_TYPE_VARIANT) {
                                        dbus_message_iter_recurse(&property, &value);
                                        if (dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
                                            const char *description = nullptr;
                                            dbus_message_iter_get_basic(&value, &description);
                                            if (description)
                                                trigger = description;
                                        }
                                    }
                                    dbus_message_iter_next(&properties);
                                }
                            }
                            if (!label.empty())
                                summary += (summary.empty() ? "" : "；") + label + "：" + trigger;
                            dbus_message_iter_next(&shortcuts);
                        }
                    }
                }
                dbus_message_iter_next(&values);
            }
            if (summary.empty()) {
                fail("桌面没有绑定所请求的快捷键");
                return;
            }
            stage_ = Stage::Bound;
            publish_({MediaAction::ShortcutStatus, 0, "桌面已授权 · " + summary});
        }
    }
};
} // namespace platform
