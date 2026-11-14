// 只在此编译单元包含 WinRT / D-Bus，避免系统 SDK 扩散到 UI、业务与音频编译单元。
#include "platform.hpp"
#include <deque>
#include <cmath>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#include <SystemMediaTransportControlsInterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.Streams.h>
#else
#include <dbus/dbus.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#undef None
#undef Status
#undef Bool
#endif

namespace platform {
std::string local_file_path(const std::string& value) {
    if (value.empty() || value.find('\0') != std::string::npos) return {};
    if (value.rfind("file:", 0) != 0) {
        if (value.find("://") != std::string::npos) return {};
        return value;
    }
    size_t begin = 5;
    if (value.compare(begin, 2, "//") == 0) {
        begin += 2;
        const auto end = value.find('/', begin);
        if (end == std::string::npos) return {};
        const auto host = value.substr(begin, end - begin);
        if (!host.empty() && host != "localhost") return {};
        begin = end;
    }
    if (begin >= value.size() || value[begin] != '/' || value.find_first_of("?#", begin) != std::string::npos) return {};
    std::string path;
    const auto nibble = [](unsigned char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; return -1; };
    for (size_t i = begin; i < value.size(); ++i) {
        if (value[i] != '%') { path += value[i]; continue; }
        if (i + 2 >= value.size()) return {};
        const int hi = nibble(value[i + 1]), lo = nibble(value[i + 2]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return {};
        path += char(hi * 16 + lo); i += 2;
    }
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
#endif
    return path;
}
std::vector<std::string> local_uri_list(const std::string& value) {
    std::vector<std::string> paths;
    if (value.size() > 2 * 1024 * 1024) return paths;
    std::istringstream lines(value); std::string line;
    while (paths.size() < 10000 && std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line.rfind("file:", 0) != 0) continue;
        auto path = local_file_path(line);
        if (!path.empty()) paths.push_back(std::move(path));
    }
    return paths;
}
namespace {
constexpr uintptr_t kOpenFilesMessage = 0x41494e4f;
const char* const audio_mimes[]{"audio/mpeg", "audio/flac", "audio/x-flac", "audio/wav", "audio/x-wav",
    "audio/mp4", "audio/x-m4a", "audio/aac", "audio/x-aac", "audio/ogg", "audio/x-vorbis+ogg", "application/ogg", "audio/opus", "audio/webm"};
#ifndef _WIN32
// 拖拽源可随时关闭窗口；只忽略独立拖放连接上的 BadWindow，其他连接仍使用原处理器。
std::atomic<Display*> drop_error_display{nullptr};
std::atomic<XErrorHandler> previous_x_error{nullptr};
int drop_x_error(Display* display, XErrorEvent* event) {
    if (display == drop_error_display.load() && event->error_code == BadWindow) return 0;
    const auto previous = previous_x_error.load();
    return previous ? previous(display, event) : 0;
}
#endif
struct EventQueue {
    std::mutex mutex;
    std::deque<MediaEvent> pending;
    std::function<void()> wake;
    bool closed = false;
    unsigned long long last = 0;
    MediaAction last_action = MediaAction::Show;
    bool push(MediaEvent event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) return false;
        const auto now = ticks();
        if (event.action == MediaAction::Toggle && last_action == event.action && now < last + 80) return true;
        if (pending.size() >= 64) return false;
        last = now; last_action = event.action; pending.push_back(std::move(event));
        if (wake) wake();
        return true;
    }
};
}
struct Desktop::Impl {
    std::shared_ptr<EventQueue> queue = std::make_shared<EventQueue>();
    std::mutex mutex;
    MediaState state;
    std::atomic<bool> tray{false}, media{false}, hidden{false}, enabled{true}, stopping{false};
    int icon_w = 0, icon_h = 0;
    std::vector<uint8_t> icon;
#ifdef _WIN32
    HWND window = nullptr;
    WNDPROC previous = nullptr;
    UINT taskbar_created = 0;
    NOTIFYICONDATAW notify{};
    winrt::Windows::Media::SystemMediaTransportControls controls{nullptr};
    winrt::event_token button_token{}, seek_token{};
    MediaState published;
    unsigned long long published_at = 0;
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = static_cast<Impl*>(GetPropW(window, L"AinoPlayerDesktop"));
        if (!self) return DefWindowProcW(window, message, w, l);
        if (message == WM_DROPFILES) {
            const auto drop = reinterpret_cast<HDROP>(w);
            std::vector<std::string> files;
            const auto count = std::min<UINT>(DragQueryFileW(drop, 0xffffffff, nullptr, 0), 10000);
            for (UINT i = 0; i < count; ++i) {
                const auto length = DragQueryFileW(drop, i, nullptr, 0);
                if (!length || length > 32767) continue;
                std::wstring path(length + 1, L'\0');
                if (DragQueryFileW(drop, i, path.data(), length + 1)) { path.resize(length); files.push_back(narrow(path)); }
            }
            DragFinish(drop);
            if (!files.empty()) self->queue->push({MediaAction::OpenFiles, 0, {}, std::move(files)});
            return 0;
        }
        if (message == WM_COPYDATA) {
            const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(l);
            if (!data || data->dwData != kOpenFilesMessage || !data->lpData || !data->cbData || data->cbData > 2 * 1024 * 1024) return FALSE;
            const auto* raw = static_cast<const char*>(data->lpData);
            if (raw[data->cbData - 1] != '\0') return FALSE;
            std::vector<std::string> paths;
            for (size_t begin = 0; begin < data->cbData && paths.size() < 10000;) {
                const auto length = size_t(std::find(raw + begin, raw + data->cbData, '\0') - (raw + begin));
                if (length) paths.emplace_back(raw + begin, length);
                begin += length + 1;
            }
            if (paths.empty()) return FALSE;
            return self->queue->push({MediaAction::OpenFiles, 0, {}, std::move(paths)}) ? TRUE : FALSE;
        }
        if (message == self->taskbar_created) { self->tray.store(false); self->sync_tray(); return 0; }
        if (message == WM_APP + 117) {
            const auto action = LOWORD(l);
            if (action == WM_LBUTTONUP || action == NIN_SELECT || action == NIN_KEYSELECT)
                self->queue->push({MediaAction::Show});
            if (action == WM_RBUTTONUP || action == WM_CONTEXTMENU) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"显示播放器");
                AppendMenuW(menu, MF_STRING, 2, self->state.playing ? L"暂停" : L"播放");
                AppendMenuW(menu, MF_STRING, 3, L"下一首");
                AppendMenuW(menu, MF_STRING, 4, L"上一首");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 5, L"退出");
                POINT point{}; GetCursorPos(&point); SetForegroundWindow(window);
                const auto id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
                DestroyMenu(menu); PostMessageW(window, WM_NULL, 0, 0);
                const MediaAction actions[]{MediaAction::Show, MediaAction::Show, MediaAction::Toggle, MediaAction::Next, MediaAction::Previous, MediaAction::Quit};
                if (id >= 1 && id <= 5) self->queue->push({actions[id]});
            }
            return 0;
        }
        if (message == WM_APPCOMMAND) {
            const int command = GET_APPCOMMAND_LPARAM(l);
            if (command == APPCOMMAND_MEDIA_PLAY_PAUSE) self->queue->push({MediaAction::Toggle});
            else if (command == APPCOMMAND_MEDIA_NEXTTRACK) self->queue->push({MediaAction::Next});
            else if (command == APPCOMMAND_MEDIA_PREVIOUSTRACK) self->queue->push({MediaAction::Previous});
            else if (command == APPCOMMAND_MEDIA_STOP) self->queue->push({MediaAction::Stop});
            else if (command == APPCOMMAND_MEDIA_PLAY) self->queue->push({MediaAction::Play});
            else if (command == APPCOMMAND_MEDIA_PAUSE) self->queue->push({MediaAction::Pause});
            else return CallWindowProcW(self->previous, window, message, w, l);
            return TRUE;
        }
        return CallWindowProcW(self->previous, window, message, w, l);
    }
    void sync_tray() {
        if (!window) return;
        if (enabled.load() && !tray.load()) {
            notify.cbSize = sizeof notify; notify.hWnd = window; notify.uID = 1;
            notify.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; notify.uCallbackMessage = WM_APP + 117;
            notify.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
            if (!notify.hIcon) notify.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
            wcscpy_s(notify.szTip, L"Aino Player");
            tray.store(Shell_NotifyIconW(NIM_ADD, &notify) != 0);
            if (tray.load()) { notify.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &notify); }
        } else if (!enabled.load() && tray.exchange(false)) Shell_NotifyIconW(NIM_DELETE, &notify);
    }
    void start_media() {
        using namespace winrt::Windows::Media;
        try {
            const auto factory = winrt::get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
            winrt::check_hresult(factory->GetForWindow(window, winrt::guid_of<SystemMediaTransportControls>(), winrt::put_abi(controls)));
            controls.IsEnabled(true); controls.IsPlayEnabled(true); controls.IsPauseEnabled(true);
            controls.IsNextEnabled(true); controls.IsPreviousEnabled(true); controls.IsStopEnabled(true);
            const auto events = queue;
            button_token = controls.ButtonPressed([events](auto const&, auto const& args) {
                switch (args.Button()) {
                    case SystemMediaTransportControlsButton::Play: events->push({MediaAction::Play}); break;
                    case SystemMediaTransportControlsButton::Pause: events->push({MediaAction::Pause}); break;
                    case SystemMediaTransportControlsButton::Next: events->push({MediaAction::Next}); break;
                    case SystemMediaTransportControlsButton::Previous: events->push({MediaAction::Previous}); break;
                    case SystemMediaTransportControlsButton::Stop: events->push({MediaAction::Stop}); break;
                    default: break;
                }
            });
            seek_token = controls.PlaybackPositionChangeRequested([events](auto const&, auto const& args) {
                events->push({MediaAction::SeekAbsolute, args.RequestedPlaybackPosition().count() / 10000});
            });
            media.store(true);
        } catch (const winrt::hresult_error&) { controls = nullptr; media.store(false); }
    }
    void publish(const MediaState& value) {
        if (!controls) return;
        const auto now = ticks();
        const bool changed = published.uid != value.uid || published.title != value.title ||
            published.artist != value.artist || published.album != value.album || published.artwork != value.artwork ||
            published.playing != value.playing || published.paused != value.paused || published.duration_ms != value.duration_ms ||
            std::llabs(published.position_ms - value.position_ms) > 1500;
        if (!changed && now < published_at + 500) return;
        using namespace winrt::Windows::Media;
        try {
            controls.IsEnabled(!value.uid.empty());
            controls.PlaybackStatus(value.playing ? MediaPlaybackStatus::Playing : value.paused ? MediaPlaybackStatus::Paused : MediaPlaybackStatus::Stopped);
            if (published.uid != value.uid || published.artwork != value.artwork || published.title != value.title || published.artist != value.artist || published.album != value.album) {
                auto display = controls.DisplayUpdater(); display.ClearAll(); display.Type(MediaPlaybackType::Music);
                auto music = display.MusicProperties(); music.Title(winrt::to_hstring(value.title));
                music.Artist(winrt::to_hstring(value.artist)); music.AlbumTitle(winrt::to_hstring(value.album));
                if (value.artwork.rfind("https://", 0) == 0 || value.artwork.rfind("http://", 0) == 0)
                    display.Thumbnail(winrt::Windows::Storage::Streams::RandomAccessStreamReference::CreateFromUri(winrt::Windows::Foundation::Uri(winrt::to_hstring(value.artwork))));
                display.Update();
            }
            SystemMediaTransportControlsTimelineProperties timeline;
            const auto end = std::max(value.duration_ms, value.position_ms);
            timeline.StartTime(std::chrono::milliseconds(0)); timeline.MinSeekTime(std::chrono::milliseconds(0));
            timeline.EndTime(std::chrono::milliseconds(end)); timeline.MaxSeekTime(std::chrono::milliseconds(end));
            timeline.Position(std::chrono::milliseconds(std::clamp(value.position_ms, 0LL, end)));
            controls.UpdateTimelineProperties(timeline);
            published = value; published_at = now;
        } catch (const winrt::hresult_error&) { media.store(false); }
    }
