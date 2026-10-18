#define _CRT_SECURE_NO_WARNINGS
#include "macos_overlay.h"

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

namespace BareStats {

// Sub-microsecond 64-bit FNV-1a Hash Combination for Frame Dirty Check
static inline size_t CombineHash(size_t seed, size_t value) {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

bool MacosOverlay::ComputeDirtyHash(const std::vector<std::vector<FormattedSegment>>& rows,
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
        hash = CombineHash(hash, 0xAA55AA55);
    }

    if (hash == inOutLastHash) {
        return false;
    }

    inOutLastHash = hash;
    return true;
}

std::vector<MacosIslandSlot> MacosOverlay::LayoutIslands(const AppConfig& config,
                                                        const SystemMetrics& metrics,
                                                        const MonitorInfo& monitor,
                                                        int slotPaddingX,
                                                        int slotPaddingY) {
    std::vector<MacosIslandSlot> slots;
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

    std::vector<int> colWidth(numCols, 0);
    for (int c = 0; c < numCols; ++c) {
        int maxW = 0;
        for (int r = 0; r < numRows; ++r) {
            if (cellWidths[r][c] > maxW) maxW = cellWidths[r][c];
        }
        colWidth[c] = (maxW > 0) ? maxW : 90;
    }

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

            MacosIslandSlot slot;
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

static inline NSColor* ColorRefToNSColor(COLORREF color, double alpha) {
    CGFloat r = ((color) & 0xFF) / 255.0;
    CGFloat g = ((color >> 8) & 0xFF) / 255.0;
    CGFloat b = ((color >> 16) & 0xFF) / 255.0;
    return [NSColor colorWithCalibratedRed:r green:g blue:b alpha:alpha];
}

} // namespace BareStats

// ==============================================================================
// Cocoa View & Window Implementation
// ==============================================================================

@interface BareStatsOverlayView : NSView
@property (nonatomic, assign) const BareStats::AppConfig* config;
@property (nonatomic, assign) BOOL isPositioningMode;
@property (nonatomic, assign) NSPoint initialDragLocation;
@property (nonatomic, copy) void (^onDragged)(int deltaX, int deltaY);
@property (nonatomic, copy) void (^onDragEnded)(void);
@property (nonatomic, copy) void (^onEscapePressed)(void);
- (void)setRows:(const std::vector<std::vector<BareStats::FormattedSegment>>&)newRows;
- (NSSize)calculateRequiredSize;
@end

@implementation BareStatsOverlayView {
    std::vector<std::vector<BareStats::FormattedSegment>> _rows;
}

- (void)setRows:(const std::vector<std::vector<BareStats::FormattedSegment>>&)newRows {
    _rows = newRows;
    [self setNeedsDisplay:YES];
}

- (BOOL)isFlipped {
    return YES; // Top-left origin matches BareStats layout engine
}

- (NSSize)calculateRequiredSize {
    if (_rows.empty() || !_config) {
        return NSMakeSize(400, 36);
    }

    int fontSize = (_config->fontSize > 0) ? _config->fontSize : 13;
    int padX = 14;
    int padY = 8;
    int rowHeight = fontSize + 8;
    int totalH = static_cast<int>(_rows.size()) * rowHeight + padY * 2;
    if (_isPositioningMode) totalH += 26; // Space for move mode banner

    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, NULL);
    CGFloat maxRowW = 0.0;

