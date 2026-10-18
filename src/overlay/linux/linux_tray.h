#pragma once

#if defined(_WIN32)
#include <functional>

namespace BareStats {

struct LinuxTrayCallbacks {
    std::function<void()> onToggleMove;
    std::function<void()> onCyclePreset;
    std::function<void()> onResetPosition;
    std::function<void()> onToggleVisibility;
    std::function<void()> onReloadConfig;
    std::function<void()> onExit;
};

class LinuxTray {
public:
    LinuxTray() = default;
    ~LinuxTray() = default;
    bool Initialize(const LinuxTrayCallbacks&) { return false; }
    void Poll() {}
    void Shutdown() {}
    bool IsActive() const { return false; }
};

} // namespace BareStats

#else

#include "barestats_icon_data.h"
#include <string>
#include <functional>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <dlfcn.h>
#include <unistd.h>

namespace BareStats {

struct LinuxTrayCallbacks {
    std::function<void()> onToggleMove;
    std::function<void()> onCyclePreset;
    std::function<void()> onResetPosition;
    std::function<void()> onToggleVisibility;
    std::function<void()> onReloadConfig;
    std::function<void()> onExit;
};

class LinuxTray {
public:
    LinuxTray() = default;
    ~LinuxTray() {
        Shutdown();
    }

    bool Initialize(const LinuxTrayCallbacks& callbacks) {
        if (isActive_) return true;
        callbacks_ = callbacks;

        // Strip $SNAP from environment if inherited from a host IDE/terminal container,
        // preventing libayatana-appindicator from prefixing icon paths with a nonexistent snap dir.
        unsetenv("SNAP");

        glibLib_ = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        gobjectLib_ = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
        gtkLib_ = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);

        ayatanaLib_ = dlopen("libayatana-appindicator3.so.1", RTLD_LAZY);
        if (!ayatanaLib_) {
            ayatanaLib_ = dlopen("libappindicator3.so.1", RTLD_LAZY);
        }

        if (!glibLib_ || !gobjectLib_ || !gtkLib_ || !ayatanaLib_) {
            CleanHandles();
            return false;
        }

        fnGtkInitCheck_ = reinterpret_cast<FnGtkInitCheck>(dlsym(gtkLib_, "gtk_init_check"));
        fnAppIndicatorNew_ = reinterpret_cast<FnAppIndicatorNew>(dlsym(ayatanaLib_, "app_indicator_new"));
        fnAppIndicatorSetStatus_ = reinterpret_cast<FnAppIndicatorSetStatus>(dlsym(ayatanaLib_, "app_indicator_set_status"));
        fnAppIndicatorSetMenu_ = reinterpret_cast<FnAppIndicatorSetMenu>(dlsym(ayatanaLib_, "app_indicator_set_menu"));
        fnAppIndicatorSetIconThemePath_ = reinterpret_cast<FnAppIndicatorSetIconThemePath>(dlsym(ayatanaLib_, "app_indicator_set_icon_theme_path"));

        fnGtkIconThemeGetDefault_ = reinterpret_cast<FnGtkIconThemeGetDefault>(dlsym(gtkLib_, "gtk_icon_theme_get_default"));
        fnGtkIconThemeAppendSearchPath_ = reinterpret_cast<FnGtkIconThemeAppendSearchPath>(dlsym(gtkLib_, "gtk_icon_theme_append_search_path"));

        fnGtkMenuNew_ = reinterpret_cast<FnGtkMenuNew>(dlsym(gtkLib_, "gtk_menu_new"));
        fnGtkMenuItemNewWithLabel_ = reinterpret_cast<FnGtkMenuItemNewWithLabel>(dlsym(gtkLib_, "gtk_menu_item_new_with_label"));
        fnGtkSeparatorMenuItemNew_ = reinterpret_cast<FnGtkSeparatorMenuItemNew>(dlsym(gtkLib_, "gtk_separator_menu_item_new"));
        fnGtkMenuShellAppend_ = reinterpret_cast<FnGtkMenuShellAppend>(dlsym(gtkLib_, "gtk_menu_shell_append"));
        fnGtkWidgetShowAll_ = reinterpret_cast<FnGtkWidgetShowAll>(dlsym(gtkLib_, "gtk_widget_show_all"));

        fnGSignalConnectData_ = reinterpret_cast<FnGSignalConnectData>(dlsym(gobjectLib_, "g_signal_connect_data"));
        fnGMainContextIteration_ = reinterpret_cast<FnGMainContextIteration>(dlsym(glibLib_, "g_main_context_iteration"));

        if (!fnGtkInitCheck_ || !fnAppIndicatorNew_ || !fnAppIndicatorSetStatus_ || !fnAppIndicatorSetMenu_ ||
            !fnGtkMenuNew_ || !fnGtkMenuItemNewWithLabel_ || !fnGtkSeparatorMenuItemNew_ ||
            !fnGtkMenuShellAppend_ || !fnGtkWidgetShowAll_ || !fnGSignalConnectData_ || !fnGMainContextIteration_) {
            CleanHandles();
            return false;
        }