#else
    Display* display = nullptr;
    ::Window window = 0;
    std::thread worker;
    Display* drops = nullptr; // 独立 X11 连接接收 XdndProxy 事件，不取走 aino 的键鼠 / IME 事件。
    ::Window drop_proxy = 0, drop_source = 0;
    Atom xdnd_enter = 0, xdnd_position = 0, xdnd_leave = 0, xdnd_drop = 0, xdnd_status = 0, xdnd_finished = 0;
    Atom xdnd_selection = 0, uri_list = 0, drop_property = 0, copy_action = 0;
    unsigned long long drop_deadline = 0;
    int drop_version = 0;
    bool drop_supported = false, drop_waiting = false;
    void drop_reply(Atom message, bool accepted) {
        if (!drops || !drop_source) return;
        XEvent reply{}; reply.xclient.type = ClientMessage; reply.xclient.display = drops;
        reply.xclient.window = drop_source; reply.xclient.message_type = message; reply.xclient.format = 32;
        reply.xclient.data.l[0] = window;
        reply.xclient.data.l[1] = accepted ? 1 : 0;
        if (message == xdnd_status) reply.xclient.data.l[4] = accepted ? copy_action : 0;
        else if (drop_version >= 5) reply.xclient.data.l[2] = accepted ? copy_action : 0;
        XSendEvent(drops, drop_source, False, NoEventMask, &reply); XFlush(drops);
    }
    void start_drops() {
        drops = XOpenDisplay(nullptr); if (!drops) return;
        drop_error_display.store(drops); previous_x_error = XSetErrorHandler(drop_x_error);
        drop_proxy = XCreateSimpleWindow(drops, DefaultRootWindow(drops), 0, 0, 1, 1, 0, 0, 0);
        auto atom = [&](const char* name) { return XInternAtom(drops, name, False); };
        xdnd_enter = atom("XdndEnter"); xdnd_position = atom("XdndPosition"); xdnd_leave = atom("XdndLeave");
        xdnd_drop = atom("XdndDrop"); xdnd_status = atom("XdndStatus"); xdnd_finished = atom("XdndFinished");
        xdnd_selection = atom("XdndSelection"); uri_list = atom("text/uri-list"); drop_property = atom("_AINO_PLAYER_DROP"); copy_action = atom("XdndActionCopy");
        const auto proxy = atom("XdndProxy"), aware = atom("XdndAware"); const unsigned long version = 5;
        XChangeProperty(drops, drop_proxy, proxy, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&drop_proxy), 1);
        XChangeProperty(drops, window, proxy, XA_WINDOW, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&drop_proxy), 1);
        XChangeProperty(drops, window, aware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&version), 1);
        XChangeProperty(drops, drop_proxy, aware, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&version), 1);
        XFlush(drops);
    }
    void poll_drops() {
        while (drops && XPending(drops)) {
            XEvent event{}; XNextEvent(drops, &event);
            if (event.type == ClientMessage) {
                const auto& data = event.xclient.data.l;
                if (event.xclient.format != 32) continue;
                if (event.xclient.message_type == xdnd_enter) {
                    if (drop_waiting) drop_reply(xdnd_finished, false);
                    drop_source = static_cast<::Window>(data[0]); drop_version = int((data[1] >> 24) & 255);
                    drop_waiting = false; drop_supported = false;
                    if (!drop_source || drop_version < 3) continue;
                    if (!(data[1] & 1)) { for (int i = 2; i < 5; ++i) drop_supported |= Atom(data[i]) == uri_list; }
                    else {
                        Atom type = 0; int format = 0; unsigned long count = 0, remaining = 0; unsigned char* bytes = nullptr;
                        if (XGetWindowProperty(drops, drop_source, XInternAtom(drops, "XdndTypeList", False), 0, 256, False,
                            XA_ATOM, &type, &format, &count, &remaining, &bytes) == Success && type == XA_ATOM && format == 32) {
                            const auto* types = reinterpret_cast<const Atom*>(bytes);
                            for (unsigned long i = 0; i < count; ++i) drop_supported |= types[i] == uri_list;
                        }
                        if (bytes) XFree(bytes);
                    }
                } else if (static_cast<::Window>(data[0]) == drop_source) {
                    if (event.xclient.message_type == xdnd_position) drop_reply(xdnd_status, drop_supported && !drop_waiting);
                    else if (event.xclient.message_type == xdnd_leave && !drop_waiting) { drop_source = 0; drop_supported = false; }
                    else if (event.xclient.message_type == xdnd_drop) {
                        if (!drop_supported || drop_waiting) { drop_reply(xdnd_finished, false); continue; }
                        drop_waiting = true; drop_deadline = ticks() + 5000;
                        XConvertSelection(drops, xdnd_selection, uri_list, drop_property, drop_proxy, data[2] ? Time(data[2]) : CurrentTime);
                        XFlush(drops);
                    }
                }
            } else if (event.type == SelectionNotify && drop_waiting && event.xselection.selection == xdnd_selection) {
                std::vector<std::string> files;
                if (event.xselection.property == drop_property) {
                    Atom type = 0; int format = 0; unsigned long count = 0, remaining = 0; unsigned char* bytes = nullptr;
                    if (XGetWindowProperty(drops, drop_proxy, drop_property, 0, 512 * 1024, True, AnyPropertyType,
                        &type, &format, &count, &remaining, &bytes) == Success && type == uri_list && format == 8 && !remaining && bytes)
                        files = local_uri_list(std::string(reinterpret_cast<const char*>(bytes), count));
                    if (bytes) XFree(bytes);
                    XDeleteProperty(drops, drop_proxy, drop_property); // 不接收无界 INCR 传输。
                }
                const bool accepted = !files.empty() && queue->push({MediaAction::OpenFiles, 0, {}, std::move(files)});
                drop_reply(xdnd_finished, accepted); drop_waiting = false; drop_source = 0;
            }
        }
        if (drop_waiting && ticks() > drop_deadline) { drop_reply(xdnd_finished, false); drop_waiting = false; drop_source = 0; }
    }
    void stop_drops() {
        if (!drops) return;
        if (drop_waiting) drop_reply(xdnd_finished, false);
        XDeleteProperty(drops, window, XInternAtom(drops, "XdndProxy", False));
        XDeleteProperty(drops, window, XInternAtom(drops, "XdndAware", False));
        if (drop_proxy) XDestroyWindow(drops, drop_proxy);
        XCloseDisplay(drops); drops = nullptr;
        XSetErrorHandler(previous_x_error.load()); drop_error_display.store(nullptr);
    }
    DBusConnection* bus = nullptr; // 只由 worker 使用和关闭。
    std::string bus_name, watcher;
    bool registered = false;
    static constexpr const char* player_iface = "org.mpris.MediaPlayer2.Player";
    static constexpr const char* root_iface = "org.mpris.MediaPlayer2";
    static constexpr const char* tray_iface = "org.kde.StatusNotifierItem";
    static void string(DBusMessageIter& it, const char* value, int type = DBUS_TYPE_STRING) { dbus_message_iter_append_basic(&it, type, &value); }
    static void boolean(DBusMessageIter& it, bool value) { dbus_bool_t b = value; dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &b); }
    static void integer(DBusMessageIter& it, int32_t value) { dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &value); }
    static void uint32(DBusMessageIter& it, uint32_t value) { dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &value); }
    static void int64(DBusMessageIter& it, int64_t value) { dbus_message_iter_append_basic(&it, DBUS_TYPE_INT64, &value); }
    static void real(DBusMessageIter& it, double value) { dbus_message_iter_append_basic(&it, DBUS_TYPE_DOUBLE, &value); }
    template<class F> static void container(DBusMessageIter& it, int type, const char* signature, F fn) {
        DBusMessageIter child; dbus_message_iter_open_container(&it, type, signature, &child); fn(child); dbus_message_iter_close_container(&it, &child);
    }
    template<class F> static void entry(DBusMessageIter& it, const char* key, const char* signature, F fn) {
        container(it, DBUS_TYPE_DICT_ENTRY, nullptr, [&](auto& row) { string(row, key); container(row, DBUS_TYPE_VARIANT, signature, fn); });
    }
    std::string track_path() const { return "/org/mpris/MediaPlayer2/track/t" + std::to_string(std::hash<std::string>{}(state.uid)); }
    bool property(DBusMessageIter& it, const std::string& iface, const std::string& key) {
        const auto str = [&](const char* text, const char* sig = "s") { container(it, DBUS_TYPE_VARIANT, sig, [&](auto& v) { string(v, text, sig[0]); }); };
        const auto b = [&](bool value) { container(it, DBUS_TYPE_VARIANT, "b", [&](auto& v) { boolean(v, value); }); };
        const auto d = [&](double value) { container(it, DBUS_TYPE_VARIANT, "d", [&](auto& v) { real(v, value); }); };
        if (iface == player_iface) {
            if (key == "PlaybackStatus") str(state.playing ? "Playing" : state.paused ? "Paused" : "Stopped");
            else if (key == "LoopStatus") str(state.loop == 2 ? "Track" : state.loop == 1 ? "Playlist" : "None");
            else if (key == "Shuffle") b(state.shuffle);
            else if (key == "Rate" || key == "MinimumRate" || key == "MaximumRate") d(1.0);
            else if (key == "Volume") d(state.volume / 100.0);
            else if (key == "Position") container(it, DBUS_TYPE_VARIANT, "x", [&](auto& v) { int64(v, state.position_ms * 1000); });
            else if (key == "CanSeek") b(state.can_seek);
            else if (key == "CanControl") b(true);
            else if (key == "CanGoNext" || key == "CanGoPrevious" || key == "CanPlay" || key == "CanPause") b(!state.uid.empty());
            else if (key == "Metadata") container(it, DBUS_TYPE_VARIANT, "a{sv}", [&](auto& v) {
                container(v, DBUS_TYPE_ARRAY, "{sv}", [&](auto& dict) {
                    entry(dict, "mpris:trackid", "o", [&](auto& a) { string(a, state.uid.empty() ? "/org/mpris/MediaPlayer2/TrackList/NoTrack" : track_path().c_str(), DBUS_TYPE_OBJECT_PATH); });
                    entry(dict, "mpris:length", "x", [&](auto& a) { int64(a, state.duration_ms * 1000); });
                    entry(dict, "xesam:title", "s", [&](auto& a) { string(a, state.title.c_str()); });
                    entry(dict, "xesam:album", "s", [&](auto& a) { string(a, state.album.c_str()); });
                    entry(dict, "xesam:artist", "as", [&](auto& a) { container(a, DBUS_TYPE_ARRAY, "s", [&](auto& list) { string(list, state.artist.c_str()); }); });
                    if (state.artwork.rfind("https://", 0) == 0 || state.artwork.rfind("http://", 0) == 0)
                        entry(dict, "mpris:artUrl", "s", [&](auto& a) { string(a, state.artwork.c_str()); });
                });
            });
            else return false;
        } else if (iface == root_iface) {
            if (key == "CanQuit" || key == "CanRaise") b(true);
            else if (key == "HasTrackList" || key == "CanSetFullscreen" || key == "Fullscreen") b(false);
            else if (key == "Identity") str("Aino Player");
            else if (key == "DesktopEntry") str("aino-player");
            else if (key == "SupportedUriSchemes" || key == "SupportedMimeTypes") container(it, DBUS_TYPE_VARIANT, "as", [&](auto& v) { container(v, DBUS_TYPE_ARRAY, "s", [&](auto& values) {
                if (key == "SupportedUriSchemes") string(values, "file"); else for (const auto* mime : audio_mimes) string(values, mime);
            }); });
            else return false;
        } else if (iface == tray_iface) {
            if (key == "Category") str("ApplicationStatus");
            else if (key == "Id") str("aino-player");
            else if (key == "Title") str("Aino Player");
            else if (key == "Status") str(enabled.load() ? "Active" : "Passive");
            else if (key == "IconName") str("aino-player");
            else if (key == "IconThemePath" || key == "OverlayIconName" || key == "AttentionIconName" || key == "AttentionMovieName") str("");
            else if (key == "ItemIsMenu") b(false);
            else if (key == "Menu") str("/Menu", "o");
            else if (key == "WindowId") container(it, DBUS_TYPE_VARIANT, "i", [&](auto& v) { integer(v, int32_t(window)); });
            else if (key == "IconPixmap" || key == "OverlayIconPixmap" || key == "AttentionIconPixmap") container(it, DBUS_TYPE_VARIANT, "a(iiay)", [&](auto& v) {
                container(v, DBUS_TYPE_ARRAY, "(iiay)", [&](auto& array) {
                    if (key != "IconPixmap" || icon.empty()) return;
                    container(array, DBUS_TYPE_STRUCT, nullptr, [&](auto& row) {
                        integer(row, icon_w); integer(row, icon_h);
                        container(row, DBUS_TYPE_ARRAY, "y", [&](auto& bytes) { const auto* data = icon.data(); dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data, int(icon.size())); });
                    });
                });
            });
            else if (key == "ToolTip") container(it, DBUS_TYPE_VARIANT, "(sa(iiay)ss)", [&](auto& v) {
                container(v, DBUS_TYPE_STRUCT, nullptr, [&](auto& row) { string(row, "aino-player"); container(row, DBUS_TYPE_ARRAY, "(iiay)", [&](auto&) {}); string(row, "Aino Player"); string(row, state.title.c_str()); });
            });
            else return false;
        } else if (iface == "com.canonical.dbusmenu") {
            if (key == "Version") container(it, DBUS_TYPE_VARIANT, "u", [&](auto& v) { uint32(v, 3); });
            else if (key == "TextDirection") str("ltr");
            else if (key == "Status") str("normal");
            else if (key == "IconThemePath") container(it, DBUS_TYPE_VARIANT, "as", [&](auto& v) { container(v, DBUS_TYPE_ARRAY, "s", [&](auto&) {}); });
            else return false;
        } else return false;
        return true;
    }
    std::vector<const char*> keys(const std::string& iface) {
        if (iface == player_iface) return {"PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume", "Position", "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious", "CanPlay", "CanPause", "CanSeek", "CanControl"};
        if (iface == root_iface) return {"CanQuit", "CanRaise", "HasTrackList", "Identity", "DesktopEntry", "SupportedUriSchemes", "SupportedMimeTypes", "Fullscreen", "CanSetFullscreen"};
        if (iface == tray_iface) return {"Category", "Id", "Title", "Status", "WindowId", "IconName", "IconPixmap", "OverlayIconName", "OverlayIconPixmap", "AttentionIconName", "AttentionIconPixmap", "AttentionMovieName", "IconThemePath", "ToolTip", "ItemIsMenu", "Menu"};
        if (iface == "com.canonical.dbusmenu") return {"Version", "TextDirection", "Status", "IconThemePath"};
        return {};
    }
    void all(DBusMessageIter& it, const std::string& iface, bool changed = false) {
        container(it, DBUS_TYPE_ARRAY, "{sv}", [&](auto& dict) {
            for (const auto* key : keys(iface)) {
                if (changed && std::string(key) == "Position") continue;
                container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, [&](auto& row) { string(row, key); property(row, iface, key); });
            }
        });
    }
    void send(DBusMessage* message) { if (message) { dbus_connection_send(bus, message, nullptr); dbus_message_unref(message); } }
    void error(DBusMessage* message, const char* reason) { send(dbus_message_new_error(message, "org.freedesktop.DBus.Error.InvalidArgs", reason)); }
    static bool arg(DBusMessageIter& it, int type) { return dbus_message_iter_get_arg_type(&it) == type; }
    static std::string read_string(DBusMessageIter& it) { const char* text = nullptr; dbus_message_iter_get_basic(&it, &text); return text ? text : ""; }
    void menu_item(DBusMessageIter& it, int id, int depth) {
        container(it, DBUS_TYPE_STRUCT, nullptr, [&](auto& row) {
            integer(row, id);
            container(row, DBUS_TYPE_ARRAY, "{sv}", [&](auto& dict) {
                const char* labels[]{"", "显示播放器", "播放 / 暂停", "下一首", "上一首", "退出"};
                if (id >= 1 && id <= 5) { entry(dict, "label", "s", [&](auto& v) { string(v, labels[id]); }); entry(dict, "enabled", "b", [&](auto& v) { boolean(v, true); }); entry(dict, "visible", "b", [&](auto& v) { boolean(v, true); }); }
                if (!id) entry(dict, "children-display", "s", [&](auto& v) { string(v, "submenu"); });
            });
            container(row, DBUS_TYPE_ARRAY, "v", [&](auto& children) {
                if (!id && depth != 0) for (int child = 1; child <= 5; ++child)
                    container(children, DBUS_TYPE_VARIANT, "(ia{sv}av)", [&](auto& v) { menu_item(v, child, 0); });
            });
        });
    }
    void dispatch(DBusMessage* request) {
        const auto text = [](const char* value) { return std::string(value ? value : ""); };
        const auto iface = text(dbus_message_get_interface(request)), member = text(dbus_message_get_member(request)), path = text(dbus_message_get_path(request));
        if (dbus_message_get_type(request) != DBUS_MESSAGE_TYPE_METHOD_CALL) return;
        if (path != "/org/mpris/MediaPlayer2" && path != "/StatusNotifierItem" && path != "/Menu") { error(request, "Unknown object"); return; }
        DBusMessage* response = dbus_message_new_method_return(request); if (!response) return;
        DBusMessageIter in{}, out{}; dbus_message_iter_init(request, &in); dbus_message_iter_init_append(response, &out);
        bool valid = true;
        if (iface == "org.freedesktop.DBus.Properties") {
            if (!arg(in, DBUS_TYPE_STRING)) valid = false;
            else {
                const auto target = read_string(in); dbus_message_iter_next(&in);
                if (keys(target).empty() || (path == "/Menu" && target != "com.canonical.dbusmenu") || (path == "/StatusNotifierItem" && target != tray_iface) || (path == "/org/mpris/MediaPlayer2" && target != player_iface && target != root_iface)) valid = false;
                else if (member == "GetAll") all(out, target);
                else if (!arg(in, DBUS_TYPE_STRING)) valid = false;
                else {
                    const auto key = read_string(in); dbus_message_iter_next(&in);
                    if (member == "Get") valid = property(out, target, key);
                    else if (member == "Set" && target == player_iface && arg(in, DBUS_TYPE_VARIANT)) {
                        DBusMessageIter value; dbus_message_iter_recurse(&in, &value);
                        if (key == "Volume" && arg(value, DBUS_TYPE_DOUBLE)) { double v; dbus_message_iter_get_basic(&value, &v); if (std::isfinite(v)) queue->push({MediaAction::Volume, static_cast<long long>(std::clamp(v, 0.0, 1.0) * 100)}); else valid = false; }
                        else if (key == "Shuffle" && arg(value, DBUS_TYPE_BOOLEAN)) { dbus_bool_t v; dbus_message_iter_get_basic(&value, &v); queue->push({MediaAction::Shuffle, v}); }
                        else if (key == "LoopStatus" && arg(value, DBUS_TYPE_STRING)) { const auto v = read_string(value); if (v == "None" || v == "Playlist" || v == "Track") queue->push({MediaAction::Loop, v == "Track" ? 2 : v == "Playlist" ? 1 : 0}); else valid = false; }
                        else if (key == "Rate" && arg(value, DBUS_TYPE_DOUBLE)) { double v; dbus_message_iter_get_basic(&value, &v); valid = v == 1.0; }
                        else valid = false;
                    } else valid = false;
                }
            }
        } else if (iface == root_iface && path == "/org/mpris/MediaPlayer2") {
            if (member == "Raise") queue->push({MediaAction::Show}); else if (member == "Quit") queue->push({MediaAction::Quit}); else valid = false;
        } else if (iface == "io.github.sudoevolve.AinoPlayer" && member == "OpenFiles" && path == "/org/mpris/MediaPlayer2" && arg(in, DBUS_TYPE_ARRAY)) {
            DBusMessageIter values; dbus_message_iter_recurse(&in, &values);
            std::vector<std::string> paths; size_t bytes = 0, count = 0;
            while (arg(values, DBUS_TYPE_STRING) && count++ < 10000) {
                const auto raw = read_string(values); bytes += raw.size() + 1;
                if (bytes > 2 * 1024 * 1024) { valid = false; break; }
                const auto path = local_file_path(raw);
                if (!path.empty()) paths.push_back(path);
                dbus_message_iter_next(&values);
            }
            if (valid && !paths.empty() && dbus_message_iter_get_arg_type(&values) == DBUS_TYPE_INVALID)
                valid = queue->push({MediaAction::OpenFiles, 0, {}, std::move(paths)});
            else valid = false;
        } else if (iface == player_iface && path == "/org/mpris/MediaPlayer2") {
            if (member == "PlayPause") queue->push({MediaAction::Toggle});
            else if (member == "Play") queue->push({MediaAction::Play});
            else if (member == "Pause") queue->push({MediaAction::Pause});
            else if (member == "Stop") queue->push({MediaAction::Stop});
            else if (member == "Next") queue->push({MediaAction::Next});
            else if (member == "Previous") queue->push({MediaAction::Previous});
            else if (member == "OpenUri" && arg(in, DBUS_TYPE_STRING)) {
                const auto path = local_file_path(read_string(in));
                if (path.empty()) valid = false;
                else valid = queue->push({MediaAction::OpenFiles, 0, {}, {path}});
            }
            else if (member == "Seek" && arg(in, DBUS_TYPE_INT64)) { int64_t v; dbus_message_iter_get_basic(&in, &v); queue->push({MediaAction::SeekRelative, v / 1000}); }
            else if (member == "SetPosition" && arg(in, DBUS_TYPE_OBJECT_PATH)) {
                const auto id = read_string(in); dbus_message_iter_next(&in);
                if (!arg(in, DBUS_TYPE_INT64)) valid = false;
                else { int64_t v; dbus_message_iter_get_basic(&in, &v); if (id == track_path() && v >= 0 && v / 1000 <= state.duration_ms) queue->push({MediaAction::SeekAbsolute, v / 1000, state.uid}); }
            } else valid = false;
        } else if (iface == tray_iface && path == "/StatusNotifierItem") {
            if (member == "Activate" || member == "SecondaryActivate" || member == "ContextMenu") queue->push({MediaAction::Show});
            else if (member == "Scroll" && arg(in, DBUS_TYPE_INT32)) { int32_t delta; dbus_message_iter_get_basic(&in, &delta); queue->push({MediaAction::Volume, std::clamp<long long>(state.volume + (delta > 0 ? 5 : -5), 0, 100)}); }
            else valid = false;
        } else if (iface == "com.canonical.dbusmenu" && path == "/Menu") {
            if (member == "GetLayout" && arg(in, DBUS_TYPE_INT32)) {
                int32_t id, depth; dbus_message_iter_get_basic(&in, &id); dbus_message_iter_next(&in);
                if (!arg(in, DBUS_TYPE_INT32) || id < 0 || id > 5) valid = false;
                else { dbus_message_iter_get_basic(&in, &depth); uint32(out, 1); menu_item(out, id, depth); }
            } else if (member == "AboutToShow") boolean(out, false);
            else if (member == "Event" && arg(in, DBUS_TYPE_INT32)) {
                int32_t id; dbus_message_iter_get_basic(&in, &id); dbus_message_iter_next(&in);
                if (!arg(in, DBUS_TYPE_STRING)) valid = false;
                else if (read_string(in) == "clicked" && id >= 1 && id <= 5) { const MediaAction actions[]{MediaAction::Show, MediaAction::Show, MediaAction::Toggle, MediaAction::Next, MediaAction::Previous, MediaAction::Quit}; queue->push({actions[id]}); }
            } else if (member == "GetProperty" && arg(in, DBUS_TYPE_INT32)) {
                int32_t id; dbus_message_iter_get_basic(&in, &id); dbus_message_iter_next(&in);
                if (id < 0 || id > 5 || !arg(in, DBUS_TYPE_STRING)) valid = false;
                else {
                    const auto key = read_string(in);
                    if (key == "label") container(out, DBUS_TYPE_VARIANT, "s", [&](auto& v) { const char* labels[]{"", "显示播放器", "播放 / 暂停", "下一首", "上一首", "退出"}; string(v, labels[id]); });
                    else if (key == "enabled" || key == "visible") container(out, DBUS_TYPE_VARIANT, "b", [&](auto& v) { boolean(v, true); });
                    else if (!id && key == "children-display") container(out, DBUS_TYPE_VARIANT, "s", [&](auto& v) { string(v, "submenu"); });
                    else valid = false;
                }
            } else if (member == "EventGroup" && arg(in, DBUS_TYPE_ARRAY)) {
                std::vector<int32_t> errors; DBusMessageIter list; dbus_message_iter_recurse(&in, &list);
                while (arg(list, DBUS_TYPE_STRUCT)) {
                    DBusMessageIter row; dbus_message_iter_recurse(&list, &row);
                    if (arg(row, DBUS_TYPE_INT32)) {
                        int32_t id; dbus_message_iter_get_basic(&row, &id); dbus_message_iter_next(&row);
                        if (id < 1 || id > 5 || !arg(row, DBUS_TYPE_STRING)) errors.push_back(id);
                        else if (read_string(row) == "clicked") { const MediaAction actions[]{MediaAction::Show, MediaAction::Show, MediaAction::Toggle, MediaAction::Next, MediaAction::Previous, MediaAction::Quit}; queue->push({actions[id]}); }
                    }
                    dbus_message_iter_next(&list);
                }
                container(out, DBUS_TYPE_ARRAY, "i", [&](auto& list) { for (const auto id : errors) integer(list, id); });
            } else if (member == "AboutToShowGroup" && arg(in, DBUS_TYPE_ARRAY)) {
                container(out, DBUS_TYPE_ARRAY, "i", [&](auto&) {}); container(out, DBUS_TYPE_ARRAY, "i", [&](auto&) {});
            } else if (member == "GetGroupProperties") {
                container(out, DBUS_TYPE_ARRAY, "(ia{sv})", [&](auto& list) {
                    for (int id = 0; id <= 5; ++id) container(list, DBUS_TYPE_STRUCT, nullptr, [&](auto& row) {
                        integer(row, id); container(row, DBUS_TYPE_ARRAY, "{sv}", [&](auto& dict) {
                            const char* labels[]{"", "显示播放器", "播放 / 暂停", "下一首", "上一首", "退出"};
                            entry(dict, "label", "s", [&](auto& v) { string(v, labels[id]); });
                            entry(dict, "enabled", "b", [&](auto& v) { boolean(v, true); });
                            entry(dict, "visible", "b", [&](auto& v) { boolean(v, true); });
                        });
                    });
                });
            } else valid = false;
        } else if (iface == "org.freedesktop.DBus.Peer" && member == "Ping") {}
        else if (iface == "org.freedesktop.DBus.Introspectable" && member == "Introspect") {
            std::string xml = "<node>";
            const auto interface_xml = [&](const std::string& name, const std::string& methods) {
                xml += "<interface name='" + name + "'>" + methods;
                for (const auto* raw : keys(name)) {
                    const std::string key = raw; std::string signature = "s";
                    if (key.rfind("Can", 0) == 0 || key == "Shuffle" || key == "HasTrackList" || key == "Fullscreen" || key == "ItemIsMenu") signature = "b";
                    else if (key == "Rate" || key == "Volume" || key == "MinimumRate" || key == "MaximumRate") signature = "d";
                    else if (key == "Position") signature = "x";
                    else if (key == "Metadata") signature = "a{sv}";
                    else if (key == "SupportedUriSchemes" || key == "SupportedMimeTypes" || (name == "com.canonical.dbusmenu" && key == "IconThemePath")) signature = "as";
                    else if (key == "WindowId") signature = "i";
                    else if (key == "Version") signature = "u";
                    else if (key == "Menu") signature = "o";
                    else if (key == "ToolTip") signature = "(sa(iiay)ss)";
                    else if (key.find("Pixmap") != std::string::npos) signature = "a(iiay)";
                    const bool writable = name == player_iface && (key == "Rate" || key == "Volume" || key == "Shuffle" || key == "LoopStatus");
                    xml += "<property name='" + key + "' type='" + signature + "' access='" + (writable ? "readwrite" : "read") + "'/>";
                }
                xml += "</interface>";
            };
            if (path == "/org/mpris/MediaPlayer2") {
                interface_xml(root_iface, "<method name='Raise'/><method name='Quit'/>");
                interface_xml(player_iface, "<method name='PlayPause'/><method name='Play'/><method name='Pause'/><method name='Stop'/><method name='Next'/><method name='Previous'/><method name='OpenUri'><arg direction='in' type='s'/></method><method name='Seek'><arg direction='in' type='x'/></method><method name='SetPosition'><arg direction='in' type='o'/><arg direction='in' type='x'/></method><signal name='Seeked'><arg type='x'/></signal>");
                interface_xml("io.github.sudoevolve.AinoPlayer", "<method name='OpenFiles'><arg direction='in' type='as'/></method>");
            } else if (path == "/StatusNotifierItem") {
                interface_xml(tray_iface, "<method name='Activate'><arg direction='in' type='i'/><arg direction='in' type='i'/></method><method name='SecondaryActivate'><arg direction='in' type='i'/><arg direction='in' type='i'/></method><method name='ContextMenu'><arg direction='in' type='i'/><arg direction='in' type='i'/></method><method name='Scroll'><arg direction='in' type='i'/><arg direction='in' type='s'/></method><signal name='NewIcon'/><signal name='NewStatus'><arg type='s'/></signal><signal name='NewToolTip'/>");
            } else {
                interface_xml("com.canonical.dbusmenu", "<method name='GetLayout'><arg direction='in' type='i'/><arg direction='in' type='i'/><arg direction='in' type='as'/><arg direction='out' type='u'/><arg direction='out' type='(ia{sv}av)'/></method><method name='GetGroupProperties'><arg direction='in' type='ai'/><arg direction='in' type='as'/><arg direction='out' type='a(ia{sv})'/></method><method name='GetProperty'><arg direction='in' type='i'/><arg direction='in' type='s'/><arg direction='out' type='v'/></method><method name='Event'><arg direction='in' type='i'/><arg direction='in' type='s'/><arg direction='in' type='v'/><arg direction='in' type='u'/></method><method name='AboutToShow'><arg direction='in' type='i'/><arg direction='out' type='b'/></method><method name='EventGroup'><arg direction='in' type='a(isvu)'/><arg direction='out' type='ai'/></method><method name='AboutToShowGroup'><arg direction='in' type='ai'/><arg direction='out' type='ai'/><arg direction='out' type='ai'/></method><signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>");
            }
            xml += "<interface name='org.freedesktop.DBus.Introspectable'><method name='Introspect'><arg direction='out' type='s'/></method></interface><interface name='org.freedesktop.DBus.Peer'><method name='Ping'/></interface><interface name='org.freedesktop.DBus.Properties'><method name='Get'><arg direction='in' type='s'/><arg direction='in' type='s'/><arg direction='out' type='v'/></method><method name='GetAll'><arg direction='in' type='s'/><arg direction='out' type='a{sv}'/></method><method name='Set'><arg direction='in' type='s'/><arg direction='in' type='s'/><arg direction='in' type='v'/></method><signal name='PropertiesChanged'><arg type='s'/><arg type='a{sv}'/><arg type='as'/></signal></interface></node>";
            string(out, xml.c_str());
        } else valid = false;
        if (valid) send(response); else { dbus_message_unref(response); error(request, "Unsupported method, property or arguments"); }
    }
    DBusMessage* call(const char* destination, const char* path, const char* iface, const char* method, const std::vector<std::string>& args) {
        auto* request = dbus_message_new_method_call(destination, path, iface, method); if (!request) return nullptr;
        DBusMessageIter it; dbus_message_iter_init_append(request, &it); for (const auto& value : args) string(it, value.c_str());
        auto* response = dbus_connection_send_with_reply_and_block(bus, request, 500, nullptr); dbus_message_unref(request); return response;
    }
    void sync_watcher() {
        auto* response = call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", {"org.kde.StatusNotifierWatcher"});
        std::string owner;
        if (response) { DBusMessageIter it; if (dbus_message_iter_init(response, &it) && arg(it, DBUS_TYPE_STRING)) owner = read_string(it); dbus_message_unref(response); }
        if (owner != watcher) { watcher = owner; registered = false; tray.store(false); }
        bool host = false;
        if (!owner.empty()) {
            response = call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties", "Get", {"org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered"});
            if (response) { DBusMessageIter it, value; if (dbus_message_iter_init(response, &it) && arg(it, DBUS_TYPE_VARIANT)) { dbus_message_iter_recurse(&it, &value); if (arg(value, DBUS_TYPE_BOOLEAN)) { dbus_bool_t b; dbus_message_iter_get_basic(&value, &b); host = b; } } dbus_message_unref(response); }
        }
        if (!owner.empty() && !registered) {
            response = call("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.kde.StatusNotifierWatcher", "RegisterStatusNotifierItem", {bus_name});
            if (response) { registered = dbus_message_get_type(response) == DBUS_MESSAGE_TYPE_METHOD_RETURN; dbus_message_unref(response); }
        }
        const bool available = registered && host && enabled.load();
        if (!available && tray.exchange(available) && hidden.load()) queue->push({MediaAction::Show}); else tray.store(available);
    }
    void run() {
        start_drops();
        struct Cleanup { Impl* self; ~Cleanup() { self->stop_drops(); } } cleanup{this};
        dbus_threads_init_default();
        bus = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
        if (!bus) { while (!stopping.load()) { poll_drops(); sleep_ms(100); } return; }
        dbus_connection_set_exit_on_disconnect(bus, false);
        bus_name = "org.mpris.MediaPlayer2.aino.instance" + std::to_string(getpid());
        if (dbus_bus_request_name(bus, bus_name.c_str(), DBUS_NAME_FLAG_DO_NOT_QUEUE, nullptr) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { dbus_connection_close(bus); dbus_connection_unref(bus); bus = nullptr; return; }
        media.store(true); unsigned long long checked = 0;
        MediaState prior; bool prior_enabled = !enabled.load();
        while (!stopping.load() && dbus_connection_get_is_connected(bus)) {
            poll_drops();
            const auto now = ticks();
            if (now > checked + 2000) { sync_watcher(); checked = now; }
            std::unique_lock<std::mutex> lock(mutex);
            const bool change = prior.uid != state.uid || prior.playing != state.playing || prior.paused != state.paused || prior.volume != state.volume || prior.shuffle != state.shuffle || prior.loop != state.loop || prior.duration_ms != state.duration_ms || prior.title != state.title || prior.artist != state.artist || prior.album != state.album || prior.can_seek != state.can_seek || prior.artwork != state.artwork;
            if (change) {
                auto* signal = dbus_message_new_signal("/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "PropertiesChanged");
                if (signal) { DBusMessageIter out; dbus_message_iter_init_append(signal, &out); string(out, player_iface); all(out, player_iface, true); container(out, DBUS_TYPE_ARRAY, "s", [&](auto&) {}); send(signal); }
            }
            if (prior.uid == state.uid && std::llabs(prior.position_ms - state.position_ms) > 1500) {
                auto* signal = dbus_message_new_signal("/org/mpris/MediaPlayer2", player_iface, "Seeked");
                if (signal) { DBusMessageIter out; dbus_message_iter_init_append(signal, &out); int64(out, state.position_ms * 1000); send(signal); }
            }
            if (prior_enabled != enabled.load()) {
                auto* signal = dbus_message_new_signal("/StatusNotifierItem", tray_iface, "NewStatus");
                if (signal) { DBusMessageIter out; dbus_message_iter_init_append(signal, &out); string(out, enabled.load() ? "Active" : "Passive"); send(signal); }
                prior_enabled = enabled.load();
            }
            prior = state; lock.unlock();
            dbus_connection_read_write(bus, 100);
            while (auto* request = dbus_connection_pop_message(bus)) { std::lock_guard<std::mutex> held(mutex); dispatch(request); dbus_message_unref(request); }
        }
        media.store(false); tray.store(false);
        if (hidden.load()) queue->push({MediaAction::Show});
        dbus_connection_close(bus); dbus_connection_unref(bus); bus = nullptr;
    }
#endif
};
#ifndef _WIN32
// 门户由桌面提供原生选择器；独立连接和后台线程避免阻塞播放器窗口。
std::string choose_desktop_file(bool folder, const std::atomic<bool>& cancelled, std::string& error,
    int kind, std::vector<std::string>* multi) {
    dbus_threads_init_default();
    auto* bus = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
    if (!bus) { error = "无法连接桌面文件选择器，请粘贴路径"; return {}; }
    dbus_connection_set_exit_on_disconnect(bus, false);
    const auto cleanup = [&] { dbus_connection_close(bus); dbus_connection_unref(bus); };
    dbus_bus_add_match(bus, "type='signal',interface='org.freedesktop.portal.Request',member='Response'", nullptr);
    auto* request = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser", "OpenFile");
    if (!request) { cleanup(); error = "无法创建文件选择请求"; return {}; }
    DBusMessageIter args, options;
    dbus_message_iter_init_append(request, &args);
    const char* parent = ""; const char* title = folder ? "选择音乐文件夹" : kind == 1 ? "打开本地音乐" : kind == 2 ? "选择音源脚本" : "选择本地歌词";
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parent); dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &title);
    dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &options);
    const auto option = [&](const char* key, const char* signature, int type, const void* value) {
        DBusMessageIter row, variant;
        dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr, &row);
        dbus_message_iter_append_basic(&row, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&row, DBUS_TYPE_VARIANT, signature, &variant);
        dbus_message_iter_append_basic(&variant, type, value);
        dbus_message_iter_close_container(&row, &variant); dbus_message_iter_close_container(&options, &row);
    };
    const auto token_text = "aino" + std::to_string(getpid()) + "_" + std::to_string(ticks());
    const char* token = token_text.c_str(); dbus_bool_t directory = folder, multiple = multi && !folder;
    option("handle_token", "s", DBUS_TYPE_STRING, &token); option("directory", "b", DBUS_TYPE_BOOLEAN, &directory); option("multiple", "b", DBUS_TYPE_BOOLEAN, &multiple);
    if (!folder) {
        DBusMessageIter row, variant, filters;
        const char* key = "filters";
        dbus_message_iter_open_container(&options, DBUS_TYPE_DICT_ENTRY, nullptr, &row);
        dbus_message_iter_append_basic(&row, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&row, DBUS_TYPE_VARIANT, "a(sa(us))", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(sa(us))", &filters);
        const auto filter = [&](const char* title, bool all) {
            DBusMessageIter group, patterns;
            dbus_message_iter_open_container(&filters, DBUS_TYPE_STRUCT, nullptr, &group);
            dbus_message_iter_append_basic(&group, DBUS_TYPE_STRING, &title);
            dbus_message_iter_open_container(&group, DBUS_TYPE_ARRAY, "(us)", &patterns);
            const auto pattern = [&](dbus_uint32_t type, const char* value) {
                DBusMessageIter item; dbus_message_iter_open_container(&patterns, DBUS_TYPE_STRUCT, nullptr, &item);
                dbus_message_iter_append_basic(&item, DBUS_TYPE_UINT32, &type); dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING, &value);
                dbus_message_iter_close_container(&patterns, &item);
            };
            if (all) pattern(0, "*");
            else if (kind == 1) for (const auto* mime : audio_mimes) pattern(1, mime);
            else pattern(0, kind == 2 ? "*.[jJ][sS]" : "*.[lL][rR][cC]");
            dbus_message_iter_close_container(&group, &patterns); dbus_message_iter_close_container(&filters, &group);
        };
        filter(kind == 1 ? "音乐文件" : kind == 2 ? "音源脚本" : "LRC 歌词", false); filter("所有文件", true);
        dbus_message_iter_close_container(&variant, &filters); dbus_message_iter_close_container(&row, &variant);
        dbus_message_iter_close_container(&options, &row);
    }
    dbus_message_iter_close_container(&args, &options);
    auto* response = dbus_connection_send_with_reply_and_block(bus, request, 3000, nullptr); dbus_message_unref(request);
    std::string handle, selected;
    if (response) {
        const char* path = nullptr;
        if (dbus_message_get_args(response, nullptr, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID)) handle = path;
        dbus_message_unref(response);
    }
    if (handle.empty()) { cleanup(); error = "桌面文件选择门户不可用，请粘贴路径"; return {}; }
    const auto deadline = ticks() + 10 * 60 * 1000; bool finished = false;
    while (!finished && !cancelled.load() && ticks() < deadline && dbus_connection_get_is_connected(bus)) {
        dbus_connection_read_write(bus, 100);
        while (auto* message = dbus_connection_pop_message(bus)) {
            const char* path = dbus_message_get_path(message);
            if (path && handle == path && dbus_message_is_signal(message, "org.freedesktop.portal.Request", "Response")) {
                finished = true; DBusMessageIter in;
                if (dbus_message_iter_init(message, &in) && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_UINT32) {
                    dbus_uint32_t code; dbus_message_iter_get_basic(&in, &code); dbus_message_iter_next(&in);
                    if (code == 0 && dbus_message_iter_get_arg_type(&in) == DBUS_TYPE_ARRAY) {
                        DBusMessageIter rows; dbus_message_iter_recurse(&in, &rows);
                        while (dbus_message_iter_get_arg_type(&rows) == DBUS_TYPE_DICT_ENTRY) {
                            DBusMessageIter row, variant, uris; dbus_message_iter_recurse(&rows, &row);
                            const char* key = nullptr;
                            if (dbus_message_iter_get_arg_type(&row) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&row, &key);
                            dbus_message_iter_next(&row);
                            if (key && std::string(key) == "uris" && dbus_message_iter_get_arg_type(&row) == DBUS_TYPE_VARIANT) {
                                dbus_message_iter_recurse(&row, &variant);
                                if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_ARRAY) {
                                    dbus_message_iter_recurse(&variant, &uris);
                                    while (dbus_message_iter_get_arg_type(&uris) == DBUS_TYPE_STRING) {
                                        const char* uri = nullptr; dbus_message_iter_get_basic(&uris, &uri);
                                        const auto path = local_file_path(uri ? uri : "");
                                        if (!path.empty()) {
                                            if (selected.empty()) selected = path;
                                            if (multi && multi->size() < 10000) multi->push_back(path);
                                        }
                                        dbus_message_iter_next(&uris);
                                    }
                                }
                            }
                            dbus_message_iter_next(&rows);
                        }
                    } else if (code != 1) error = "桌面文件选择失败，请粘贴路径";
                }
            }
            dbus_message_unref(message);
        }
    }
    if (!finished) {
        request = dbus_message_new_method_call("org.freedesktop.portal.Desktop", handle.c_str(), "org.freedesktop.portal.Request", "Close");
        if (request) { dbus_message_set_no_reply(request, true); dbus_connection_send(bus, request, nullptr); dbus_connection_flush(bus); dbus_message_unref(request); }
        if (!cancelled.load()) error = "文件选择已超时，请重试或粘贴路径";
    }
    cleanup(); return cancelled.load() ? std::string() : selected;
}
#endif
bool forward_local_files(const std::vector<std::string>& paths) {
    if (paths.empty() || paths.size() > 10000) return false;
    size_t bytes = 0;
    for (const auto& path : paths) { bytes += path.size() + 1; if (bytes > 2 * 1024 * 1024) return false; }
#ifdef _WIN32
    struct Search { HWND target = nullptr; } search;
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
        if (!GetPropW(window, L"AinoPlayerDesktop")) return TRUE;
        reinterpret_cast<Search*>(data)->target = window; return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.target) return false;
    std::string payload;
    for (const auto& path : paths) { payload += path; payload += '\0'; }
    if (payload.size() > 2 * 1024 * 1024) return false;
    COPYDATASTRUCT data{}; data.dwData = kOpenFilesMessage; data.cbData = DWORD(payload.size()); data.lpData = payload.data();
    DWORD_PTR accepted = 0;
    return SendMessageTimeoutW(search.target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &accepted) && accepted;