    for (const auto& row : _rows) {
        CGFloat curRowW = 0.0;
        for (const auto& seg : row) {
            if (seg.text.empty()) continue;
            std::string utf8 = BareStats::WStringToUtf8(seg.text);
            NSString* str = [NSString stringWithUTF8String:utf8.c_str()];
            if (!str) continue;

            NSDictionary* attr = @{ (id)kCTFontAttributeName: (__bridge id)font };
            NSAttributedString* attrStr = [[NSAttributedString alloc] initWithString:str attributes:attr];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attrStr);
            CGFloat w = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
            curRowW += w;
            CFRelease(line);
        }
        if (curRowW > maxRowW) maxRowW = curRowW;
    }

    if (font) CFRelease(font);
    CGFloat totalW = maxRowW + padX * 2;
    if (_isPositioningMode && totalW < 420) totalW = 420;
    return NSMakeSize(totalW, totalH);
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];

    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    if (!ctx || !_config) return;

    NSRect bounds = self.bounds;
    double alpha = _config->opacity / 100.0;

    // Reposition / Move Mode Visual Feedback
    if (_isPositioningMode) {
        CGContextSaveGState(ctx);

        // Subtle dark translucent background
        NSBezierPath* bgPath = [NSBezierPath bezierPathWithRoundedRect:bounds xRadius:8.0 yRadius:8.0];
        [[NSColor colorWithCalibratedRed:0.08 green:0.08 blue:0.08 alpha:0.85] setFill];
        [bgPath fill];

        // Crisp gold / amber border
        [[NSColor colorWithCalibratedRed:1.0 green:0.84 blue:0.0 alpha:0.95] setStroke];
        [bgPath setLineWidth:2.0];
        [bgPath stroke];

        // Move Mode Guide Banner
        NSString* bannerText = @"MOVE MODE • Drag to reposition • Esc or click outside to save";
        NSDictionary* bannerAttr = @{
            NSFontAttributeName: [NSFont boldSystemFontOfSize:11.0],
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:1.0 green:0.84 blue:0.0 alpha:1.0]
        };
        [bannerText drawAtPoint:NSMakePoint(14, 8) withAttributes:bannerAttr];

        CGContextRestoreGState(ctx);
    }

    if (_rows.empty()) return;

    int fontSize = (_config->fontSize > 0) ? _config->fontSize : 13;
    CGFloat padX = 14.0;
    CGFloat startY = _isPositioningMode ? 32.0 : 6.0;
    CGFloat rowHeight = fontSize + 8.0;

    CTFontRef baseFont = NULL;
    CTFontRef boldFont = NULL;

    if (_config->fontFace == L"auto" || _config->fontFace.empty()) {
        baseFont = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, NULL);
        boldFont = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, fontSize, NULL);
    } else {
        std::string faceName = BareStats::WStringToUtf8(_config->fontFace);
        CFStringRef cfName = CFStringCreateWithCString(kCFAllocatorDefault, faceName.c_str(), kCFStringEncodingUTF8);
        baseFont = CTFontCreateWithName(cfName, fontSize, NULL);
        boldFont = CTFontCreateWithName(cfName, fontSize, NULL);
        if (cfName) CFRelease(cfName);
    }

    if (!baseFont) baseFont = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, fontSize, NULL);
    if (!boldFont) boldFont = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, fontSize, NULL);

    for (size_t r = 0; r < _rows.size(); ++r) {
        const auto& row = _rows[r];
        CGFloat curX = padX;
        CGFloat curY = startY + r * rowHeight;

        for (const auto& seg : row) {
            if (seg.text.empty()) continue;
            std::string utf8 = BareStats::WStringToUtf8(seg.text);
            NSString* str = [NSString stringWithUTF8String:utf8.c_str()];
            if (!str) continue;

            NSColor* segColor = nil;
            if (seg.type == BareStats::SegmentType::Separator || seg.isSeparator) {
                segColor = BareStats::ColorRefToNSColor(_config->separatorColor, alpha);
            } else if (seg.type == BareStats::SegmentType::Label) {
                segColor = BareStats::ColorRefToNSColor(_config->labelColor, alpha);
            } else if (seg.severity == BareStats::MetricSeverity::Critical) {
                segColor = BareStats::ColorRefToNSColor(_config->colorCritical, alpha);
            } else if (seg.severity == BareStats::MetricSeverity::High) {
                segColor = BareStats::ColorRefToNSColor(_config->colorHigh, alpha);
            } else {
                segColor = BareStats::ColorRefToNSColor(_config->textColor, alpha);
            }

            CTFontRef useFont = (seg.isBold || _config->fontWeight == L"bold") ? boldFont : baseFont;

            NSMutableDictionary* attrs = [NSMutableDictionary dictionaryWithObjectsAndKeys:
                (__bridge id)useFont, (id)kCTFontAttributeName,
                (id)segColor.CGColor, (id)kCTForegroundColorAttributeName,
                nil];

            NSAttributedString* attrStr = [[NSAttributedString alloc] initWithString:str attributes:attrs];
            CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attrStr);

            CGContextSaveGState(ctx);
            if (_config->dropShadow) {
                CGColorRef shadowColor = CGColorCreateGenericRGB(0.0, 0.0, 0.0, 0.85 * alpha);
                CGContextSetShadowWithColor(ctx, CGSizeMake(1.0, -1.0), 2.0, shadowColor);
                CGColorRelease(shadowColor);
            }
            // Flip for CoreText line drawing within flipped view coordinate system
            CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
            CGContextTranslateCTM(ctx, curX, curY + fontSize);
            CGContextScaleCTM(ctx, 1.0, -1.0);

            CTLineDraw(line, ctx);
            CGContextRestoreGState(ctx);

            CGFloat width = CTLineGetTypographicBounds(line, NULL, NULL, NULL);
            curX += width;
            CFRelease(line);
        }
    }

    if (baseFont) CFRelease(baseFont);
    if (boldFont) CFRelease(boldFont);
}