        if (!fnGtkInitCheck_(nullptr, nullptr)) {
            CleanHandles();
            return false;
        }

        indicator_ = fnAppIndicatorNew_("barestats", "barestats", 0); // 0 = APP_INDICATOR_CATEGORY_APPLICATION_STATUS
        if (!indicator_) {
            CleanHandles();
            return false;
        }

        std::string iconThemePath = ResolveIconThemePath();
        if (fnAppIndicatorSetIconThemePath_ && !iconThemePath.empty()) {
            fnAppIndicatorSetIconThemePath_(indicator_, iconThemePath.c_str());
        }
        if (fnGtkIconThemeGetDefault_ && fnGtkIconThemeAppendSearchPath_ && !iconThemePath.empty()) {
            void* defTheme = fnGtkIconThemeGetDefault_();
            if (defTheme) {
                fnGtkIconThemeAppendSearchPath_(defTheme, iconThemePath.c_str());
            }
        }

        menu_ = fnGtkMenuNew_();
        if (!menu_) {
            CleanHandles();
            return false;
        }

        AddMenuItem("Reposition Mode (Drag Overlay)", &callbacks_.onToggleMove);
        AddMenuItem("Cycle Position (Next Corner)", &callbacks_.onCyclePreset);
        AddMenuItem("Reset to Bottom-Center", &callbacks_.onResetPosition);
        AddMenuItem("Toggle Overlay (Hide/Show)", &callbacks_.onToggleVisibility);
        AddMenuItem("Reload Configuration", &callbacks_.onReloadConfig);
        AddSeparator();
        AddMenuItem("Exit BareStats", &callbacks_.onExit);

        fnGtkWidgetShowAll_(menu_);
        fnAppIndicatorSetMenu_(indicator_, menu_);
        fnAppIndicatorSetStatus_(indicator_, 1); // 1 = APP_INDICATOR_STATUS_ACTIVE

        isActive_ = true;
        return true;
    }

    void Poll() {
        if (!isActive_ || !fnGMainContextIteration_) return;
        // Non-blocking drain of pending GTK / DBus menu events
        while (fnGMainContextIteration_(nullptr, 0)) {}
    }

    void Shutdown() {
        if (isActive_ && fnAppIndicatorSetStatus_ && indicator_) {
            fnAppIndicatorSetStatus_(indicator_, 0); // 0 = PASSIVE (hidden)
        }
        indicator_ = nullptr;
        menu_ = nullptr;
        isActive_ = false;
        CleanHandles();
    }

    bool IsActive() const { return isActive_; }

private:
    typedef int (*FnGtkInitCheck)(int*, char***);
    typedef void* (*FnAppIndicatorNew)(const char*, const char*, int);
    typedef void (*FnAppIndicatorSetStatus)(void*, int);
    typedef void (*FnAppIndicatorSetMenu)(void*, void*);
    typedef void (*FnAppIndicatorSetIconThemePath)(void*, const char*);
    typedef void* (*FnGtkIconThemeGetDefault)();
    typedef void (*FnGtkIconThemeAppendSearchPath)(void*, const char*);
    typedef void* (*FnGtkMenuNew)();
    typedef void* (*FnGtkMenuItemNewWithLabel)(const char*);
    typedef void* (*FnGtkSeparatorMenuItemNew)();
    typedef void (*FnGtkMenuShellAppend)(void*, void*);
    typedef void (*FnGtkWidgetShowAll)(void*);
    typedef unsigned long (*FnGSignalConnectData)(void*, const char*, void*, void*, void*, int);
    typedef int (*FnGMainContextIteration)(void*, int);

    std::string ResolveIconThemePath() {
        // 1. Check system icon theme path (/usr/share/icons)
        // Freedesktop / GNOME St.IconTheme expects the root directory containing the theme,
        // so /usr/share/icons allows resolving /usr/share/icons/hicolor/scalable/apps/barestats.svg
        // and /usr/share/icons/hicolor/256x256/apps/barestats.png.
        if (access("/usr/share/icons/hicolor/scalable/apps/barestats.svg", R_OK) == 0 ||
            access("/usr/share/icons/hicolor/256x256/apps/barestats.png", R_OK) == 0) {
            return "/usr/share/icons";
        }

        // 2. Check /usr/share/pixmaps (direct barestats.png)
        if (access("/usr/share/pixmaps/barestats.png", R_OK) == 0) {
            return "/usr/share/pixmaps";
        }

        // 3. Check AppImage runtime mount ($APPDIR)
        const char* appdir = std::getenv("APPDIR");
        if (appdir && appdir[0] != '\0') {
            std::string appDirIcons = std::string(appdir) + "/usr/share/icons";
            if (access(appDirIcons.c_str(), R_OK) == 0) {
                return appDirIcons;
            }
            std::string appDirRoot = std::string(appdir);
            std::string appDirPng = appDirRoot + "/barestats.png";
            if (access(appDirPng.c_str(), R_OK) == 0) {
                return appDirRoot;
            }
        }

        // 4. Check executable directory (portable mode)
        char exePath[4096] = { 0 };
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            std::string path(exePath);
            size_t lastSlash = path.find_last_of('/');
            if (lastSlash != std::string::npos) {
                std::string exeDir = path.substr(0, lastSlash);
                std::string localPng = exeDir + "/barestats.png";
                if (access(localPng.c_str(), R_OK) == 0) {
                    return exeDir;
                }
                std::string installedFromExe = exeDir + "/../share/icons";
                if (access(installedFromExe.c_str(), R_OK) == 0) {
                    return installedFromExe;
                }
            }
        }