#else
    dbus_threads_init_default(); auto* bus = dbus_bus_get_private(DBUS_BUS_SESSION, nullptr);
    if (!bus) return false;
    dbus_connection_set_exit_on_disconnect(bus, false);
    std::vector<std::string> players;
    auto* request = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames");
    auto* reply = request ? dbus_connection_send_with_reply_and_block(bus, request, 1000, nullptr) : nullptr;
    if (request) dbus_message_unref(request);
    if (reply) {
        DBusMessageIter root, names;
        if (dbus_message_iter_init(reply, &root) && dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
            dbus_message_iter_recurse(&root, &names);
            while (dbus_message_iter_get_arg_type(&names) == DBUS_TYPE_STRING) {
                const char* name = nullptr; dbus_message_iter_get_basic(&names, &name);
                if (name && std::string(name).rfind("org.mpris.MediaPlayer2.aino.instance", 0) == 0) players.emplace_back(name);
                dbus_message_iter_next(&names);
            }
        }
        dbus_message_unref(reply);
    }
    bool accepted = false;
    for (const auto& name : players) {
        request = dbus_message_new_method_call(name.c_str(), "/org/mpris/MediaPlayer2", "io.github.sudoevolve.AinoPlayer", "OpenFiles");
        if (!request) continue;
        DBusMessageIter root, files; dbus_message_iter_init_append(request, &root);
        dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY, "s", &files);
        for (const auto& path : paths) { const char* text = path.c_str(); dbus_message_iter_append_basic(&files, DBUS_TYPE_STRING, &text); }
        dbus_message_iter_close_container(&root, &files);
        reply = dbus_connection_send_with_reply_and_block(bus, request, 1000, nullptr); dbus_message_unref(request);
        if (reply) { accepted = dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN; dbus_message_unref(reply); }
        if (accepted) break;
    }
    dbus_connection_close(bus); dbus_connection_unref(bus); return accepted;