// Mouse Drag Events for Move Mode
- (void)mouseDown:(NSEvent *)event {
    if (!_isPositioningMode) return;
    _initialDragLocation = [NSEvent mouseLocation];
}

- (void)mouseDragged:(NSEvent *)event {
    if (!_isPositioningMode) return;
    NSPoint currentLocation = [NSEvent mouseLocation];
    int deltaX = static_cast<int>(currentLocation.x - _initialDragLocation.x);
    int deltaY = static_cast<int>(currentLocation.y - _initialDragLocation.y);
    _initialDragLocation = currentLocation;
    if (_onDragged) {
        _onDragged(deltaX, deltaY);
    }
}

- (void)mouseUp:(NSEvent *)event {
    if (!_isPositioningMode) return;
    if (_onDragEnded) {
        _onDragEnded();
    }
}

- (void)keyDown:(NSEvent *)event {
    if (_isPositioningMode && event.keyCode == 53) { // Escape key
        if (_onEscapePressed) {
            _onEscapePressed();
        }
    } else {
        [super keyDown:event];
    }
}

- (BOOL)acceptsFirstResponder {
    return _isPositioningMode;
}

@end

@interface BareStatsMenuHandler : NSObject
@property (nonatomic, assign) BareStats::MacosOverlay* overlayOwner;
- (void)onToggleMoveMode:(id)sender;
- (void)onSelectPreset:(id)sender;
- (void)onToggleVisibility:(id)sender;
- (void)onReloadConfig:(id)sender;
- (void)onQuit:(id)sender;
@end

@implementation BareStatsMenuHandler
- (void)onToggleMoveMode:(id)sender {
    if (_overlayOwner) _overlayOwner->TogglePositioningMode();
}
- (void)onSelectPreset:(id)sender {
    if (_overlayOwner && [sender isKindOfClass:[NSMenuItem class]]) {
        NSMenuItem* item = (NSMenuItem*)sender;
        _overlayOwner->SetPreset(static_cast<BareStats::PositionPreset>(item.tag));
    }
}
- (void)onToggleVisibility:(id)sender {
    if (_overlayOwner) _overlayOwner->ToggleVisibility();
}
- (void)onReloadConfig:(id)sender {
    BareStats::MacosOverlay::RequestReloadConfig();
}
- (void)onQuit:(id)sender {
    BareStats::MacosOverlay::RequestStop();
}
@end

// ==============================================================================
// MacosOverlay Implementation
// ==============================================================================

namespace BareStats {

static MacosOverlay* g_activeOverlay = nullptr;

struct MacosOverlay::Impl {
    AppConfig& config;
    std::string configPath;
    NSPanel* panel = nil;
    BareStatsOverlayView* overlayView = nil;
    NSStatusItem* statusItem = nil;
    BareStatsMenuHandler* menuHandler = nil;

    std::vector<NSPanel*> islandPanels;
    std::vector<BareStatsOverlayView*> islandViews;
    std::vector<MacosIslandSlot> islandSlots;

    size_t lastFrameHash = 0;
    SystemMetrics lastMetrics{};
    SystemMetrics animTargetMetrics{};
    bool isAnimatingRollUp = false;
    int animTicksRemaining = 0;
    const int totalAnimTicks = 18;
    NSTimer* animTimer = nil;

    id globalEventMonitor = nil;
    id localEventMonitor = nil;
    bool isPositioningMode = false;

    Impl(AppConfig& cfg, const std::string& path)
        : config(cfg), configPath(path) {}

    ~Impl() {
        if (animTimer) {
            [animTimer invalidate];
            animTimer = nil;
        }
        if (globalEventMonitor) {
            [NSEvent removeMonitor:globalEventMonitor];
            globalEventMonitor = nil;
        }
        if (localEventMonitor) {
            [NSEvent removeMonitor:localEventMonitor];
            localEventMonitor = nil;
        }
        DestroyIslands();
        if (panel) {
            [panel close];
            panel = nil;
        }
    }

    void DestroyIslands() {
        for (NSPanel* p : islandPanels) {
            [p close];
        }
        islandPanels.clear();
        islandViews.clear();
        islandSlots.clear();
    }

    NSRect ConvertBareStatsToCocoa(int x, int y, int w, int h) {
        NSScreen* screen = [NSScreen mainScreen];
        CGFloat screenH = screen ? screen.frame.size.height : 1080.0;
        CGFloat screenY = screen ? screen.frame.origin.y : 0.0;
        return NSMakeRect(x, screenY + screenH - (y + h), w, h);
    }

