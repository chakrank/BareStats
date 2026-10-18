#define _CRT_SECURE_NO_WARNINGS
#include "linux_overlay.h"
#include "linux_tray.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(BARESTATS_HAS_CAIRO) && !defined(_WIN32)
#include <cairo/cairo.h>
#endif

#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <X11/cursorfont.h>
#include <cairo/cairo-xlib.h>

#ifdef None
#undef None
#endif
#endif

#if defined(BARESTATS_HAS_WAYLAND) && !defined(_WIN32)
#include <wayland-client.h>
#endif

namespace BareStats {

// Sub-microsecond 64-bit FNV-1a Hash Combination for Frame Dirty Check
static inline size_t CombineHash(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

bool LinuxOverlay::ComputeDirtyHash(const std::vector<std::vector<FormattedSegment>>& rows,
                                    size_t& inOutLastHash) {
    size_t hash = 14695981039346656037ULL;
    std::hash<std::wstring> strHasher;

    for (const auto& row : rows) {
        for (const auto& seg : row) {
            hash = CombineHash(hash, strHasher(seg.text));
            hash = CombineHash(hash, static_cast<size_t>(seg.severity));
            hash = CombineHash(hash, static_cast<size_t>(seg.type));
            hash = CombineHash(hash, seg.isBold ? 1 : 0);
            hash = CombineHash(hash, seg.isSeparator ? 2 : 0);
        }
        hash = CombineHash(hash, 0xAA55AA55); // Row separator marker
    }

    if (hash == inOutLastHash) {
        return false; // Dirty-frame hash bypass triggered: frame is identical, 0.00% duty cycle
    }

    inOutLastHash = hash;
    return true; // Frame changed, redraw required
}

std::vector<LinuxIslandSlot> LinuxOverlay::LayoutIslands(const AppConfig& config,
                                                        const SystemMetrics& metrics,
                                                        const MonitorInfo& monitor,
                                                        int slotPaddingX,
                                                        int slotPaddingY) {
    std::vector<LinuxIslandSlot> slots;
    if (config.customGrid.rows.empty()) return slots;

    const int charWidth = 8;
    const int baseHeight = 18 + slotPaddingY * 2;

    int numRows = static_cast<int>(config.customGrid.rows.size());
    int numCols = 0;
    for (const auto& rDef : config.customGrid.rows) {
        if (static_cast<int>(rDef.cells.size()) > numCols) {
            numCols = static_cast<int>(rDef.cells.size());
        }
    }
    if (numCols <= 0) return slots;

    // Measure cell widths per row/col
    std::vector<std::vector<int>> cellWidths(numRows, std::vector<int>(numCols, 0));
    std::vector<std::vector<std::wstring>> cellTexts(numRows, std::vector<std::wstring>(numCols));

    for (int r = 0; r < numRows; ++r) {
        const auto& rDef = config.customGrid.rows[r];
        for (size_t c = 0; c < rDef.cells.size(); ++c) {
            const auto& cell = rDef.cells[c];
            if (cell.token.empty() || cell.token == ".") continue;

            auto segs = Formatter::FormatCell(cell.token, metrics, &config);
            int cellLen = 0;
            std::wstring combined;
            for (const auto& s : segs) {
                cellLen += static_cast<int>(s.text.size());
                combined += s.text;
            }
            cellWidths[r][c] = (cellLen * charWidth) + (slotPaddingX * 2);
            cellTexts[r][c] = std::move(combined);
        }
    }

    // Determine max column width for each column
    std::vector<int> colWidth(numCols, 0);
    for (int c = 0; c < numCols; ++c) {
        int maxW = 0;
        for (int r = 0; r < numRows; ++r) {
            if (cellWidths[r][c] > maxW) maxW = cellWidths[r][c];
        }
        colWidth[c] = (maxW > 0) ? maxW : 90;
    }

    // Determine column X offsets relative to 0
    std::vector<int> colX(numCols, 0);
    colX[0] = 0;
    for (int c = 1; c < numCols; ++c) {
        colX[c] = colX[c - 1] + colWidth[c - 1] + config.cellGap;
    }

    int totalStatsWidth = (numCols > 0) ? (colX.back() + colWidth.back()) : 0;
    int totalStatsHeight = numRows * baseHeight + (numRows > 1 ? (numRows - 1) * config.rowGap : 0);

    Point origin = MonitorHelper::CalculatePosition(monitor, config.preset, totalStatsWidth, totalStatsHeight, config.margin, config.margin);
    if (config.preset == PositionPreset::Custom || config.customX >= 0) {
        if (config.customX >= 0) origin.x = config.customX;
        if (config.customY >= 0) origin.y = config.customY;
    }

    for (int r = 0; r < numRows; ++r) {
        const auto& rDef = config.customGrid.rows[r];
        int slotY = origin.y + r * (baseHeight + config.rowGap);
        for (size_t c = 0; c < rDef.cells.size(); ++c) {
            const auto& cell = rDef.cells[c];
            if (cell.token.empty() || cell.token == ".") continue;

            int cellW = cellWidths[r][c];
            if (cellW <= 0) continue;

            LinuxIslandSlot slot;
            slot.rowIndex = r;
            slot.colIndex = static_cast<int>(c);
            slot.token = cell.token;
            slot.lastText = cellTexts[r][c];
            slot.relX = origin.x + colX[c];
            slot.relY = slotY;
            slot.width = cellW;
            slot.height = baseHeight;

            slots.push_back(slot);
        }
    }

    return slots;
}

#if defined(BARESTATS_HAS_CAIRO) && !defined(_WIN32)

static void DrawRoundedRectangle(cairo_t* cr, double x, double y, double width, double height, double radius) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, x + radius, y + height - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static inline void ColorRefToRgba(uint32_t color, double alpha, double& r, double& g, double& b, double& a) {
    r = (color & 0xFF) / 255.0;
    g = ((color >> 8) & 0xFF) / 255.0;
    b = ((color >> 16) & 0xFF) / 255.0;
    a = alpha;
}

static std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    std::string out;
    for (wchar_t wc : wstr) {
        if (wc <= 0x7F) {
            out.push_back(static_cast<char>(wc));
        } else if (wc <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((wc >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | ((wc >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    return out;
}

static cairo_font_weight_t ResolveCairoFontWeight(const std::wstring& wweight) {
    std::string w = WStringToUtf8(wweight);
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "regular" || w == "normal") {
        return CAIRO_FONT_WEIGHT_NORMAL;
    }
    return CAIRO_FONT_WEIGHT_BOLD;
}

static std::string ResolveFontFaceName(const std::wstring& wface) {
    std::string fontFaceName = "DejaVu Sans";
    if (!wface.empty()) {
        std::string utf8Face = WStringToUtf8(wface);
        std::string lowerFace = utf8Face;
        for (auto& c : lowerFace) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerFace != "auto" && lowerFace != "system" && !utf8Face.empty()) {
            fontFaceName = utf8Face;
        }
    }
    return fontFaceName;
}

#endif // BARESTATS_HAS_CAIRO

#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
static int X11IgnoreErrorHandler(Display*, XErrorEvent*) {
    return 0;
}
#endif

struct LinuxOverlay::Impl {
    AppConfig& config;
    LinuxOverlayBackendType backendType = LinuxOverlayBackendType::None;
    size_t lastFrameHash = 0;
    SystemMetrics lastMetrics{};
    std::vector<LinuxIslandSlot> islandSlots;
    bool isVisible = true;
    bool isPositioningMode = false;
    bool isDragging = false;
    bool isPointerGrabbed = false;
    int dragOffsetX = 0;
    int dragOffsetY = 0;

    bool isAnimatingRollUp = false;
    int animTicksRemaining = 0;
    const int totalAnimTicks = 25; // 25 ticks * 16ms = ~400ms duration
    SystemMetrics animTargetMetrics{};

    static SystemMetrics InterpolateMetrics(const SystemMetrics& target, float progress) {
        SystemMetrics cur = target;
        cur.cpu.utilizationPercent = target.cpu.utilizationPercent * progress;
        if (target.cpu.temperatureC.has_value()) {
            cur.cpu.temperatureC = *target.cpu.temperatureC * progress;
        }
        if (target.cpu.powerWatts.has_value()) {
            cur.cpu.powerWatts = *target.cpu.powerWatts * progress;
        }
        if (target.cpu.fanRpm.has_value()) {
            cur.cpu.fanRpm = static_cast<uint32_t>(*target.cpu.fanRpm * progress);
        }

        cur.memory.usedBytes = static_cast<uint64_t>(target.memory.usedBytes * progress);
        cur.memory.totalBytes = target.memory.totalBytes;

        if (target.gpu.utilizationPercent.has_value()) {
            cur.gpu.utilizationPercent = *target.gpu.utilizationPercent * progress;
        }
        cur.gpu.memoryUsedBytes = static_cast<uint64_t>(target.gpu.memoryUsedBytes * progress);
        cur.gpu.memoryTotalBytes = target.gpu.memoryTotalBytes;
        if (target.gpu.temperatureC.has_value()) {
            cur.gpu.temperatureC = static_cast<uint32_t>(*target.gpu.temperatureC * progress);
        }
        if (target.gpu.powerWatts.has_value()) {
            cur.gpu.powerWatts = static_cast<uint32_t>(*target.gpu.powerWatts * progress);
        }
        if (target.gpu.fanRpm.has_value()) {
            cur.gpu.fanRpm = static_cast<uint32_t>(*target.gpu.fanRpm * progress);
        } else if (target.gpu.fanSpeedPercent.has_value()) {
            cur.gpu.fanSpeedPercent = static_cast<uint32_t>(*target.gpu.fanSpeedPercent * progress);
        }

        cur.gpus.resize(target.gpus.size());
        for (size_t gi = 0; gi < target.gpus.size(); ++gi) {
            const auto& tg = target.gpus[gi];
            auto& g = cur.gpus[gi];
            g = tg;
            if (tg.utilizationPercent.has_value()) g.utilizationPercent = *tg.utilizationPercent * progress;
            g.memoryUsedBytes = static_cast<uint64_t>(tg.memoryUsedBytes * progress);
            g.memoryTotalBytes = tg.memoryTotalBytes;
            if (tg.temperatureC.has_value()) g.temperatureC = static_cast<uint32_t>(*tg.temperatureC * progress);
            if (tg.powerWatts.has_value()) g.powerWatts = static_cast<uint32_t>(*tg.powerWatts * progress);
            if (tg.fanRpm.has_value()) g.fanRpm = static_cast<uint32_t>(*tg.fanRpm * progress);
            if (tg.fanSpeedPercent.has_value()) g.fanSpeedPercent = static_cast<uint32_t>(*tg.fanSpeedPercent * progress);
        }

        cur.network.rxBytesPerSec = static_cast<uint64_t>(target.network.rxBytesPerSec * progress);
        cur.network.txBytesPerSec = static_cast<uint64_t>(target.network.txBytesPerSec * progress);
        return cur;
    }

    int currentWidth = 400;
    int currentHeight = 36;
    int currentX = 0;
    int currentY = 0;

    // Platform handles
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
    Display* x11Display = nullptr;
    Window x11Window = 0;
    cairo_surface_t* x11Surface = nullptr;
    cairo_t* x11Cr = nullptr;
    Cursor fleurCursor = 0;
    KeyCode keyM = 0;
    KeyCode keyP = 0;
    KeyCode keyH = 0;
    KeyCode keyEscape = 0;

    struct LinuxIslandWindow {
        Window window = 0;
        cairo_surface_t* surface = nullptr;
        cairo_t* cr = nullptr;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        std::string token;
    };

    std::vector<LinuxIslandWindow> islandWindows;
    Visual* x11Visual = nullptr;
    int x11Depth = 0;
    Colormap x11Colormap = 0;
#endif

    explicit Impl(AppConfig& cfg) : config(cfg) {}

    ~Impl() {
        CleanupPlatform();
    }

    void DestroyIslandWindows() {
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (x11Display) {
            for (auto& iw : islandWindows) {
                if (iw.cr) { cairo_destroy(iw.cr); iw.cr = nullptr; }
                if (iw.surface) { cairo_surface_destroy(iw.surface); iw.surface = nullptr; }
                if (iw.window) { XDestroyWindow(x11Display, iw.window); iw.window = 0; }
            }
        }
        islandWindows.clear();
#endif
    }

    void CleanupPlatform() {
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (x11Display && isPointerGrabbed) {
            XUngrabPointer(x11Display, CurrentTime);
            XUngrabKeyboard(x11Display, CurrentTime);
            isPointerGrabbed = false;
        }
        DestroyIslandWindows();
        if (x11Cr) { cairo_destroy(x11Cr); x11Cr = nullptr; }
        if (x11Surface) { cairo_surface_destroy(x11Surface); x11Surface = nullptr; }
        if (x11Display) {
            unsigned int ctrlAltMods[] = {
                ControlMask | Mod1Mask,
                ControlMask | Mod1Mask | Mod2Mask,
                ControlMask | Mod1Mask | LockMask,
                ControlMask | Mod1Mask | Mod2Mask | LockMask
            };
            unsigned int ctrlShiftMods[] = {
                ControlMask | ShiftMask,
                ControlMask | ShiftMask | Mod2Mask,
                ControlMask | ShiftMask | LockMask,
                ControlMask | ShiftMask | Mod2Mask | LockMask
            };

            if (keyM != 0) {
                for (unsigned int mod : ctrlAltMods) XUngrabKey(x11Display, keyM, mod, DefaultRootWindow(x11Display));
                for (unsigned int mod : ctrlShiftMods) XUngrabKey(x11Display, keyM, mod, DefaultRootWindow(x11Display));
            }
            if (keyP != 0) {
                for (unsigned int mod : ctrlAltMods) XUngrabKey(x11Display, keyP, mod, DefaultRootWindow(x11Display));
            }
            if (keyH != 0) {
                for (unsigned int mod : ctrlAltMods) XUngrabKey(x11Display, keyH, mod, DefaultRootWindow(x11Display));
                for (unsigned int mod : ctrlShiftMods) XUngrabKey(x11Display, keyH, mod, DefaultRootWindow(x11Display));
            }

            if (fleurCursor != 0) {
                XFreeCursor(x11Display, fleurCursor);
                fleurCursor = 0;
            }

            if (x11Window) { XDestroyWindow(x11Display, x11Window); x11Window = 0; }
            XCloseDisplay(x11Display);
            x11Display = nullptr;
        }
#endif
    }

    bool Initialize(LinuxOverlayBackendType& outType) {
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        // 1. Prioritize X11 / XWayland when DISPLAY is present
        const char* x11DisplayStr = std::getenv("DISPLAY");
        if (x11DisplayStr && std::strlen(x11DisplayStr) > 0) {
            x11Display = XOpenDisplay(nullptr);
            if (x11Display) {
                XSetErrorHandler(X11IgnoreErrorHandler);

                int screen = DefaultScreen(x11Display);
                int screenW = DisplayWidth(x11Display, screen);
                int screenH = DisplayHeight(x11Display, screen);

                // 32-bit TrueColor Visual for ARGB alpha transparency
                XVisualInfo vinfo;
                bool hasArgb = XMatchVisualInfo(x11Display, screen, 32, TrueColor, &vinfo);
                Visual* visual = hasArgb ? vinfo.visual : DefaultVisual(x11Display, screen);
                int depth = hasArgb ? vinfo.depth : DefaultDepth(x11Display, screen);
                Colormap colormap = hasArgb ? XCreateColormap(x11Display, RootWindow(x11Display, screen), visual, AllocNone)
                                            : DefaultColormap(x11Display, screen);

                x11Visual = visual;
                x11Depth = depth;
                x11Colormap = colormap;

                XSetWindowAttributes attr{};
                attr.colormap = colormap;
                attr.border_pixel = 0;
                attr.background_pixel = 0;
                attr.override_redirect = True; // Bypass window manager decoration

                currentWidth = 400;
                currentHeight = 36;

                MonitorInfo mon;
                mon.index = 0;
                mon.isPrimary = true;
                mon.rcMonitor = { 0, 0, screenW, screenH };
                mon.rcWork = { 0, 0, screenW, screenH };
                Point pt = MonitorHelper::CalculatePosition(mon, config.preset, currentWidth, currentHeight, config.margin, config.margin);
                currentX = (config.customX >= 0) ? config.customX : pt.x;
                currentY = (config.customY >= 0) ? config.customY : pt.y;

                unsigned long valueMask = CWBorderPixel | CWBackPixel | CWOverrideRedirect;
                if (hasArgb) valueMask |= CWColormap;

                x11Window = XCreateWindow(x11Display, RootWindow(x11Display, screen),
                                          currentX, currentY, currentWidth, currentHeight, 0,
                                          depth, InputOutput, visual,
                                          valueMask, &attr);

                // Set Window Properties
                XStoreName(x11Display, x11Window, "BareStats");
                XClassHint classHint;
                char resName[] = "barestats";
                char resClass[] = "BareStats";
                classHint.res_name = resName;
                classHint.res_class = resClass;
                XSetClassHint(x11Display, x11Window, &classHint);

                Atom netWmWindowType = XInternAtom(x11Display, "_NET_WM_WINDOW_TYPE", False);
                Atom netWmTypeDock = XInternAtom(x11Display, "_NET_WM_WINDOW_TYPE_DOCK", False);
                XChangeProperty(x11Display, x11Window, netWmWindowType, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(&netWmTypeDock), 1);

                Atom netWmState = XInternAtom(x11Display, "_NET_WM_STATE", False);
                Atom states[2] = {
                    XInternAtom(x11Display, "_NET_WM_STATE_ABOVE", False),
                    XInternAtom(x11Display, "_NET_WM_STATE_STAYS_ON_TOP", False)
                };
                XChangeProperty(x11Display, x11Window, netWmState, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(states), 2);

                pid_t curPid = getpid();
                Atom netWmPid = XInternAtom(x11Display, "_NET_WM_PID", False);
                XChangeProperty(x11Display, x11Window, netWmPid, XA_CARDINAL, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(&curPid), 1);

                // Click-through input transparent mask via XFixes extension by default
                XserverRegion region = XFixesCreateRegion(x11Display, nullptr, 0);
                XFixesSetWindowShapeRegion(x11Display, x11Window, ShapeInput, 0, 0, region);
                XFixesDestroyRegion(x11Display, region);

                // Resolve Hotkeys
                keyM = XKeysymToKeycode(x11Display, XStringToKeysym("m"));
                if (keyM == 0) keyM = XKeysymToKeycode(x11Display, XStringToKeysym("M"));

                keyP = XKeysymToKeycode(x11Display, XStringToKeysym("p"));
                if (keyP == 0) keyP = XKeysymToKeycode(x11Display, XStringToKeysym("P"));

                keyH = XKeysymToKeycode(x11Display, XStringToKeysym("h"));
                if (keyH == 0) keyH = XKeysymToKeycode(x11Display, XStringToKeysym("H"));

                keyEscape = XKeysymToKeycode(x11Display, XStringToKeysym("Escape"));

                unsigned int ctrlAltMods[] = {
                    ControlMask | Mod1Mask,
                    ControlMask | Mod1Mask | Mod2Mask,
                    ControlMask | Mod1Mask | LockMask,
                    ControlMask | Mod1Mask | Mod2Mask | LockMask
                };
                unsigned int ctrlShiftMods[] = {
                    ControlMask | ShiftMask,
                    ControlMask | ShiftMask | Mod2Mask,
                    ControlMask | ShiftMask | LockMask,
                    ControlMask | ShiftMask | Mod2Mask | LockMask
                };

                // 1. Move Mode: Ctrl + Alt + M and Ctrl + Shift + M
                if (keyM != 0) {
                    for (unsigned int mod : ctrlAltMods) {
                        XGrabKey(x11Display, keyM, mod, RootWindow(x11Display, screen), False, GrabModeAsync, GrabModeAsync);
                    }
                    for (unsigned int mod : ctrlShiftMods) {
                        XGrabKey(x11Display, keyM, mod, RootWindow(x11Display, screen), False, GrabModeAsync, GrabModeAsync);
                    }
                }

                // 2. Cycle Preset: Ctrl + Alt + P
                if (keyP != 0) {
                    for (unsigned int mod : ctrlAltMods) {
                        XGrabKey(x11Display, keyP, mod, RootWindow(x11Display, screen), False, GrabModeAsync, GrabModeAsync);
                    }
                }

                // 3. Toggle Visibility: Ctrl + Alt + H and Ctrl + Shift + H
                if (keyH != 0) {
                    for (unsigned int mod : ctrlAltMods) {
                        XGrabKey(x11Display, keyH, mod, RootWindow(x11Display, screen), False, GrabModeAsync, GrabModeAsync);
                    }
                    for (unsigned int mod : ctrlShiftMods) {
                        XGrabKey(x11Display, keyH, mod, RootWindow(x11Display, screen), False, GrabModeAsync, GrabModeAsync);
                    }
                }

                // Cairo Xlib surface
                x11Surface = cairo_xlib_surface_create(x11Display, x11Window, visual, currentWidth, currentHeight);
                x11Cr = cairo_create(x11Surface);

                XMapRaised(x11Display, x11Window);
                XFlush(x11Display);

                backendType = LinuxOverlayBackendType::X11;
                outType = backendType;
                return true;
            }
        }
#endif

        // 2. Wayland Fallback Check
        const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
        if (waylandDisplay && std::strlen(waylandDisplay) > 0) {
            backendType = LinuxOverlayBackendType::Wayland;
            outType = backendType;
            return true;
        }

        // 3. Headless Fallback (CI, containers, or terminal session)
        backendType = LinuxOverlayBackendType::Headless;
        outType = backendType;
        return true;
    }

    void SetVisible(bool visible) {
        isVisible = visible;
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (x11Display) {
            if (config.layout == OverlayLayout::CustomGrid && config.gridMode == GridMode::Islands) {
                for (auto& iw : islandWindows) {
                    if (visible) {
                        XMapRaised(x11Display, iw.window);
                    } else {
                        XUnmapWindow(x11Display, iw.window);
                    }
                }
                if (x11Window) {
                    if (visible && isPositioningMode) {
                        XMapRaised(x11Display, x11Window);
                    } else {
                        XUnmapWindow(x11Display, x11Window);
                    }
                }
            } else if (x11Window) {
                if (visible) {
                    XMapRaised(x11Display, x11Window);
                } else {
                    XUnmapWindow(x11Display, x11Window);
                }
            }
            XFlush(x11Display);
        }
#endif
    }

    void SetPositioningMode(bool enable) {
        if (isPositioningMode == enable) return;
        isPositioningMode = enable;
        isDragging = false;

#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (x11Display) {
            if (isPositioningMode) {
                if (fleurCursor == 0) {
                    fleurCursor = XCreateFontCursor(x11Display, XC_fleur);
                }

                // Main window
                if (x11Window) {
                    XFixesSetWindowShapeRegion(x11Display, x11Window, ShapeInput, 0, 0, 0);
                    if (fleurCursor != 0) XDefineCursor(x11Display, x11Window, fleurCursor);
                    XSelectInput(x11Display, x11Window,
                                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                 Button1MotionMask | KeyPressMask);
                }

                // Island windows
                for (auto& iw : islandWindows) {
                    XFixesSetWindowShapeRegion(x11Display, iw.window, ShapeInput, 0, 0, 0);
                    if (fleurCursor != 0) XDefineCursor(x11Display, iw.window, fleurCursor);
                    XSelectInput(x11Display, iw.window,
                                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                 Button1MotionMask | KeyPressMask);
                }

                int screen = DefaultScreen(x11Display);
                Window rootWin = RootWindow(x11Display, screen);
                int grabRes = XGrabPointer(x11Display, rootWin, False,
                                           ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                           GrabModeAsync, GrabModeAsync,
                                           0, fleurCursor, CurrentTime);
                if (grabRes == GrabSuccess) {
                    isPointerGrabbed = true;
                }
                XGrabKeyboard(x11Display, rootWin, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            } else {
                if (isPointerGrabbed) {
                    XUngrabPointer(x11Display, CurrentTime);
                    isPointerGrabbed = false;
                }
                XUngrabKeyboard(x11Display, CurrentTime);

                if (fleurCursor != 0) {
                    if (x11Window) XUndefineCursor(x11Display, x11Window);
                    for (auto& iw : islandWindows) {
                        XUndefineCursor(x11Display, iw.window);
                    }
                }

                // Restore click-through transparent input mask on main window
                if (x11Window) {
                    XserverRegion region = XFixesCreateRegion(x11Display, nullptr, 0);
                    XFixesSetWindowShapeRegion(x11Display, x11Window, ShapeInput, 0, 0, region);
                    XFixesDestroyRegion(x11Display, region);
                    XSelectInput(x11Display, x11Window, ExposureMask);
                }

                // Restore click-through transparent input mask on island windows
                for (auto& iw : islandWindows) {
                    XserverRegion ir = XFixesCreateRegion(x11Display, nullptr, 0);
                    XFixesSetWindowShapeRegion(x11Display, iw.window, ShapeInput, 0, 0, ir);
                    XFixesDestroyRegion(x11Display, ir);
                    XSelectInput(x11Display, iw.window, ExposureMask);
                }
            }
            XFlush(x11Display);
        }
#endif
        ForceRepaint();
    }

    void PaintIslands(const SystemMetrics& metrics) {
        (void)metrics;
#if defined(BARESTATS_HAS_CAIRO) && defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (backendType != LinuxOverlayBackendType::X11 || !x11Display) return;

        if (!isVisible) {
            for (auto& iw : islandWindows) {
                XUnmapWindow(x11Display, iw.window);
            }
            if (x11Window) XUnmapWindow(x11Display, x11Window);
            return;
        }

        MonitorInfo mon = MonitorHelper::GetMonitor(config.monitorIndex);
        islandSlots = LinuxOverlay::LayoutIslands(config, metrics, mon);

        if (islandSlots.empty()) {
            DestroyIslandWindows();
            return;
        }

        int screen = DefaultScreen(x11Display);

        // Synchronize window instances
        if (islandWindows.size() != islandSlots.size()) {
            DestroyIslandWindows();
            islandWindows.resize(islandSlots.size());

            for (size_t i = 0; i < islandSlots.size(); ++i) {
                const auto& slot = islandSlots[i];
                XSetWindowAttributes attr{};
                attr.colormap = x11Colormap;
                attr.border_pixel = 0;
                attr.background_pixel = 0;
                attr.override_redirect = True;

                unsigned long valueMask = CWBorderPixel | CWBackPixel | CWOverrideRedirect;
                if (x11Visual != DefaultVisual(x11Display, screen)) valueMask |= CWColormap;

                int initW = (slot.width > 0) ? slot.width : 100;
                int initH = (slot.height > 0) ? slot.height : 26;

                Window win = XCreateWindow(x11Display, RootWindow(x11Display, screen),
                                          slot.relX, slot.relY, initW, initH, 0,
                                          x11Depth, InputOutput, x11Visual,
                                          valueMask, &attr);

                XStoreName(x11Display, win, "BareStats Island");
                XClassHint classHint;
                char resName[] = "barestats";
                char resClass[] = "BareStats";
                classHint.res_name = resName;
                classHint.res_class = resClass;
                XSetClassHint(x11Display, win, &classHint);

                Atom netWmWindowType = XInternAtom(x11Display, "_NET_WM_WINDOW_TYPE", False);
                Atom netWmTypeDock = XInternAtom(x11Display, "_NET_WM_WINDOW_TYPE_DOCK", False);
                XChangeProperty(x11Display, win, netWmWindowType, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(&netWmTypeDock), 1);

                Atom netWmState = XInternAtom(x11Display, "_NET_WM_STATE", False);
                Atom states[2] = {
                    XInternAtom(x11Display, "_NET_WM_STATE_ABOVE", False),
                    XInternAtom(x11Display, "_NET_WM_STATE_STAYS_ON_TOP", False)
                };
                XChangeProperty(x11Display, win, netWmState, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(states), 2);

                pid_t curPid = getpid();
                Atom netWmPid = XInternAtom(x11Display, "_NET_WM_PID", False);
                XChangeProperty(x11Display, win, netWmPid, XA_CARDINAL, 32, PropModeReplace,
                                reinterpret_cast<unsigned char*>(&curPid), 1);

                if (!isPositioningMode) {
                    XserverRegion region = XFixesCreateRegion(x11Display, nullptr, 0);
                    XFixesSetWindowShapeRegion(x11Display, win, ShapeInput, 0, 0, region);
                    XFixesDestroyRegion(x11Display, region);
                    XSelectInput(x11Display, win, ExposureMask);
                } else {
                    XFixesSetWindowShapeRegion(x11Display, win, ShapeInput, 0, 0, 0);
                    XSelectInput(x11Display, win,
                                 ExposureMask | ButtonPressMask | ButtonReleaseMask |
                                 Button1MotionMask | KeyPressMask);
                    if (fleurCursor != 0) XDefineCursor(x11Display, win, fleurCursor);
                }

                cairo_surface_t* srf = cairo_xlib_surface_create(x11Display, win, x11Visual, initW, initH);
                cairo_t* cr = cairo_create(srf);

                XMapRaised(x11Display, win);

                LinuxIslandWindow& iw = islandWindows[i];
                iw.window = win;
                iw.surface = srf;
                iw.cr = cr;
                iw.x = slot.relX;
                iw.y = slot.relY;
                iw.width = initW;
                iw.height = initH;
                iw.token = slot.token;
            }
        }

        // Anchor window for Move Mode or hide in normal mode
        if (x11Window) {
            if (isPositioningMode) {
                int bannerW = 340;
                int bannerH = 26;
                int anchorX = currentX > 0 ? currentX : islandSlots[0].relX;
                int anchorY = currentY > 32 ? currentY - 32 : (std::max)(0, islandSlots[0].relY - 32);
                XMoveResizeWindow(x11Display, x11Window, anchorX, anchorY, bannerW, bannerH);
                cairo_xlib_surface_set_size(x11Surface, bannerW, bannerH);

                cairo_set_operator(x11Cr, CAIRO_OPERATOR_CLEAR);
                cairo_paint(x11Cr);
                cairo_set_operator(x11Cr, CAIRO_OPERATOR_OVER);

                cairo_set_source_rgba(x11Cr, 0.08, 0.12, 0.18, 0.95);
                DrawRoundedRectangle(x11Cr, 1.0, 1.0, bannerW - 2.0, bannerH - 2.0, 6.0);
                cairo_fill(x11Cr);

                cairo_set_source_rgba(x11Cr, 1.0, 0.84, 0.0, 0.95);
                cairo_set_line_width(x11Cr, 1.5);
                DrawRoundedRectangle(x11Cr, 1.0, 1.0, bannerW - 2.0, bannerH - 2.0, 6.0);
                cairo_stroke(x11Cr);

                cairo_select_font_face(x11Cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(x11Cr, 11);
                cairo_move_to(x11Cr, 12, 18);
                cairo_show_text(x11Cr, "MOVE MODE \xE2\x80\xA2 Drag anywhere \xE2\x80\xA2 Esc / Click to Exit");
                cairo_surface_flush(x11Surface);
                XMapRaised(x11Display, x11Window);
            } else {
                XUnmapWindow(x11Display, x11Window);
            }
        }

        int fontSize = (config.fontSize > 0) ? config.fontSize : 13;
        std::string fontFaceName = ResolveFontFaceName(config.fontFace);
        cairo_font_weight_t fontWeight = ResolveCairoFontWeight(config.fontWeight);

        int padX = 14;
        int padY = 6;
        double shadowOffset = 1.0;

        for (size_t i = 0; i < islandSlots.size(); ++i) {
            const auto& slot = islandSlots[i];
            if (i >= islandWindows.size()) break;
            auto& iw = islandWindows[i];
            if (!iw.cr || !iw.surface) continue;

            cairo_select_font_face(iw.cr, fontFaceName.c_str(), CAIRO_FONT_SLANT_NORMAL, fontWeight);
            cairo_set_font_size(iw.cr, fontSize);

            auto segs = Formatter::FormatCell(slot.token, metrics, &config);

            double textW = 0.0;
            for (const auto& s : segs) {
                if (s.text.empty()) continue;
                std::string utf8 = WStringToUtf8(s.text);
                cairo_text_extents_t ext;
                cairo_text_extents(iw.cr, utf8.c_str(), &ext);
                textW += ext.x_advance;
            }

            int neededW = static_cast<int>(std::ceil(textW)) + padX * 2;
            int neededH = fontSize + padY * 2 + 4;
            if (neededW < 60) neededW = 60;
            if (neededH < 26) neededH = 26;

            int targetX = slot.relX;
            int targetY = slot.relY;

            if (neededW != iw.width || neededH != iw.height || targetX != iw.x || targetY != iw.y) {
                iw.width = neededW;
                iw.height = neededH;
                iw.x = targetX;
                iw.y = targetY;
                XMoveResizeWindow(x11Display, iw.window, iw.x, iw.y, iw.width, iw.height);
                cairo_xlib_surface_set_size(iw.surface, iw.width, iw.height);

                if (!isPositioningMode) {
                    XserverRegion region = XFixesCreateRegion(x11Display, nullptr, 0);
                    XFixesSetWindowShapeRegion(x11Display, iw.window, ShapeInput, 0, 0, region);
                    XFixesDestroyRegion(x11Display, region);
                }
            }

            cairo_reset_clip(iw.cr);
            cairo_set_operator(iw.cr, CAIRO_OPERATOR_CLEAR);
            cairo_paint(iw.cr);
            cairo_set_operator(iw.cr, CAIRO_OPERATOR_OVER);

            double textAlpha = std::clamp(config.opacity / 100.0, 0.1, 1.0);
            if (isPositioningMode) {
                // Golden accent border and dark plate in Move Mode
                cairo_set_source_rgba(iw.cr, 0.08, 0.12, 0.18, 0.90);
                DrawRoundedRectangle(iw.cr, 1.0, 1.0, iw.width - 2.0, iw.height - 2.0, 6.0);
                cairo_fill(iw.cr);

                cairo_set_source_rgba(iw.cr, 1.0, 0.84, 0.0, 0.95);
                cairo_set_line_width(iw.cr, 2.0);
                DrawRoundedRectangle(iw.cr, 1.0, 1.0, iw.width - 2.0, iw.height - 2.0, 6.0);
                cairo_stroke(iw.cr);
            }

            double curX = padX;
            double curY = padY + fontSize;

            for (const auto& seg : segs) {
                if (seg.text.empty()) continue;
                std::string utf8 = WStringToUtf8(seg.text);
                cairo_text_extents_t ext;
                cairo_text_extents(iw.cr, utf8.c_str(), &ext);

                if (config.dropShadow) {
                    cairo_set_source_rgba(iw.cr, 0.0, 0.0, 0.0, 0.75 * textAlpha);
                    cairo_move_to(iw.cr, curX + shadowOffset, curY + shadowOffset);
                    cairo_show_text(iw.cr, utf8.c_str());
                }

                double red, green, blue, alpha;
                if (seg.type == SegmentType::Separator || seg.isSeparator) {
                    ColorRefToRgba(config.separatorColor, textAlpha, red, green, blue, alpha);
                } else if (seg.type == SegmentType::Label) {
                    ColorRefToRgba(config.labelColor, textAlpha, red, green, blue, alpha);
                } else if (seg.severity == MetricSeverity::Critical) {
                    ColorRefToRgba(config.colorCritical, textAlpha, red, green, blue, alpha);
                } else if (seg.severity == MetricSeverity::High) {
                    ColorRefToRgba(config.colorHigh, textAlpha, red, green, blue, alpha);
                } else {
                    ColorRefToRgba(config.textColor, textAlpha, red, green, blue, alpha);
                }

                cairo_set_source_rgba(iw.cr, red, green, blue, alpha);
                cairo_move_to(iw.cr, curX, curY);
                cairo_show_text(iw.cr, utf8.c_str());
                curX += ext.x_advance;
            }

            cairo_surface_flush(iw.surface);
        }
        XFlush(x11Display);
#endif
    }

    void RenderMetricsFrame(const SystemMetrics& m) {
        if (config.layout == OverlayLayout::CustomGrid && config.gridMode == GridMode::Islands) {
            PaintIslands(m);
            return;
        }

        DestroyIslandWindows();
        auto rows = Formatter::FormatRows(m, &config);
        PaintSurface(rows);
    }

    void TriggerLaunchAnimation(const SystemMetrics& target) {
        animTargetMetrics = target;
        lastMetrics = target;
        animTicksRemaining = totalAnimTicks;
        isAnimatingRollUp = true;

        SystemMetrics zeroMetrics = InterpolateMetrics(target, 0.0f);
        RenderMetricsFrame(zeroMetrics);
    }

    void UpdateMetrics(const SystemMetrics& metrics) {
        if (isAnimatingRollUp) {
            animTargetMetrics = metrics;
            return;
        }
        lastMetrics = metrics;

        if (config.layout == OverlayLayout::CustomGrid && config.gridMode == GridMode::Islands) {
            PaintIslands(metrics);
            return;
        }

        DestroyIslandWindows();

        auto rows = Formatter::FormatRows(metrics, &config);

        // Compute dirty frame hash to bypass rendering if metrics haven't changed and not in move mode
        if (!isPositioningMode) {
            bool dirty = LinuxOverlay::ComputeDirtyHash(rows, lastFrameHash);
            if (!dirty) {
                return; // Sub-microsecond bypass: true 0.00% idle duty cycle
            }
        }

        PaintSurface(rows);
    }

    void PaintSurface(const std::vector<std::vector<FormattedSegment>>& rows) {
#if defined(BARESTATS_HAS_CAIRO) && defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (backendType == LinuxOverlayBackendType::X11 && x11Cr && x11Surface && isVisible) {
            int fontSize = (config.fontSize > 0) ? config.fontSize : 13;
            std::string fontFaceName = ResolveFontFaceName(config.fontFace);
            cairo_font_weight_t fontWeight = ResolveCairoFontWeight(config.fontWeight);
            cairo_select_font_face(x11Cr, fontFaceName.c_str(), CAIRO_FONT_SLANT_NORMAL, fontWeight);
            cairo_set_font_size(x11Cr, fontSize);

            // 1. Measure text content across all rows
            double maxRowWidth = 0.0;
            for (const auto& row : rows) {
                double rowW = 0.0;
                for (const auto& seg : row) {
                    if (seg.text.empty()) continue;
                    std::string utf8 = WStringToUtf8(seg.text);
                    cairo_text_extents_t extents;
                    cairo_text_extents(x11Cr, utf8.c_str(), &extents);
                    rowW += extents.x_advance;
                }
                if (rowW > maxRowWidth) maxRowWidth = rowW;
            }

            if (isPositioningMode) {
                maxRowWidth += 115.0; // Space for [MOVE MODE] badge
            }

            int padX = 14;
            int padY = 6;
            int neededW = static_cast<int>(std::ceil(maxRowWidth)) + padX * 2;
            int rowCount = (std::max)(static_cast<int>(rows.size()), 1);
            double rowSpacing = 4.0;
            int neededH = static_cast<int>(rowCount * (fontSize + rowSpacing) + padY * 2);

            if (neededW < 100) neededW = 100;
            if (neededH < 26) neededH = 26;

            int screen = DefaultScreen(x11Display);
            int screenW = DisplayWidth(x11Display, screen);
            int screenH = DisplayHeight(x11Display, screen);

            MonitorInfo mon;
            mon.index = 0;
            mon.isPrimary = true;
            mon.rcMonitor = { 0, 0, screenW, screenH };
            mon.rcWork = { 0, 0, screenW, screenH };

            Point targetPt = MonitorHelper::CalculatePosition(mon, config.preset, neededW, neededH, config.margin, config.margin);
            int targetX = (config.customX >= 0) ? config.customX : targetPt.x;
            int targetY = (config.customY >= 0) ? config.customY : targetPt.y;

            // If in active drag, keep user's current dragged position
            if (isPositioningMode && (currentX != 0 || currentY != 0)) {
                targetX = currentX;
                targetY = currentY;
            }

            // Resize and move window if dimensions or position changed
            if (neededW != currentWidth || neededH != currentHeight || targetX != currentX || targetY != currentY) {
                currentWidth = neededW;
                currentHeight = neededH;
                currentX = targetX;
                currentY = targetY;
                XMoveResizeWindow(x11Display, x11Window, currentX, currentY, currentWidth, currentHeight);
                cairo_xlib_surface_set_size(x11Surface, currentWidth, currentHeight);

                if (!isPositioningMode) {
                    XserverRegion region = XFixesCreateRegion(x11Display, nullptr, 0);
                    XFixesSetWindowShapeRegion(x11Display, x11Window, ShapeInput, 0, 0, region);
                    XFixesDestroyRegion(x11Display, region);
                }
            }

            // 2. Clear background surface
            cairo_reset_clip(x11Cr);
            cairo_set_operator(x11Cr, CAIRO_OPERATOR_CLEAR);
            cairo_paint(x11Cr);
            cairo_set_operator(x11Cr, CAIRO_OPERATOR_OVER);

            // 3. Draw bounding box plate (only in Move Mode)
            double textAlpha = std::clamp(config.opacity / 100.0, 0.1, 1.0);
            if (isPositioningMode) {
                cairo_set_source_rgba(x11Cr, 0.08, 0.12, 0.18, 0.90);
                DrawRoundedRectangle(x11Cr, 1.0, 1.0, currentWidth - 2.0, currentHeight - 2.0, 8.0);
                cairo_fill(x11Cr);

                cairo_set_source_rgba(x11Cr, 1.0, 0.84, 0.0, 0.95);
                cairo_set_line_width(x11Cr, 2.0);
                DrawRoundedRectangle(x11Cr, 1.0, 1.0, currentWidth - 2.0, currentHeight - 2.0, 8.0);
                cairo_stroke(x11Cr);
            }

            // 4. Render Rows and Segments
            double startX = padX;
            double startY = padY + fontSize;
            double shadowOffset = 1.0;

            for (size_t r = 0; r < rows.size(); ++r) {
                double curX = startX;
                double curY = startY + r * (fontSize + rowSpacing);

                if (isPositioningMode && r == 0) {
                    cairo_set_source_rgba(x11Cr, 1.0, 0.84, 0.0, 1.0);
                    cairo_move_to(x11Cr, curX, curY);
                    cairo_show_text(x11Cr, "[MOVE MODE] ");
                    cairo_text_extents_t badgeExt;
                    cairo_text_extents(x11Cr, "[MOVE MODE] ", &badgeExt);
                    curX += badgeExt.x_advance;
                }

                for (const auto& seg : rows[r]) {
                    if (seg.text.empty()) continue;
                    std::string utf8 = WStringToUtf8(seg.text);

                    cairo_text_extents_t extents;
                    cairo_text_extents(x11Cr, utf8.c_str(), &extents);

                    // Contrast drop shadow
                    if (config.dropShadow) {
                        cairo_set_source_rgba(x11Cr, 0.0, 0.0, 0.0, 0.75 * textAlpha);
                        cairo_move_to(x11Cr, curX + shadowOffset, curY + shadowOffset);
                        cairo_show_text(x11Cr, utf8.c_str());
                    }

                    // Segment color by type & severity
                    double red, green, blue, alpha;
                    if (seg.type == SegmentType::Separator || seg.isSeparator) {
                        ColorRefToRgba(config.separatorColor, textAlpha, red, green, blue, alpha);
                    } else if (seg.type == SegmentType::Label) {
                        ColorRefToRgba(config.labelColor, textAlpha, red, green, blue, alpha);
                    } else if (seg.severity == MetricSeverity::Critical) {
                        ColorRefToRgba(config.colorCritical, textAlpha, red, green, blue, alpha);
                    } else if (seg.severity == MetricSeverity::High) {
                        ColorRefToRgba(config.colorHigh, textAlpha, red, green, blue, alpha);
                    } else {
                        ColorRefToRgba(config.textColor, textAlpha, red, green, blue, alpha);
                    }

                    cairo_set_source_rgba(x11Cr, red, green, blue, alpha);
                    cairo_move_to(x11Cr, curX, curY);
                    cairo_show_text(x11Cr, utf8.c_str());

                    curX += extents.x_advance;
                }
            }

            cairo_surface_flush(x11Surface);
            XFlush(x11Display);
        }
#else
        (void)rows;
#endif
    }

    void ForceRepaint() {
        lastFrameHash = 0;
        UpdateMetrics(lastMetrics);
    }
};

static std::atomic<bool> s_pendingStop{false};
static std::atomic<bool> s_pendingToggleVisibility{false};
static std::atomic<bool> s_pendingTogglePositioningMode{false};
static std::atomic<bool> s_pendingReloadConfig{false};

void LinuxOverlay::RequestStop() {
    s_pendingStop.store(true);
}

void LinuxOverlay::RequestToggleVisibility() {
    s_pendingToggleVisibility.store(true);
}

void LinuxOverlay::RequestTogglePositioningMode() {
    s_pendingTogglePositioningMode.store(true);
}

void LinuxOverlay::RequestReloadConfig() {
    s_pendingReloadConfig.store(true);
}

LinuxOverlay::LinuxOverlay(AppConfig& config, const std::string& configPath)
    : config_(config),
      configPath_(configPath.empty() ? ConfigManager::GetDefaultConfigPath() : configPath),
      impl_(std::make_unique<Impl>(config)) {
}

LinuxOverlay::~LinuxOverlay() {
    Stop();
}

bool LinuxOverlay::Initialize() {
    bool ok = impl_->Initialize(backendType_);
    if (config_.showTrayIcon) {
        SetupTray();
    }
    return ok;
}

void LinuxOverlay::SetupTray() {
    if (!tray_) {
        tray_ = std::make_unique<LinuxTray>();
    }
    LinuxTrayCallbacks cbs;
    cbs.onToggleMove = [this]() { TogglePositioningMode(); };
    cbs.onCyclePreset = [this]() { CyclePositionPreset(); };
    cbs.onResetPosition = [this]() { SetPreset(PositionPreset::BottomCenter); };
    cbs.onToggleVisibility = [this]() { ToggleVisibility(); };
    cbs.onReloadConfig = []() { RequestReloadConfig(); };
    cbs.onExit = []() { RequestStop(); };
    tray_->Initialize(cbs);
}

void LinuxOverlay::Show() {
    isVisible_ = true;
    impl_->SetVisible(true);
    ForceRepaint();
}

void LinuxOverlay::Hide() {
    isVisible_ = false;
    impl_->SetVisible(false);
}

void LinuxOverlay::ToggleVisibility() {
    if (isVisible_) Hide();
    else Show();
}

void LinuxOverlay::TogglePositioningMode() {
    impl_->SetPositioningMode(!impl_->isPositioningMode);
}

bool LinuxOverlay::IsPositioningMode() const {
    return impl_->isPositioningMode;
}

void LinuxOverlay::CyclePositionPreset() {
    static const PositionPreset kPresets[] = {
        PositionPreset::BottomCenter,
        PositionPreset::BottomRight,
        PositionPreset::TopRight,
        PositionPreset::TopCenter,
        PositionPreset::TopLeft,
        PositionPreset::BottomLeft
    };
    int count = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
    int nextIdx = 0;
    for (int i = 0; i < count; ++i) {
        if (config_.preset == kPresets[i]) {
            nextIdx = (i + 1) % count;
            break;
        }
    }
    SetPreset(kPresets[nextIdx]);
}

void LinuxOverlay::SetPreset(PositionPreset preset) {
    config_.preset = preset;
    config_.customX = -1;
    config_.customY = -1;
    config_.customRelX = -1;
    config_.customRelY = -1;
    std::string savePath = configPath_.empty() ? ConfigManager::GetDefaultConfigPath() : configPath_;
    ConfigManager::Save(savePath, config_);
    ApplyConfig(config_);
}

void LinuxOverlay::ApplyConfig(const AppConfig& newConfig) {
    bool gridModeChanged = (config_.gridMode != newConfig.gridMode || config_.layout != newConfig.layout);
    bool trayChanged = (config_.showTrayIcon != newConfig.showTrayIcon);
    config_ = newConfig;
    if (gridModeChanged) {
        impl_->DestroyIslandWindows();
#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
        if (impl_->x11Window && impl_->x11Display) {
            XMapRaised(impl_->x11Display, impl_->x11Window);
        }
#endif
    }
    if (trayChanged) {
        if (config_.showTrayIcon) {
            SetupTray();
        } else if (tray_) {
            tray_->Shutdown();
        }
    }
    impl_->lastFrameHash = 0;
    impl_->UpdateMetrics(impl_->lastMetrics);
}

void LinuxOverlay::UpdateMetrics(const SystemMetrics& metrics) {
    if (!isVisible_) return;
    impl_->UpdateMetrics(metrics);
}

void LinuxOverlay::TriggerLaunchAnimation(const SystemMetrics& targetMetrics) {
    if (!isVisible_) return;
    impl_->TriggerLaunchAnimation(targetMetrics);
}

const SystemMetrics& LinuxOverlay::GetLastMetrics() const {
    return impl_->lastMetrics;
}

void LinuxOverlay::ForceRepaint() {
    impl_->ForceRepaint();
}

void LinuxOverlay::PollEvents() {
    if (tray_ && tray_->IsActive()) {
        tray_->Poll();
    }
    if (s_pendingStop.exchange(false)) {
        Stop();
    }
    if (s_pendingToggleVisibility.exchange(false)) {
        ToggleVisibility();
    }
    if (s_pendingTogglePositioningMode.exchange(false)) {
        TogglePositioningMode();
    }
    if (s_pendingReloadConfig.exchange(false)) {
        std::string cfgPath = configPath_.empty() ? ConfigManager::GetDefaultConfigPath() : configPath_;
        AppConfig reloaded = ConfigManager::Load(cfgPath);
        ApplyConfig(reloaded);
        if (onConfigReload_) onConfigReload_();
        TriggerLaunchAnimation(impl_->lastMetrics);
    }

#if defined(BARESTATS_HAS_X11) && !defined(_WIN32)
    if (impl_ && impl_->x11Display) {
        if (impl_->isPositioningMode && !impl_->isPointerGrabbed) {
            int screen = DefaultScreen(impl_->x11Display);
            Window rootWin = RootWindow(impl_->x11Display, screen);
            int grabRes = XGrabPointer(impl_->x11Display, rootWin, False,
                                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                       GrabModeAsync, GrabModeAsync,
                                       0, impl_->fleurCursor, CurrentTime);
            if (grabRes == GrabSuccess) {
                impl_->isPointerGrabbed = true;
                XGrabKeyboard(impl_->x11Display, rootWin, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            }
        }

        while (XPending(impl_->x11Display) > 0) {
            XEvent ev;
            XNextEvent(impl_->x11Display, &ev);
            if (ev.type == KeyPress) {
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                if (impl_->isPositioningMode) {
                    if (sym == XK_Escape || sym == XK_Return || sym == XK_KP_Enter ||
                        sym == XK_m || sym == XK_M ||
                        (impl_->keyEscape != 0 && ev.xkey.keycode == impl_->keyEscape) ||
                        (impl_->keyM != 0 && ev.xkey.keycode == impl_->keyM)) {
                        TogglePositioningMode();
                        continue;
                    }
                }
                if (impl_->keyM != 0 && ev.xkey.keycode == impl_->keyM) {
                    TogglePositioningMode();
                } else if (impl_->keyP != 0 && ev.xkey.keycode == impl_->keyP) {
                    CyclePositionPreset();
                } else if (impl_->keyH != 0 && ev.xkey.keycode == impl_->keyH) {
                    ToggleVisibility();
                }
            } else if (impl_->isPositioningMode) {
                if (ev.type == ButtonPress) {
                    if (ev.xbutton.button == Button3) {
                        TogglePositioningMode();
                        continue;
                    }
                    if (ev.xbutton.button == Button1) {
                        int px = ev.xbutton.x_root;
                        int py = ev.xbutton.y_root;
                        bool insideYellowBox = false;

                        if (impl_->config.layout == OverlayLayout::CustomGrid &&
                            impl_->config.gridMode == GridMode::Islands) {
                            for (const auto& iw : impl_->islandWindows) {
                                if (px >= iw.x && px < (iw.x + iw.width) &&
                                    py >= iw.y && py < (iw.y + iw.height)) {
                                    insideYellowBox = true;
                                    break;
                                }
                            }
                            if (!insideYellowBox && impl_->x11Window) {
                                int bannerW = 340;
                                int bannerH = 26;
                                int anchorX = impl_->currentX > 0 ? impl_->currentX :
                                              (!impl_->islandSlots.empty() ? impl_->islandSlots[0].relX : 16);
                                int anchorY = impl_->currentY > 32 ? impl_->currentY - 32 :
                                              (!impl_->islandSlots.empty() ? (std::max)(0, impl_->islandSlots[0].relY - 32) : 16);
                                if (px >= anchorX && px < (anchorX + bannerW) &&
                                    py >= anchorY && py < (anchorY + bannerH)) {
                                    insideYellowBox = true;
                                }
                            }
                        } else {
                            if (px >= impl_->currentX && px < (impl_->currentX + impl_->currentWidth) &&
                                py >= impl_->currentY && py < (impl_->currentY + impl_->currentHeight)) {
                                insideYellowBox = true;
                            }
                        }

                        if (insideYellowBox) {
                            impl_->isDragging = true;
                            impl_->dragOffsetX = px - impl_->currentX;
                            impl_->dragOffsetY = py - impl_->currentY;
                        } else {
                            // Clicked anywhere outside yellow box: exit reposition mode immediately
                            TogglePositioningMode();
                            continue;
                        }
                    }
                } else if (ev.type == MotionNotify && impl_->isDragging) {
                    int screen = DefaultScreen(impl_->x11Display);
                    int screenW = DisplayWidth(impl_->x11Display, screen);
                    int screenH = DisplayHeight(impl_->x11Display, screen);

                    int newX = ev.xmotion.x_root - impl_->dragOffsetX;
                    int newY = ev.xmotion.y_root - impl_->dragOffsetY;
                    newX = std::clamp(newX, 0, (std::max)(0, screenW - 60));
                    newY = std::clamp(newY, 0, (std::max)(0, screenH - 26));

                    int dx = newX - impl_->currentX;
                    int dy = newY - impl_->currentY;
                    impl_->currentX = newX;
                    impl_->currentY = newY;

                    if (impl_->config.layout == OverlayLayout::CustomGrid && impl_->config.gridMode == GridMode::Islands) {
                        for (auto& iw : impl_->islandWindows) {
                            iw.x += dx;
                            iw.y += dy;
                            XMoveWindow(impl_->x11Display, iw.window, iw.x, iw.y);
                        }
                        if (impl_->x11Window) {
                            XMoveWindow(impl_->x11Display, impl_->x11Window, impl_->currentX, (std::max)(0, impl_->currentY - 32));
                        }
                    } else {
                        XMoveWindow(impl_->x11Display, impl_->x11Window, impl_->currentX, impl_->currentY);
                    }
                } else if (ev.type == ButtonRelease && ev.xbutton.button == Button1) {
                    if (impl_->isDragging) {
                        impl_->isDragging = false;
                        config_.preset = PositionPreset::Custom;
                        config_.customX = impl_->currentX;
                        config_.customY = impl_->currentY;
                        std::string savePath = configPath_.empty() ? ConfigManager::GetDefaultConfigPath() : configPath_;
                        ConfigManager::Save(savePath, config_);
                    }
                }
            }
        }
    }
#endif

    if (impl_ && impl_->isAnimatingRollUp) {
        impl_->animTicksRemaining--;
        if (impl_->animTicksRemaining <= 0) {
            impl_->animTicksRemaining = 0;
            impl_->isAnimatingRollUp = false;
            impl_->RenderMetricsFrame(impl_->animTargetMetrics);
        } else {
            float t = 1.0f - (static_cast<float>(impl_->animTicksRemaining) / static_cast<float>(impl_->totalAnimTicks));
            // Ease-Out Quadratic: f(t) = 1 - (1 - t)^2
            float progress = 1.0f - (1.0f - t) * (1.0f - t);
            SystemMetrics cur = Impl::InterpolateMetrics(impl_->animTargetMetrics, progress);
            impl_->RenderMetricsFrame(cur);
        }
    }
}

void LinuxOverlay::Run() {
    isRunning_.store(true);
    while (isRunning_.load()) {
        PollEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 Hz event loop
    }
}

void LinuxOverlay::Stop() {
    if (tray_) {
        tray_->Shutdown();
    }
    isRunning_.store(false);
}

} // namespace BareStats