#endif
}
bool set_default_music_player(std::string& error, const std::function<bool()>& cancelled) {
    error.clear();
    if (cancelled && cancelled()) return false;
#ifdef _WIN32
    wchar_t path[32768]{}; const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) { error = "无法读取播放器路径"; return false; }
    const std::wstring executable(path, length);
    auto write = [&](const std::wstring& key, const wchar_t* name, const std::wstring& value) {
        HKEY handle = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) != ERROR_SUCCESS) return false;
        const auto code = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(handle); return code == ERROR_SUCCESS;
    };
    const std::wstring capabilities = L"Software\\AinoPlayer\\Capabilities", progid = L"Software\\Classes\\AinoPlayer.Audio";
    bool ok = write(capabilities, L"ApplicationName", L"Aino Player") && write(capabilities, L"ApplicationDescription", L"本地与在线音乐播放器") &&
        write(capabilities, L"ApplicationIcon", L"\"" + executable + L"\",0") &&
        write(L"Software\\RegisteredApplications", L"AinoPlayer", capabilities) && write(progid, nullptr, L"Aino 音乐文件") &&
        write(progid + L"\\DefaultIcon", nullptr, L"\"" + executable + L"\",0") &&
        write(progid + L"\\shell\\open\\command", nullptr, L"\"" + executable + L"\" --open \"%1\"");
    for (const auto* extension : {L".mp3", L".flac", L".wav", L".wave", L".m4a", L".aac", L".ogg", L".oga", L".opus", L".webm"}) {
        ok = write(capabilities + L"\\FileAssociations", extension, L"AinoPlayer.Audio") && ok;
        HKEY handle = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + extension + L"\\OpenWithProgids").c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr) == ERROR_SUCCESS) {
            ok = RegSetValueExW(handle, L"AinoPlayer.Audio", 0, REG_NONE, nullptr, 0) == ERROR_SUCCESS && ok; RegCloseKey(handle);
        } else ok = false;
    }
    if (!ok) { error = "无法注册音乐文件关联，请检查当前用户的注册表权限"; return false; }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    // Windows 保护 UserChoice；注册可选应用后由用户在系统设置中完成选择。
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=AinoPlayer", nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
        error = "文件类型已注册，请到 Windows 设置 → 应用 → 默认应用选择 Aino Player"; return false;
    }
    return true;