    Point ConvertCocoaToBareStats(NSRect cocoaRect) {
        NSScreen* screen = [NSScreen mainScreen];
        CGFloat screenH = screen ? screen.frame.size.height : 1080.0;
        CGFloat screenY = screen ? screen.frame.origin.y : 0.0;
        Point pt;
        pt.x = static_cast<int>(cocoaRect.origin.x);
        pt.y = static_cast<int>(screenY + screenH - (cocoaRect.origin.y + cocoaRect.size.height));
        return pt;
    }

    void RecalculateWindowPosition(int contentWidth, int contentHeight) {
        if (!panel) return;

        NSArray<NSScreen*>* screens = [NSScreen screens];
        int monIdx = (config.monitorIndex >= 0 && config.monitorIndex < static_cast<int>(screens.count))
            ? config.monitorIndex : 0;
        NSScreen* screen = (screens.count > 0) ? screens[monIdx] : [NSScreen mainScreen];
        if (!screen) return;

        NSRect visibleFrame = screen.visibleFrame;
        NSRect screenFrame = screen.frame;

        CGFloat posX = 0.0;
        CGFloat posY = 0.0;
        CGFloat margin = static_cast<CGFloat>(config.margin);

        if (config.preset == PositionPreset::Custom && config.customX >= 0 && config.customY >= 0) {
            posX = screenFrame.origin.x + config.customX;
            posY = screenFrame.origin.y + screenFrame.size.height - (config.customY + contentHeight);
        } else {
            switch (config.preset) {
                case PositionPreset::TopLeft:
                    posX = visibleFrame.origin.x + margin;
                    posY = visibleFrame.origin.y + visibleFrame.size.height - contentHeight - margin;
                    break;
                case PositionPreset::TopCenter:
                    posX = visibleFrame.origin.x + (visibleFrame.size.width - contentWidth) / 2.0;
                    posY = visibleFrame.origin.y + visibleFrame.size.height - contentHeight - margin;
                    break;
                case PositionPreset::TopRight:
                    posX = visibleFrame.origin.x + visibleFrame.size.width - contentWidth - margin;
                    posY = visibleFrame.origin.y + visibleFrame.size.height - contentHeight - margin;
                    break;
                case PositionPreset::CenterLeft:
                    posX = visibleFrame.origin.x + margin;
                    posY = visibleFrame.origin.y + (visibleFrame.size.height - contentHeight) / 2.0;
                    break;
                case PositionPreset::Center:
                    posX = visibleFrame.origin.x + (visibleFrame.size.width - contentWidth) / 2.0;
                    posY = visibleFrame.origin.y + (visibleFrame.size.height - contentHeight) / 2.0;
                    break;
                case PositionPreset::CenterRight:
                    posX = visibleFrame.origin.x + visibleFrame.size.width - contentWidth - margin;
                    posY = visibleFrame.origin.y + (visibleFrame.size.height - contentHeight) / 2.0;
                    break;
                case PositionPreset::BottomLeft:
                    posX = visibleFrame.origin.x + margin;
                    posY = visibleFrame.origin.y + margin;
                    break;
                case PositionPreset::BottomRight:
                    posX = visibleFrame.origin.x + visibleFrame.size.width - contentWidth - margin;
                    posY = visibleFrame.origin.y + margin;
                    break;
                case PositionPreset::BottomCenter:
                default:
                    posX = visibleFrame.origin.x + (visibleFrame.size.width - contentWidth) / 2.0;
                    posY = visibleFrame.origin.y + margin;
                    break;
            }
        }

        NSRect cocoaFrame = NSMakeRect(posX, posY, contentWidth, contentHeight);
        [panel setFrame:cocoaFrame display:YES animate:NO];
        if (panel) {
            [panel orderFrontRegardless];
        }
    }