        // 5. Bulletproof Fallback: Write embedded 1.5 KB barestats.png to user cache
        std::string cacheDir;
        const char* xdgCache = std::getenv("XDG_CACHE_HOME");
        if (xdgCache && xdgCache[0] != '\0') {
            cacheDir = std::string(xdgCache) + "/barestats/icons";
        } else {
            const char* home = std::getenv("HOME");
            if (home && home[0] != '\0') {
                cacheDir = std::string(home) + "/.cache/barestats/icons";
            } else {
                cacheDir = "/tmp/barestats_icons";
            }
        }

        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        std::string fallbackIconPath = cacheDir + "/barestats.png";
        if (access(fallbackIconPath.c_str(), R_OK) != 0) {
            std::ofstream out(fallbackIconPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(BARESTATS_EMBEDDED_PNG),
                          static_cast<std::streamsize>(BARESTATS_EMBEDDED_PNG_LEN));
            }
        }
        return cacheDir;
    }

    static void OnMenuActivated(void* /*item*/, void* userData) {
        auto* fn = static_cast<std::function<void()>*>(userData);
        if (fn && *fn) {
            (*fn)();
        }
    }

    void AddMenuItem(const char* label, std::function<void()>* cb) {
        if (!fnGtkMenuItemNewWithLabel_ || !fnGSignalConnectData_ || !fnGtkMenuShellAppend_ || !menu_) return;
        void* item = fnGtkMenuItemNewWithLabel_(label);
        if (item) {
            fnGSignalConnectData_(item, "activate", reinterpret_cast<void*>(OnMenuActivated), cb, nullptr, 0);
            fnGtkMenuShellAppend_(menu_, item);
        }
    }

    void AddSeparator() {
        if (!fnGtkSeparatorMenuItemNew_ || !fnGtkMenuShellAppend_ || !menu_) return;
        void* sep = fnGtkSeparatorMenuItemNew_();
        if (sep) {
            fnGtkMenuShellAppend_(menu_, sep);
        }
    }

    void CleanHandles() {
        if (ayatanaLib_) { dlclose(ayatanaLib_); ayatanaLib_ = nullptr; }
        if (gtkLib_) { dlclose(gtkLib_); gtkLib_ = nullptr; }
        if (gobjectLib_) { dlclose(gobjectLib_); gobjectLib_ = nullptr; }
        if (glibLib_) { dlclose(glibLib_); glibLib_ = nullptr; }
    }

    bool isActive_ = false;
    LinuxTrayCallbacks callbacks_;

    void* glibLib_ = nullptr;
    void* gobjectLib_ = nullptr;
    void* gtkLib_ = nullptr;
    void* ayatanaLib_ = nullptr;
    void* indicator_ = nullptr;
    void* menu_ = nullptr;

    FnGtkInitCheck fnGtkInitCheck_ = nullptr;
    FnAppIndicatorNew fnAppIndicatorNew_ = nullptr;
    FnAppIndicatorSetStatus fnAppIndicatorSetStatus_ = nullptr;
    FnAppIndicatorSetMenu fnAppIndicatorSetMenu_ = nullptr;
    FnAppIndicatorSetIconThemePath fnAppIndicatorSetIconThemePath_ = nullptr;
    FnGtkIconThemeGetDefault fnGtkIconThemeGetDefault_ = nullptr;
    FnGtkIconThemeAppendSearchPath fnGtkIconThemeAppendSearchPath_ = nullptr;
    FnGtkMenuNew fnGtkMenuNew_ = nullptr;
    FnGtkMenuItemNewWithLabel fnGtkMenuItemNewWithLabel_ = nullptr;
    FnGtkSeparatorMenuItemNew fnGtkSeparatorMenuItemNew_ = nullptr;
    FnGtkMenuShellAppend fnGtkMenuShellAppend_ = nullptr;
    FnGtkWidgetShowAll fnGtkWidgetShowAll_ = nullptr;
    FnGSignalConnectData fnGSignalConnectData_ = nullptr;
    FnGMainContextIteration fnGMainContextIteration_ = nullptr;
};

} // namespace BareStats

#endif // !_WIN32