#else
    std::error_code ec;
    const auto executable = fs::read_symlink("/proc/self/exe", ec);
    if (ec || !executable.is_absolute()) { error = "无法读取播放器路径"; return false; }
    if (!fs::is_regular_file(executable, ec) || ec) { error = "播放器文件已移动或更新，请重启后再设置默认应用"; return false; }
    auto data_home = env("XDG_DATA_HOME");
    if (data_home.empty()) { const auto home_dir = env("HOME"); if (!home_dir.empty()) data_home = join(home_dir, ".local/share"); }
    if (data_home.empty() || !fs::u8path(data_home).is_absolute()) { error = "无法读取用户应用目录"; return false; }
    const auto applications = fs::u8path(data_home) / "applications";
    fs::create_directories(applications, ec);
    if (ec) { error = "无法创建用户应用目录：" + ec.message(); return false; }
    const auto file = applications / "aino-player.desktop";
    const bool system_entry = fs::is_regular_file("/usr/share/applications/aino-player.desktop", ec);
    ec.clear();
    const bool user_entry = fs::exists(file, ec);
    if (ec) { error = "无法读取用户应用入口：" + ec.message(); return false; }
    // 安装版使用包提供的 desktop；便携版 / 自定义路径使用用户级入口。
    if (executable != fs::path("/usr/bin/aino_player") || !system_entry || user_entry) {
        std::string quoted = "\"";
        for (char ch : executable.u8string()) {
            if (ch == '\n' || ch == '\r') { error = "播放器路径不能包含换行"; return false; }
            if (ch == '%') quoted += '%';
            if (ch == '\\') quoted += "\\\\\\\\";
            else { if (ch == '"' || ch == '$' || ch == '`') quoted += "\\\\"; quoted += ch; }
        }
        quoted += "\"";
        std::string entry = "[Desktop Entry]\nType=Application\nName=Aino Player\nExec=" + quoted + " --open %U\nIcon=audio-x-generic\nTerminal=false\nCategories=AudioVideo;Audio;Player;\nStartupWMClass=aino-player\nMimeType=";
        for (const auto* mime : audio_mimes) entry += std::string(mime) + ";";
        entry += '\n';
        const auto temporary = file.u8string() + ".tmp";
        { std::ofstream output(fs::u8path(temporary), std::ios::binary | std::ios::trunc); output << entry; output.close(); if (!output) { error = "无法保存用户应用入口"; return false; } }
        fs::rename(fs::u8path(temporary), file, ec);
        if (ec) { error = "无法更新用户应用入口：" + ec.message(); return false; }
    }
    // 刷新用户入口的 MIME 索引；精简桌面未装此工具时，仍可由 xdg-mime 设置关联。
    run_process({"update-desktop-database", applications.u8string()}, 3000, cancelled);
    bool ok = true;
    const auto deadline = ticks() + 15000;
    for (const auto* mime : audio_mimes) {
        if ((cancelled && cancelled()) || ticks() > deadline) { ok = false; break; }
        if (run_process({"xdg-mime", "default", "aino-player.desktop", mime}, 3000, cancelled) != 0) ok = false;
    }
    if (!ok) error = "部分文件类型设置失败，请在系统的“打开方式”中选择 Aino Player";
    return ok;