    void PaintIslands(const SystemMetrics& metrics) {
        if (panel) {
            [panel orderOut:nil];
        }

        auto monitors = MonitorHelper::EnumerateMonitors();
        int monIdx = (config.monitorIndex >= 0 && config.monitorIndex < static_cast<int>(monitors.size()))
            ? config.monitorIndex : 0;
        const auto& monitor = monitors[monIdx];

        islandSlots = MacosOverlay::LayoutIslands(config, metrics, monitor);

        while (islandPanels.size() < islandSlots.size()) {
            NSRect f = ConvertBareStatsToCocoa(0, 0, 100, 30);
            NSWindowStyleMask style = NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;
            NSPanel* p = [[NSPanel alloc] initWithContentRect:f
                                                    styleMask:style
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
            [p setOpaque:NO];
            [p setBackgroundColor:[NSColor clearColor]];
            [p setHasShadow:NO];
            [p setLevel:NSStatusWindowLevel];
            [p setIgnoresMouseEvents:YES];
            [p setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorStationary |
                                     NSWindowCollectionBehaviorIgnoresCycle];
            [p setHidesOnDeactivate:NO];

            BareStatsOverlayView* v = [[BareStatsOverlayView alloc] initWithFrame:f];
            v.config = &config;
            v.isPositioningMode = NO;
            [p setContentView:v];
            [p orderFrontRegardless];

            islandPanels.push_back(p);
            islandViews.push_back(v);
        }

        while (islandPanels.size() > islandSlots.size()) {
            [islandPanels.back() close];
            islandPanels.pop_back();
            islandViews.pop_back();
        }

        for (size_t i = 0; i < islandSlots.size(); ++i) {
            const auto& slot = islandSlots[i];
            NSPanel* p = islandPanels[i];
            BareStatsOverlayView* v = islandViews[i];

            auto cellSegs = Formatter::FormatCell(slot.token, metrics, &config);
            std::vector<std::vector<FormattedSegment>> cellRows = { cellSegs };
            [v setRows:cellRows];

            NSRect cocoaF = ConvertBareStatsToCocoa(slot.relX, slot.relY, slot.width, slot.height);
            [p setFrame:cocoaF display:YES animate:NO];
        }
    }

    static SystemMetrics InterpolateMetrics(const SystemMetrics& target, float progress) {
        SystemMetrics cur = target;
        cur.cpu.utilizationPercent = target.cpu.utilizationPercent * progress;
        if (target.cpu.temperatureC.has_value()) cur.cpu.temperatureC = *target.cpu.temperatureC * progress;
        if (target.cpu.powerWatts.has_value()) cur.cpu.powerWatts = *target.cpu.powerWatts * progress;
        if (target.cpu.fanRpm.has_value()) cur.cpu.fanRpm = static_cast<uint32_t>(*target.cpu.fanRpm * progress);

        cur.memory.usedBytes = static_cast<uint64_t>(target.memory.usedBytes * progress);
        cur.memory.totalBytes = target.memory.totalBytes;

        if (target.gpu.utilizationPercent.has_value()) cur.gpu.utilizationPercent = *target.gpu.utilizationPercent * progress;
        cur.gpu.memoryUsedBytes = static_cast<uint64_t>(target.gpu.memoryUsedBytes * progress);
        cur.gpu.memoryTotalBytes = target.gpu.memoryTotalBytes;
        if (target.gpu.temperatureC.has_value()) cur.gpu.temperatureC = static_cast<uint32_t>(*target.gpu.temperatureC * progress);
        if (target.gpu.powerWatts.has_value()) cur.gpu.powerWatts = static_cast<uint32_t>(*target.gpu.powerWatts * progress);
        if (target.gpu.fanRpm.has_value()) cur.gpu.fanRpm = static_cast<uint32_t>(*target.gpu.fanRpm * progress);
        else if (target.gpu.fanSpeedPercent.has_value()) cur.gpu.fanSpeedPercent = static_cast<uint32_t>(*target.gpu.fanSpeedPercent * progress);

        cur.gpus.resize(target.gpus.size());
        for (size_t i = 0; i < target.gpus.size(); ++i) {
            cur.gpus[i] = target.gpus[i];
            if (target.gpus[i].utilizationPercent.has_value()) cur.gpus[i].utilizationPercent = *target.gpus[i].utilizationPercent * progress;
            cur.gpus[i].memoryUsedBytes = static_cast<uint64_t>(target.gpus[i].memoryUsedBytes * progress);
        }

        cur.network.rxBytesPerSec = static_cast<uint64_t>(target.network.rxBytesPerSec * progress);
        cur.network.txBytesPerSec = static_cast<uint64_t>(target.network.txBytesPerSec * progress);
        return cur;
    }

    void SetupStatusItem(MacosOverlay* overlayOwner) {
        statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
        if (!statusItem) return;

        menuHandler = [[BareStatsMenuHandler alloc] init];
        menuHandler.overlayOwner = overlayOwner;

        // Load official BareStats application icon from bundle resources
        NSImage* appIcon = [[NSBundle mainBundle] imageForResource:@"barestats"];
        if (!appIcon) {
            NSString* icnsPath = [[NSBundle mainBundle] pathForResource:@"barestats" ofType:@"icns"];
            if (icnsPath) {
                appIcon = [[NSImage alloc] initWithContentsOfFile:icnsPath];
            }
        }
        if (!appIcon) {
            NSString* pngPath = [[NSBundle mainBundle] pathForResource:@"barestats" ofType:@"png"];
            if (pngPath) {
                appIcon = [[NSImage alloc] initWithContentsOfFile:pngPath];
            }
        }
        if (!appIcon) {
            appIcon = [NSImage imageNamed:@"barestats"];
        }
        if (!appIcon) {
            appIcon = [NSApp applicationIconImage];
        }

        if (appIcon) {
            NSImage* menuIcon = [appIcon copy];
            [menuIcon setSize:NSMakeSize(18, 18)];
            [menuIcon setTemplate:NO];
            statusItem.button.image = menuIcon;
        } else {
            // Create vector template icon fallback
            NSImage* icon = [NSImage imageWithSize:NSMakeSize(18, 18) flipped:NO drawingHandler:^BOOL(NSRect dstRect) {
                (void)dstRect;
                [[NSColor blackColor] setStroke];
                NSBezierPath* path = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(2, 2, 14, 14) xRadius:3 yRadius:3];
                [path setLineWidth:1.5];
                [path stroke];

                NSBezierPath* bar = [NSBezierPath bezierPath];
                [bar moveToPoint:NSMakePoint(5, 5)];
                [bar lineToPoint:NSMakePoint(5, 11)];
                [bar moveToPoint:NSMakePoint(9, 5)];
                [bar lineToPoint:NSMakePoint(9, 13)];
                [bar moveToPoint:NSMakePoint(13, 5)];
                [bar lineToPoint:NSMakePoint(13, 8)];
                [bar setLineWidth:1.5];
                [bar stroke];
                return YES;
            }];
            [icon setTemplate:YES];
            statusItem.button.image = icon;
        }

        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"BareStats"];

