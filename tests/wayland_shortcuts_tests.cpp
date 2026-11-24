// 只连接发布检查创建的独立 session bus，用假 Portal 核对 wire 契约，不打开授权对话框。
#include "../src/wayland_shortcuts.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
static void text(DBusMessageIter &it, const char *value, int type = DBUS_TYPE_STRING) {
    dbus_message_iter_append_basic(&it, type, &value);
}
static void option(DBusMessageIter &it, const char *key, const char *value) {
    DBusMessageIter row, variant;
    dbus_message_iter_open_container(&it, DBUS_TYPE_DICT_ENTRY, nullptr, &row);
    text(row, key);
    dbus_message_iter_open_container(&row, DBUS_TYPE_VARIANT, "s", &variant);
    text(variant, value);
    dbus_message_iter_close_container(&row, &variant);
    dbus_message_iter_close_container(&it, &row);
}
int main() {
    DBusConnection *server = nullptr;
    try {
        dbus_threads_init_default();
        server = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
        check(server != nullptr, "isolated D-Bus connection");
        dbus_connection_set_exit_on_disconnect(server, false);
        check(dbus_bus_request_name(server, "org.freedesktop.portal.Desktop", DBUS_NAME_FLAG_DO_NOT_QUEUE, nullptr) ==
                  DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER,
              "fake portal owns the isolated service");
        std::vector<platform::MediaEvent> events;
        platform::WaylandShortcuts client([&](platform::MediaEvent event) { events.push_back(std::move(event)); });
        platform::ShortcutSettings settings;
        bool bound = false, activated = false;
        int binds = 0;
        std::string destination;
        const char *session = "/org/freedesktop/portal/desktop/session/test/aino";
        client.configure(settings);
        const auto deadline = platform::ticks() + 5000;
        while (platform::ticks() < deadline && !activated) {
            dbus_connection_read_write(server, 0);
            while (auto *request = dbus_connection_pop_message(server)) {
                const bool create =
                    dbus_message_is_method_call(request, "org.freedesktop.portal.GlobalShortcuts", "CreateSession");
                const bool bind =
                    dbus_message_is_method_call(request, "org.freedesktop.portal.GlobalShortcuts", "BindShortcuts");
                if (create || bind) {
                    check(std::string(dbus_message_get_signature(request)) == (create ? "a{sv}" : "oa(sa{sv})sa{sv}"),
                          "portal method signature");
                    destination = dbus_message_get_sender(request);
                    DBusMessageIter args, options;
                    dbus_message_iter_init(request, &args);
                    if (bind) {
                        const char *handle = nullptr;
                        dbus_message_iter_get_basic(&args, &handle);
                        check(handle && std::string(handle) == session, "bind uses the created session");
                        dbus_message_iter_next(&args);
                        DBusMessageIter shortcuts;
                        dbus_message_iter_recurse(&args, &shortcuts);
                        int count = 0;
                        while (dbus_message_iter_get_arg_type(&shortcuts) == DBUS_TYPE_STRUCT) {
                            ++count;
                            dbus_message_iter_next(&shortcuts);
                        }
                        check(count == 4, "all requested shortcuts are sent");
                        dbus_message_iter_next(&args);
                        dbus_message_iter_next(&args);
                        ++binds;
                    }
                    dbus_message_iter_recurse(&args, &options);
                    std::string token;
                    while (dbus_message_iter_get_arg_type(&options) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter row, value;
                        dbus_message_iter_recurse(&options, &row);
                        const char *name = nullptr;
                        dbus_message_iter_get_basic(&row, &name);
                        dbus_message_iter_next(&row);
                        dbus_message_iter_recurse(&row, &value);
                        const char *content = nullptr;
                        dbus_message_iter_get_basic(&value, &content);
                        if (name && std::string(name) == "handle_token")
                            token = content;
                        dbus_message_iter_next(&options);
                    }
                    check(!token.empty(), "request provides a unique handle token");
                    std::string sender = destination.substr(1);
                    std::replace(sender.begin(), sender.end(), '.', '_');
                    const std::string path = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
                    auto *response =
                        dbus_message_new_signal(path.c_str(), "org.freedesktop.portal.Request", "Response");
                    dbus_message_set_destination(response, destination.c_str());
                    DBusMessageIter out, dict;
                    dbus_message_iter_init_append(response, &out);
                    uint32_t code = 0;
                    dbus_message_iter_append_basic(&out, DBUS_TYPE_UINT32, &code);
                    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &dict);
                    if (create)
                        option(dict, "session_handle", session);
                    else {
                        DBusMessageIter row, variant, list, item, properties;
                        dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &row);
                        text(row, "shortcuts");
                        dbus_message_iter_open_container(&row, DBUS_TYPE_VARIANT, "a(sa{sv})", &variant);
                        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(sa{sv})", &list);
                        dbus_message_iter_open_container(&list, DBUS_TYPE_STRUCT, nullptr, &item);
                        text(item, "toggle");
                        dbus_message_iter_open_container(&item, DBUS_TYPE_ARRAY, "{sv}", &properties);
                        option(properties, "trigger_description", "Ctrl+Alt+Space");
                        dbus_message_iter_close_container(&item, &properties);
                        dbus_message_iter_close_container(&list, &item);
                        dbus_message_iter_close_container(&variant, &list);
                        dbus_message_iter_close_container(&row, &variant);
                        dbus_message_iter_close_container(&dict, &row);
                        bound = true;
                    }
                    dbus_message_iter_close_container(&out, &dict);
                    // 故意先发送 Response，覆盖授权信号早于方法回包的情况。
                    dbus_connection_send(server, response, nullptr);
                    dbus_message_unref(response);
                    auto *reply = dbus_message_new_method_return(request);
                    dbus_message_iter_init_append(reply, &out);
                    text(out, path.c_str(), DBUS_TYPE_OBJECT_PATH);
                    dbus_connection_send(server, reply, nullptr);
                    dbus_message_unref(reply);
                }
                dbus_message_unref(request);
            }
            if (bound) {
                auto *signal = dbus_message_new_signal("/org/freedesktop/portal/desktop",
                                                       "org.freedesktop.portal.GlobalShortcuts", "Activated");
                dbus_message_set_destination(signal, destination.c_str());
                DBusMessageIter args, options;
                dbus_message_iter_init_append(signal, &args);
                text(args, session, DBUS_TYPE_OBJECT_PATH);
                text(args, "toggle");
                uint64_t timestamp = 1;
                dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT64, &timestamp);
                dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
                dbus_message_iter_close_container(&args, &options);
                dbus_connection_send(server, signal, nullptr);
                dbus_message_unref(signal);
                bound = false;
            }
            dbus_connection_flush(server);
            client.poll();
            activated = std::any_of(events.begin(), events.end(),
                                    [](const auto &event) { return event.action == platform::MediaAction::Toggle; });
            platform::sleep_ms(5);
        }
        check(activated && binds == 1, "authorized activation reaches the player exactly once");
        check(std::any_of(events.begin(), events.end(),
                          [](const auto &event) {
                              return event.action == platform::MediaAction::ShortcutStatus &&
                                     event.track.find("Ctrl+Alt+Space") != std::string::npos;
                          }),
              "actual desktop trigger is reported");
        client.configure(settings);
        settings.enabled = false;
        client.configure(settings);
        client.poll();
        client.close();
        dbus_connection_close(server);
        dbus_connection_unref(server);
        server = nullptr;
        std::cout << "Wayland shortcut Portal contract passed\n";
        return 0;
    } catch (const std::exception &error) {
        if (server) {
            dbus_connection_close(server);
            dbus_connection_unref(server);
        }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