#endif
}
Desktop::Desktop() : impl_(std::make_unique<Impl>()) {}
Desktop::~Desktop() { stop(); }
void Desktop::start(void* window, void* display, unsigned long xid, std::function<void()> wake) {
    impl_->queue->wake = std::move(wake);
#ifdef _WIN32
    impl_->window = static_cast<HWND>(window);
    if (!SetPropW(impl_->window, L"AinoPlayerDesktop", impl_.get())) return;
    impl_->previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(impl_->window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Impl::procedure)));
    if (!impl_->previous) { RemovePropW(impl_->window, L"AinoPlayerDesktop"); return; }
    DragAcceptFiles(impl_->window, TRUE);
    impl_->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated"); impl_->sync_tray(); impl_->start_media();
#else
    impl_->display = static_cast<Display*>(display); impl_->window = xid;
    impl_->worker = std::thread([this] { impl_->run(); });
#endif
}
void Desktop::stop() {
    if (!impl_) return;
    { std::lock_guard<std::mutex> lock(impl_->queue->mutex); impl_->queue->closed = true; impl_->queue->wake = {}; }
#ifdef _WIN32
    if (impl_->controls) {
        try { impl_->controls.ButtonPressed(impl_->button_token); impl_->controls.PlaybackPositionChangeRequested(impl_->seek_token); impl_->controls.IsEnabled(false); } catch (...) {}
        impl_->controls = nullptr;
    }
    if (impl_->tray.exchange(false)) Shell_NotifyIconW(NIM_DELETE, &impl_->notify);
    if (impl_->window && impl_->previous && IsWindow(impl_->window)) { DragAcceptFiles(impl_->window, FALSE); SetWindowLongPtrW(impl_->window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(impl_->previous)); RemovePropW(impl_->window, L"AinoPlayerDesktop"); }
    impl_->window = nullptr;
#else
    impl_->stopping.store(true); if (impl_->worker.joinable()) impl_->worker.join();
#endif
}
void Desktop::update(const MediaState& state, bool tray_enabled) {
#ifdef _WIN32
    impl_->state = state; impl_->enabled.store(tray_enabled); impl_->sync_tray(); impl_->publish(state);
    if (!tray_enabled && hidden()) show();
#else
    { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->state = state; }
    impl_->enabled.store(tray_enabled);
    if (!tray_enabled && hidden()) show();
#endif
}
std::vector<MediaEvent> Desktop::events() {
    std::lock_guard<std::mutex> lock(impl_->queue->mutex);
    std::vector<MediaEvent> out(impl_->queue->pending.begin(), impl_->queue->pending.end()); impl_->queue->pending.clear(); return out;
}
bool Desktop::tray_available() const { return impl_->tray.load() && impl_->enabled.load(); }
bool Desktop::media_available() const { return impl_->media.load(); }
bool Desktop::hidden() const { return impl_->hidden.load(); }
void Desktop::hide() {
    if (!tray_available()) return;
    impl_->hidden.store(true);
#ifdef _WIN32
    ShowWindow(impl_->window, SW_HIDE);
#else
    XUnmapWindow(impl_->display, impl_->window); XFlush(impl_->display);
#endif
}
void Desktop::show() {
    impl_->hidden.store(false);
#ifdef _WIN32
    ShowWindow(impl_->window, SW_RESTORE); SetForegroundWindow(impl_->window);
#else
    XMapRaised(impl_->display, impl_->window); XFlush(impl_->display);
#endif
    if (impl_->queue->wake) impl_->queue->wake();
}
void Desktop::set_icon(int width, int height, const std::vector<uint8_t>& rgba) {
#ifndef _WIN32
    if (width <= 0 || height <= 0 || width > 128 || height > 128 || rgba.size() != size_t(width) * height * 4) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->icon_w = width; impl_->icon_h = height; impl_->icon.resize(rgba.size());
    for (size_t i = 0; i < rgba.size(); i += 4) { impl_->icon[i] = rgba[i + 3]; impl_->icon[i + 1] = rgba[i]; impl_->icon[i + 2] = rgba[i + 1]; impl_->icon[i + 3] = rgba[i + 2]; }
#endif
}
} // namespace platform