        // Toggle Reposition Mode
        NSMenuItem* moveItem = [[NSMenuItem alloc] initWithTitle:@"Toggle Reposition Mode"
                                                          action:@selector(onToggleMoveMode:)
                                                   keyEquivalent:@"m"];
        [moveItem setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagOption];
        [moveItem setTarget:menuHandler];
        [menu addItem:moveItem];

        // Presets Submenu
        NSMenuItem* presetsMenu = [[NSMenuItem alloc] initWithTitle:@"Position Presets" action:nil keyEquivalent:@""];
        NSMenu* subMenu = [[NSMenu alloc] initWithTitle:@"Presets"];

        static const struct { const char* label; PositionPreset preset; } kPresets[] = {
            { "Bottom Center", PositionPreset::BottomCenter },
            { "Bottom Right",  PositionPreset::BottomRight },
            { "Top Right",     PositionPreset::TopRight },
            { "Top Center",    PositionPreset::TopCenter },
            { "Top Left",      PositionPreset::TopLeft },
            { "Bottom Left",   PositionPreset::BottomLeft },
            { "Center",        PositionPreset::Center },
        };

        for (const auto& p : kPresets) {
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:p.label]
                                                          action:@selector(onSelectPreset:)
                                                   keyEquivalent:@""];
            item.tag = static_cast<NSInteger>(p.preset);
            [item setTarget:menuHandler];
            [subMenu addItem:item];
        }
        [presetsMenu setSubmenu:subMenu];
        [menu addItem:presetsMenu];

        // Visibility Toggle
        NSMenuItem* visItem = [[NSMenuItem alloc] initWithTitle:@"Toggle Visibility"
                                                         action:@selector(onToggleVisibility:)
                                                  keyEquivalent:@"h"];
        [visItem setKeyEquivalentModifierMask:NSEventModifierFlagControl | NSEventModifierFlagOption];
        [visItem setTarget:menuHandler];
        [menu addItem:visItem];

        // Reload Config
        NSMenuItem* reloadItem = [[NSMenuItem alloc] initWithTitle:@"Reload Configuration"
                                                            action:@selector(onReloadConfig:)
                                                     keyEquivalent:@"r"];
        [reloadItem setTarget:menuHandler];
        [menu addItem:reloadItem];

        [menu addItem:[NSMenuItem separatorItem]];

        // Quit
        NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit BareStats"
                                                          action:@selector(onQuit:)
                                                   keyEquivalent:@"q"];
        [quitItem setTarget:menuHandler];
        [menu addItem:quitItem];

        statusItem.menu = menu;
    }
};

MacosOverlay::MacosOverlay(AppConfig& config, const std::string& configPath)
    : config_(config), configPath_(configPath), impl_(std::make_unique<Impl>(config, configPath)) {
    g_activeOverlay = this;
}

MacosOverlay::~MacosOverlay() {
    if (g_activeOverlay == this) {
        g_activeOverlay = nullptr;
    }
}

bool MacosOverlay::Initialize() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSRect frame = impl_->ConvertBareStatsToCocoa(0, 0, 500, 36);
        NSWindowStyleMask style = NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;

        impl_->panel = [[NSPanel alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
        if (!impl_->panel) {
            return false;
        }

        [impl_->panel setOpaque:NO];
        [impl_->panel setBackgroundColor:[NSColor clearColor]];
        [impl_->panel setHasShadow:NO];
        [impl_->panel setLevel:NSStatusWindowLevel];
        [impl_->panel setIgnoresMouseEvents:YES];
        [impl_->panel setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                            NSWindowCollectionBehaviorStationary |
                                            NSWindowCollectionBehaviorIgnoresCycle];
        [impl_->panel setHidesOnDeactivate:NO];

        impl_->overlayView = [[BareStatsOverlayView alloc] initWithFrame:frame];
        impl_->overlayView.config = &config_;
        impl_->overlayView.isPositioningMode = NO;

#if __has_feature(objc_arc)
        __weak NSPanel* weakPanel = impl_->panel;
#else
        __unsafe_unretained NSPanel* weakPanel = impl_->panel;
#endif
        impl_->overlayView.onDragged = ^(int deltaX, int deltaY) {
            if (!weakPanel) return;
            NSRect cur = [weakPanel frame];
            cur.origin.x += deltaX;
            cur.origin.y += deltaY;
            [weakPanel setFrame:cur display:YES animate:NO];
        };

        impl_->overlayView.onDragEnded = ^{
            if (!weakPanel) return;
            Point pt = impl_->ConvertCocoaToBareStats([weakPanel frame]);
            config_.customX = pt.x;
            config_.customY = pt.y;
            config_.preset = PositionPreset::Custom;
            if (!configPath_.empty()) {
                ConfigManager::Save(configPath_, config_);
            }
        };

        impl_->overlayView.onEscapePressed = ^{
            this->SetPositioningMode(false);
        };

        [impl_->panel setContentView:impl_->overlayView];
        impl_->SetupStatusItem(this);
    }
    return true;
}

void MacosOverlay::Show() {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->Show();
        });
        return;
    }
    isVisible_ = true;
    if (impl_->panel) {
        [impl_->panel orderFrontRegardless];
    }
}

void MacosOverlay::Hide() {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->Hide();
        });
        return;
    }
    isVisible_ = false;
    if (impl_->panel) {
        [impl_->panel orderOut:nil];
    }
}

void MacosOverlay::ToggleVisibility() {
    if (isVisible_) Hide();
    else Show();
}

void MacosOverlay::SetPositioningMode(bool enable) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->SetPositioningMode(enable);
        });
        return;
    }
    impl_->isPositioningMode = enable;
    if (impl_->overlayView) {
        impl_->overlayView.isPositioningMode = enable;
    }

    if (enable) {
        [impl_->panel setIgnoresMouseEvents:NO];
        [impl_->panel makeKeyAndOrderFront:nil];

        // Global monitor for clicks outside overlay to exit move mode
#if __has_feature(objc_arc)
        __weak NSPanel* weakPanel = impl_->panel;
#else
        __unsafe_unretained NSPanel* weakPanel = impl_->panel;
#endif
        impl_->globalEventMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown handler:^(NSEvent *e) {
            (void)e;
            if (!weakPanel) return;
            NSPoint clickLoc = [NSEvent mouseLocation];
            if (!NSPointInRect(clickLoc, [weakPanel frame])) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    this->SetPositioningMode(false);
                });
            }
        }];
    } else {
        [impl_->panel setIgnoresMouseEvents:YES];
        if (impl_->globalEventMonitor) {
            [NSEvent removeMonitor:impl_->globalEventMonitor];
            impl_->globalEventMonitor = nil;
        }
        if (!configPath_.empty()) {
            ConfigManager::Save(configPath_, config_);
        }
    }

    ForceRepaint();
}

void MacosOverlay::TogglePositioningMode() {
    SetPositioningMode(!impl_->isPositioningMode);
}

bool MacosOverlay::IsPositioningMode() const {
    return impl_->isPositioningMode;
}

void MacosOverlay::CyclePositionPreset() {
    static const PositionPreset kOrder[] = {
        PositionPreset::BottomCenter, PositionPreset::BottomRight,
        PositionPreset::TopRight, PositionPreset::TopCenter,
        PositionPreset::TopLeft, PositionPreset::BottomLeft
    };
    int count = sizeof(kOrder) / sizeof(kOrder[0]);
    int next = 0;
    for (int i = 0; i < count; ++i) {
        if (config_.preset == kOrder[i]) {
            next = (i + 1) % count;
            break;
        }
    }
    SetPreset(kOrder[next]);
}

void MacosOverlay::SetPreset(PositionPreset preset) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->SetPreset(preset);
        });
        return;
    }
    config_.preset = preset;
    config_.customX = -1;
    config_.customY = -1;
    if (!configPath_.empty()) {
        ConfigManager::Save(configPath_, config_);
    }
    ForceRepaint();
}

void MacosOverlay::ApplyConfig(const AppConfig& newConfig) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->ApplyConfig(newConfig);
        });
        return;
    }
    config_ = newConfig;
    if (impl_->overlayView) {
        impl_->overlayView.config = &config_;
    }
    ForceRepaint();
}

void MacosOverlay::TriggerLaunchAnimation(const SystemMetrics& targetMetrics) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->TriggerLaunchAnimation(targetMetrics);
        });
        return;
    }
    impl_->animTargetMetrics = targetMetrics;
    impl_->lastMetrics = targetMetrics;
    impl_->animTicksRemaining = impl_->totalAnimTicks;
    impl_->isAnimatingRollUp = true;

    if (impl_->animTimer) {
        [impl_->animTimer invalidate];
    }

    impl_->animTimer = [NSTimer scheduledTimerWithTimeInterval:0.016 repeats:YES block:^(NSTimer *t) {
        impl_->animTicksRemaining--;
        if (impl_->animTicksRemaining <= 0) {
            [t invalidate];
            impl_->animTimer = nil;
            impl_->isAnimatingRollUp = false;
            this->UpdateMetrics(impl_->animTargetMetrics);
        } else {
            float progressFraction = 1.0f - (static_cast<float>(impl_->animTicksRemaining) / static_cast<float>(impl_->totalAnimTicks));
            float progress = 1.0f - (1.0f - progressFraction) * (1.0f - progressFraction);
            SystemMetrics cur = Impl::InterpolateMetrics(impl_->animTargetMetrics, progress);
            auto rows = Formatter::FormatRows(cur, &config_);
            if (impl_->overlayView) {
                [impl_->overlayView setRows:rows];
                NSSize req = [impl_->overlayView calculateRequiredSize];
                impl_->RecalculateWindowPosition(req.width, req.height);
            }
        }
    }];
}

void MacosOverlay::UpdateMetrics(const SystemMetrics& metrics) {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->UpdateMetrics(metrics);
        });
        return;
    }
    if (impl_->isAnimatingRollUp) {
        impl_->animTargetMetrics = metrics;
        return;
    }
    impl_->lastMetrics = metrics;

    if (config_.layout == OverlayLayout::CustomGrid && config_.gridMode == GridMode::Islands) {
        impl_->PaintIslands(metrics);
        return;
    }

    impl_->DestroyIslands();
    if (!impl_->panel) return;
    if (isVisible_) [impl_->panel orderFrontRegardless];

    auto rows = Formatter::FormatRows(metrics, &config_);

    if (!impl_->isPositioningMode) {
        bool dirty = ComputeDirtyHash(rows, impl_->lastFrameHash);
        if (!dirty) {
            return; // Sub-microsecond bypass: 0.00% idle duty cycle
        }
    }

    if (impl_->overlayView) {
        [impl_->overlayView setRows:rows];
        NSSize req = [impl_->overlayView calculateRequiredSize];
        impl_->RecalculateWindowPosition(req.width, req.height);
    }
}

const SystemMetrics& MacosOverlay::GetLastMetrics() const {
    return impl_->lastMetrics;
}

void MacosOverlay::ForceRepaint() {
    if (![NSThread isMainThread]) {
        dispatch_async(dispatch_get_main_queue(), ^{
            this->ForceRepaint();
        });
        return;
    }
    impl_->lastFrameHash = 0;
    UpdateMetrics(impl_->lastMetrics);
}

void MacosOverlay::PollEvents() {
    @autoreleasepool {
        NSEvent* event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                           untilDate:[NSDate distantPast]
                                              inMode:NSDefaultRunLoopMode
                                             dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

void MacosOverlay::Run() {
    isRunning_.store(true);
    @autoreleasepool {
        [NSApp run];
    }
}

void MacosOverlay::Stop() {
    isRunning_.store(false);
    [NSApp stop:nil];
}

void MacosOverlay::RequestStop() {
    if (g_activeOverlay) g_activeOverlay->Stop();
}

void MacosOverlay::RequestToggleVisibility() {
    if (g_activeOverlay) g_activeOverlay->ToggleVisibility();
}

void MacosOverlay::RequestTogglePositioningMode() {
    if (g_activeOverlay) g_activeOverlay->TogglePositioningMode();
}

void MacosOverlay::RequestReloadConfig() {
    if (g_activeOverlay && g_activeOverlay->onConfigReload_) {
        g_activeOverlay->onConfigReload_();
    }
}

} // namespace BareStats

#endif // __APPLE__
