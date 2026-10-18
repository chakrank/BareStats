#include "formatter.h"
#include "../config/config.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    return wstr;
}
static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string str(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], len, nullptr, nullptr);
    return str;
}
#else
static std::wstring Utf8ToWide(const std::string& str) {
    std::wstring out;
    size_t i = 0;
    while (i < str.size()) {
        uint8_t c = static_cast<uint8_t>(str[i]);
        if (c < 0x80) {
            out.push_back(static_cast<wchar_t>(c));
            i++;
        } else if ((c >> 5) == 0x6 && i + 1 < str.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(str[i + 1]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < str.size()) {
            uint32_t cp = ((c & 0x0F) << 12) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 2]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < str.size()) {
            uint32_t cp = ((c & 0x07) << 18) | ((static_cast<uint8_t>(str[i + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(str[i + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(str[i + 3]) & 0x3F);
            out.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            i++;
        }
    }
    return out;
}

static std::string WideToUtf8(const std::wstring& wstr) {
    std::string out;
    for (wchar_t wc : wstr) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}
#endif

namespace BareStats {

static MetricSeverity GetSeverity(double percent, double highThresh = 80.0, double critThresh = 90.0) {
    if (percent >= critThresh) return MetricSeverity::Critical;
    if (percent >= highThresh) return MetricSeverity::High;
    return MetricSeverity::Normal;
}

std::wstring Formatter::FormatBytesGb(uint64_t usedBytes, uint64_t totalBytes) {
    if (totalBytes < 1024ULL * 1024 * 1024 && totalBytes > 0) {
        uint32_t usedMb = static_cast<uint32_t>(usedBytes / (1024 * 1024));
        uint32_t totalMb = static_cast<uint32_t>(totalBytes / (1024 * 1024));
        return std::to_wstring(usedMb) + L"/" + std::to_wstring(totalMb) + L"MB";
    }

    double usedGb = static_cast<double>(usedBytes) / (1024.0 * 1024.0 * 1024.0);
    double totalGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);

    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1) << usedGb << L"/";
    
    // Check if total is integer-like (e.g. 16, 24, 32, 64)
    double rounded = std::round(totalGb);
    if (totalGb >= 1.0 && std::abs(totalGb - rounded) < 0.15) {
        oss << static_cast<uint32_t>(rounded) << L"GB";
    } else {
        oss << std::fixed << std::setprecision(1) << totalGb << L"GB";
    }

    return oss.str();
}

std::wstring Formatter::FormatNetworkRate(uint64_t bytesPerSec) {
    double bytes = static_cast<double>(bytesPerSec);
    std::wostringstream oss;
    oss << std::fixed;
    if (bytes < 1024.0) {
        oss << std::setprecision(0) << bytes << L"B";
    } else if (bytes < 1024.0 * 1024.0) {
        double kb = bytes / 1024.0;
        if (kb >= 100.0) {
            oss << std::setprecision(0) << kb << L"K";
        } else {
            oss << std::setprecision(1) << kb << L"K";
        }
    } else if (bytes < 1024.0 * 1024.0 * 1024.0) {
        double mb = bytes / (1024.0 * 1024.0);
        if (mb >= 100.0) {
            oss << std::setprecision(0) << mb << L"M";
        } else {
            oss << std::setprecision(1) << mb << L"M";
        }
    } else {
        double gb = bytes / (1024.0 * 1024.0 * 1024.0);
        oss << std::setprecision(1) << gb << L"G";
    }
    return oss.str();
}

static void AppendMemorySegments(std::vector<FormattedSegment>& segments, uint64_t usedBytes, uint64_t totalBytes, MetricSeverity sev, bool showMax = true) {
    if (totalBytes < 1024ULL * 1024 * 1024 && totalBytes > 0) {
        uint32_t usedMb = static_cast<uint32_t>(std::round(static_cast<double>(usedBytes) / (1024.0 * 1024.0)));
        segments.push_back({ std::to_wstring(usedMb), sev, SegmentType::Number, true, false });

        if (showMax) {
            segments.push_back({ L"/", sev, SegmentType::Unit, false, false });
            uint32_t totalMb = static_cast<uint32_t>(std::round(static_cast<double>(totalBytes) / (1024.0 * 1024.0)));
            segments.push_back({ std::to_wstring(totalMb), sev, SegmentType::Number, true, false });
        }
        segments.push_back({ L"MB", sev, SegmentType::Unit, false, false });
        return;
    }

    double usedGb = static_cast<double>(usedBytes) / (1024.0 * 1024.0 * 1024.0);

    std::wostringstream ossUsed;
    ossUsed << std::fixed << std::setprecision(1) << usedGb;
    segments.push_back({ ossUsed.str(), sev, SegmentType::Number, true, false }); // Used number: Prominent Bold

    if (showMax && totalBytes > 0) {
        double totalGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
        segments.push_back({ L"/", sev, SegmentType::Unit, false, false });         // Slash: Normal

        std::wostringstream ossTotal;
        double rounded = std::round(totalGb);
        if (totalGb >= 1.0 && std::abs(totalGb - rounded) < 0.15) {
            ossTotal << static_cast<uint32_t>(rounded);
        } else {
            ossTotal << std::fixed << std::setprecision(1) << totalGb;
        }
        segments.push_back({ ossTotal.str(), sev, SegmentType::Number, true, false }); // Total number: Prominent Bold
    }

    segments.push_back({ L"GB", sev, SegmentType::Unit, false, false });         // Unit: Normal
}

std::vector<FormattedSegment> Formatter::FormatSegments(const SystemMetrics& metrics, const AppConfig* config) {
    std::vector<FormattedSegment> segments;

    bool showCpu = !config || config->showCpu;
    bool showGpu = !config || config->showGpu;
    bool showRam = !config || config->showRam;
    bool showNet = config && config->showNet;
    bool showMaxMemory = !config || config->showMaxMemory;
    bool hideOffline = config && config->hideOfflineMetrics;
    bool enableColors = !config || config->enableDynamicColors;

    std::wstring sepStr = (config && !config->separator.empty()) ? config->separator : L"\x2502";

    auto applySeverity = [enableColors](MetricSeverity sev) -> MetricSeverity {
        return enableColors ? sev : MetricSeverity::Normal;
    };

    bool hasPreviousBlock = false;

    auto appendSeparator = [&]() {
        if (hasPreviousBlock) {
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        segments.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
    };

    auto appendCpu = [&]() {
        appendSeparator();
        segments.push_back({ L" CPU ", MetricSeverity::Normal, SegmentType::Label, false, false });

        // CPU Util
        MetricSeverity cpuSev = applySeverity(GetSeverity(metrics.cpu.utilizationPercent, 80.0, 90.0));
        segments.push_back({ std::to_wstring(static_cast<int>(std::round(metrics.cpu.utilizationPercent))), cpuSev, SegmentType::Number, true, false });
        segments.push_back({ L"%", cpuSev, SegmentType::Unit, false, false });

        // CPU Temp
        if (metrics.cpu.temperatureC.has_value()) {
            MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.cpu.temperatureC, 75.0, 85.0));
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segments.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.temperatureC))), tempSev, SegmentType::Number, true, false });
            segments.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
        }

        // CPU Power
        if (metrics.cpu.powerWatts.has_value()) {
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segments.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.powerWatts))), MetricSeverity::Normal, SegmentType::Number, true, false });
            segments.push_back({ L"W", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }

        // CPU Fan
        if (metrics.cpu.fanRpm.has_value()) {
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segments.push_back({ std::to_wstring(*metrics.cpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
            segments.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }

        hasPreviousBlock = true;
    };
 
    auto appendSingleGpu = [&](const GpuMetrics& gpu, const std::wstring& label, const PerGpuConfig& gpuCfg) {
        appendSeparator();
        segments.push_back({ L" " + label + L" ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (gpu.isAvailable) {
            bool hasPrintedData = false;

            // GPU Util (only render if known - never output false 0%)
            if (gpuCfg.showUtil && gpu.utilizationPercent.has_value()) {
                MetricSeverity gpuSev = applySeverity(GetSeverity(*gpu.utilizationPercent, 80.0, 90.0));
                segments.push_back({ std::to_wstring(static_cast<int>(std::round(*gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
                segments.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // VRAM
            if (gpuCfg.showVram && gpu.memoryTotalBytes > 0) {
                uint64_t totalBytes = gpu.memoryTotalBytes;
                if (config && config->vramMode == VramMode::Usable && gpu.memoryReservedBytes > 0 && totalBytes > gpu.memoryReservedBytes) {
                    totalBytes -= gpu.memoryReservedBytes;
                }
                double vramPct = (static_cast<double>(gpu.memoryUsedBytes) / totalBytes) * 100.0;
                MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                AppendMemorySegments(segments, gpu.memoryUsedBytes, totalBytes, vramSev, showMaxMemory);
                hasPrintedData = true;
            }

            // GPU Temp
            if (gpuCfg.showTemp && gpu.temperatureC.has_value()) {
                double critLimit = gpu.tempSlowdownLimitC.has_value() 
                    ? static_cast<double>(*gpu.tempSlowdownLimitC - 2) 
                    : 83.0;
                if (critLimit > 83.0) critLimit = 83.0;
                double highLimit = critLimit - 8.0;
                MetricSeverity tempSev = applySeverity(GetSeverity(*gpu.temperatureC, highLimit, critLimit));
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*gpu.temperatureC), tempSev, SegmentType::Number, true, false });
                segments.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // GPU Power
            if (gpuCfg.showPower && gpu.powerWatts.has_value()) {
                MetricSeverity powerSev = MetricSeverity::Normal;
                if (gpu.powerLimitWatts.has_value() && *gpu.powerLimitWatts > 0) {
                    double powerPct = (static_cast<double>(*gpu.powerWatts) / *gpu.powerLimitWatts) * 100.0;
                    powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
                }
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*gpu.powerWatts), powerSev, SegmentType::Number, true, false });
                segments.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // GPU Fan
            if (gpuCfg.showFan) {
                if (gpu.fanRpm.has_value()) {
                    if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    segments.push_back({ std::to_wstring(*gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                    segments.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
                } else if (gpu.fanSpeedPercent.has_value()) {
                    MetricSeverity fanSev = applySeverity(GetSeverity(*gpu.fanSpeedPercent, 80.0, 95.0));
                    if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    segments.push_back({ std::to_wstring(*gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
                    segments.push_back({ L"%", fanSev, SegmentType::Unit, false, false });
                }
            }

            if (!hasPrintedData) {
                segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
        } else {
            segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }

        hasPreviousBlock = true;
    };

    auto appendGpu = [&]() {
        if (metrics.gpus.size() > 1) {
            for (size_t g = 0; g < metrics.gpus.size(); ++g) {
                uint32_t targetIdx = (config && g < config->targetGpuIndices.size() && !config->targetAllGpus)
                    ? config->targetGpuIndices[g]
                    : static_cast<uint32_t>(g);
                PerGpuConfig gCfg = config ? config->GetGpuConfig(targetIdx) : PerGpuConfig{};
                if (!gCfg.enabled) continue;
                std::wstring label = !gCfg.label.empty() ? Utf8ToWide(gCfg.label) : (L"GPU" + std::to_wstring(g));
                appendSingleGpu(metrics.gpus[g], label, gCfg);
            }
        } else {
            uint32_t targetIdx = (config && !config->targetGpuIndices.empty()) ? config->targetGpuIndices[0] : 0;
            PerGpuConfig gCfg = config ? config->GetGpuConfig(targetIdx) : PerGpuConfig{};
            if (gCfg.enabled) {
                std::wstring label = !gCfg.label.empty() ? Utf8ToWide(gCfg.label) : L"GPU";
                appendSingleGpu(metrics.gpu, label, gCfg);
            }
        }
    };

    auto appendRam = [&]() {
        appendSeparator();
        segments.push_back({ L" RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            AppendMemorySegments(segments, metrics.memory.usedBytes, metrics.memory.totalBytes, ramSev, showMaxMemory);
        } else {
            segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }

        hasPreviousBlock = true;
    };

    auto appendNet = [&]() {
        appendSeparator();
        segments.push_back({ L" NET ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (metrics.network.isAvailable) {
            std::wstring rxStr = FormatNetworkRate(metrics.network.rxBytesPerSec);
            segments.push_back({ rxStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segments.push_back({ L"\x2193", MetricSeverity::Normal, SegmentType::Unit, false, false }); // ↓

            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });

            std::wstring txStr = FormatNetworkRate(metrics.network.txBytesPerSec);
            segments.push_back({ txStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segments.push_back({ L"\x2191", MetricSeverity::Normal, SegmentType::Unit, false, false }); // ↑
        } else {
            segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }

        hasPreviousBlock = true;
    };

    std::vector<std::string> defaultOrder = { "CPU", "GPU", "RAM", "NET" };
    const std::vector<std::string>& activeOrder = (config && !config->metricOrder.empty()) ? config->metricOrder : defaultOrder;

    bool netRendered = false;
    for (const auto& item : activeOrder) {
        if (item == "CPU" && showCpu) {
            appendCpu();
        } else if (item == "GPU" && showGpu) {
            bool hasAvailableGpu = metrics.gpu.isAvailable;
            for (const auto& g : metrics.gpus) {
                if (g.isAvailable) { hasAvailableGpu = true; break; }
            }
            if (!hideOffline || hasAvailableGpu) {
                appendGpu();
            }
        } else if (item == "RAM" && showRam) {
            appendRam();
        } else if (item == "NET" && showNet && (!hideOffline || metrics.network.isAvailable)) {
            appendNet();
            netRendered = true;
        }
    }

    if (showNet && !netRendered && (!hideOffline || metrics.network.isAvailable)) {
        appendNet();
    }

    if (hasPreviousBlock) {
        segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        segments.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
    }

    return segments;
}

std::vector<std::vector<FormattedSegment>> Formatter::FormatRows(const SystemMetrics& metrics, const AppConfig* config) {
    if (!config || config->layout == OverlayLayout::Horizontal) {
        auto segs = FormatSegments(metrics, config);
        if (segs.empty()) return {};
        return { segs };
    }

    if (config->layout == OverlayLayout::CustomGrid) {
        std::vector<std::vector<FormattedSegment>> gridRows;
        std::wstring sepStr = (!config->separator.empty()) ? config->separator : L"\x2502";

        for (const auto& r : config->customGrid.rows) {
            std::vector<FormattedSegment> rowSegs;
            bool hasBlock = false;
            for (const auto& cell : r.cells) {
                if (cell.token.empty()) continue;
                auto cellSegs = FormatCell(cell.token, metrics, config);
                if (cellSegs.empty()) continue;

                if (hasBlock) {
                    rowSegs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    rowSegs.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
                }
                rowSegs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                rowSegs.insert(rowSegs.end(), cellSegs.begin(), cellSegs.end());
                hasBlock = true;
            }
            if (hasBlock) {
                rowSegs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                rowSegs.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
            }
            gridRows.push_back(std::move(rowSegs));
        }
        return gridRows;
    }

    // Grid Layout Mode:
    std::vector<std::vector<FormattedSegment>> rows;

    bool showCpu = !config || config->showCpu;
    bool showGpu = !config || config->showGpu;
    bool showRam = !config || config->showRam;
    bool showNet = config && config->showNet;
    bool showMaxMemory = !config || config->showMaxMemory;
    bool hideOffline = config && config->hideOfflineMetrics;
    bool enableColors = !config || config->enableDynamicColors;

    std::wstring sepStr = (!config->separator.empty()) ? config->separator : L"\x2502";

    auto applySeverity = [enableColors](MetricSeverity sev) -> MetricSeverity {
        return enableColors ? sev : MetricSeverity::Normal;
    };

    // Row 0: Non-GPU Telemetry (CPU, RAM, NET)
    std::vector<FormattedSegment> row0;
    bool row0HasBlock = false;

    auto row0AppendSeparator = [&]() {
        if (row0HasBlock) {
            row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        row0.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
    };

    if (showCpu) {
        row0AppendSeparator();
        row0.push_back({ L" CPU ", MetricSeverity::Normal, SegmentType::Label, false, false });
        MetricSeverity cpuSev = applySeverity(GetSeverity(metrics.cpu.utilizationPercent, 80.0, 90.0));
        row0.push_back({ std::to_wstring(static_cast<int>(std::round(metrics.cpu.utilizationPercent))), cpuSev, SegmentType::Number, true, false });
        row0.push_back({ L"%", cpuSev, SegmentType::Unit, false, false });
        if (metrics.cpu.temperatureC.has_value()) {
            MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.cpu.temperatureC, 75.0, 85.0));
            row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            row0.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.temperatureC))), tempSev, SegmentType::Number, true, false });
            row0.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
        }
        if (metrics.cpu.powerWatts.has_value()) {
            row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            row0.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.powerWatts))), MetricSeverity::Normal, SegmentType::Number, true, false });
            row0.push_back({ L"W", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        if (metrics.cpu.fanRpm.has_value()) {
            row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            row0.push_back({ std::to_wstring(*metrics.cpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
            row0.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        row0HasBlock = true;
    }

    if (showRam) {
        row0AppendSeparator();
        row0.push_back({ L" RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            AppendMemorySegments(row0, metrics.memory.usedBytes, metrics.memory.totalBytes, ramSev, showMaxMemory);
        } else {
            row0.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        row0HasBlock = true;
    }

    if (showNet && (!hideOffline || metrics.network.isAvailable)) {
        row0AppendSeparator();
        row0.push_back({ L" NET ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.network.isAvailable) {
            std::wstring rxStr = FormatNetworkRate(metrics.network.rxBytesPerSec);
            row0.push_back({ rxStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            row0.push_back({ L"\x2193", MetricSeverity::Normal, SegmentType::Unit, false, false });
            row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            std::wstring txStr = FormatNetworkRate(metrics.network.txBytesPerSec);
            row0.push_back({ txStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            row0.push_back({ L"\x2191", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            row0.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        row0HasBlock = true;
    }

    if (row0HasBlock) {
        row0.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        row0.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
        rows.push_back(std::move(row0));
    }

    // Row 1..N: GPU Telemetry (wrapped by maxGpusPerRow)
    if (showGpu) {
        bool hasAvailableGpu = metrics.gpu.isAvailable;
        for (const auto& g : metrics.gpus) {
            if (g.isAvailable) { hasAvailableGpu = true; break; }
        }

        if (!hideOffline || hasAvailableGpu) {
            struct ActiveGpuEntry {
                GpuMetrics metrics;
                PerGpuConfig cfg;
                std::wstring label;
            };
            std::vector<ActiveGpuEntry> activeGpus;
            const auto& rawGpus = metrics.gpus.empty()
                ? std::vector<GpuMetrics>{ metrics.gpu }
                : metrics.gpus;

            for (size_t g = 0; g < rawGpus.size(); ++g) {
                uint32_t targetIdx = (g < config->targetGpuIndices.size() && !config->targetAllGpus)
                    ? config->targetGpuIndices[g]
                    : static_cast<uint32_t>(g);
                PerGpuConfig gCfg = config->GetGpuConfig(targetIdx);
                if (!gCfg.enabled) continue;
                std::wstring label = !gCfg.label.empty()
                    ? Utf8ToWide(gCfg.label)
                    : (rawGpus.size() > 1 ? (L"GPU" + std::to_wstring(g)) : L"GPU");
                activeGpus.push_back({ rawGpus[g], gCfg, label });
            }

            size_t perRow = (config->maxGpusPerRow > 0) ? static_cast<size_t>(config->maxGpusPerRow) : 2;

            for (size_t start = 0; start < activeGpus.size(); start += perRow) {
                std::vector<FormattedSegment> gpuRow;
                bool gpuRowHasBlock = false;

                auto gpuRowAppendSeparator = [&]() {
                    if (gpuRowHasBlock) {
                        gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    }
                    gpuRow.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
                };

                size_t end = (std::min)(start + perRow, activeGpus.size());
                for (size_t g = start; g < end; ++g) {
                    const auto& gpu = activeGpus[g].metrics;
                    const auto& gCfg = activeGpus[g].cfg;
                    const auto& label = activeGpus[g].label;

                    gpuRowAppendSeparator();
                    gpuRow.push_back({ L" " + label + L" ", MetricSeverity::Normal, SegmentType::Label, false, false });
                    if (gpu.isAvailable) {
                        bool hasPrintedData = false;

                        if (gCfg.showUtil && gpu.utilizationPercent.has_value()) {
                            MetricSeverity gpuSev = applySeverity(GetSeverity(*gpu.utilizationPercent, 80.0, 90.0));
                            gpuRow.push_back({ std::to_wstring(static_cast<int>(std::round(*gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
                            gpuRow.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });
                            hasPrintedData = true;
                        }

                        if (gCfg.showVram && gpu.memoryTotalBytes > 0) {
                            uint64_t totalBytes = gpu.memoryTotalBytes;
                            if (config->vramMode == VramMode::Usable && gpu.memoryReservedBytes > 0 && totalBytes > gpu.memoryReservedBytes) {
                                totalBytes -= gpu.memoryReservedBytes;
                            }
                            double vramPct = (static_cast<double>(gpu.memoryUsedBytes) / totalBytes) * 100.0;
                            MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
                            if (hasPrintedData) gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                            AppendMemorySegments(gpuRow, gpu.memoryUsedBytes, totalBytes, vramSev, showMaxMemory);
                            hasPrintedData = true;
                        }

                        if (gCfg.showTemp && gpu.temperatureC.has_value()) {
                            double critLimit = gpu.tempSlowdownLimitC.has_value() 
                                ? static_cast<double>(*gpu.tempSlowdownLimitC - 2) 
                                : 83.0;
                            if (critLimit > 83.0) critLimit = 83.0;
                            double highLimit = critLimit - 8.0;
                            MetricSeverity tempSev = applySeverity(GetSeverity(*gpu.temperatureC, highLimit, critLimit));
                            if (hasPrintedData) gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                            gpuRow.push_back({ std::to_wstring(*gpu.temperatureC), tempSev, SegmentType::Number, true, false });
                            gpuRow.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
                            hasPrintedData = true;
                        }

                        if (gCfg.showPower && gpu.powerWatts.has_value()) {
                            MetricSeverity powerSev = MetricSeverity::Normal;
                            if (gpu.powerLimitWatts.has_value() && *gpu.powerLimitWatts > 0) {
                                double powerPct = (static_cast<double>(*gpu.powerWatts) / *gpu.powerLimitWatts) * 100.0;
                                powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
                            }
                            if (hasPrintedData) gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                            gpuRow.push_back({ std::to_wstring(*gpu.powerWatts), powerSev, SegmentType::Number, true, false });
                            gpuRow.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
                            hasPrintedData = true;
                        }

                        if (gCfg.showFan) {
                            if (gpu.fanRpm.has_value()) {
                                if (hasPrintedData) gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                                gpuRow.push_back({ std::to_wstring(*gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                                gpuRow.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
                            } else if (gpu.fanSpeedPercent.has_value()) {
                                MetricSeverity fanSev = applySeverity(GetSeverity(*gpu.fanSpeedPercent, 80.0, 95.0));
                                if (hasPrintedData) gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                                gpuRow.push_back({ std::to_wstring(*gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
                                gpuRow.push_back({ L"%", fanSev, SegmentType::Unit, false, false });
                            }
                        }

                        if (!hasPrintedData) {
                            gpuRow.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
                        }
                    } else {
                        gpuRow.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
                    }

                    gpuRowHasBlock = true;
                }

                if (gpuRowHasBlock) {
                    gpuRow.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    gpuRow.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
                    rows.push_back(std::move(gpuRow));
                }
            }
        }
    }

    return rows;
}

std::vector<FormattedSegment> Formatter::FormatCell(const std::string& rawToken, const SystemMetrics& metrics, const AppConfig* config) {
    std::string token = rawToken;
    size_t first = token.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = token.find_last_not_of(" \t\r\n");
    token = token.substr(first, last - first + 1);
    for (char& c : token) c = static_cast<char>(tolower(c));

    if (token.empty() || token == "." || token == "null" || token == "empty" || token == "-") {
        return {};
    }

    bool showMaxMemory = !config || config->showMaxMemory;
    bool enableColors = !config || config->enableDynamicColors;
    bool hideOffline = config && config->hideOfflineMetrics;

    auto applySeverity = [enableColors](MetricSeverity sev) -> MetricSeverity {
        return enableColors ? sev : MetricSeverity::Normal;
    };

    // CPU block
    if (token == "cpu") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"CPU ", MetricSeverity::Normal, SegmentType::Label, false, false });
        MetricSeverity cpuSev = applySeverity(GetSeverity(metrics.cpu.utilizationPercent, 80.0, 90.0));
        segs.push_back({ std::to_wstring(static_cast<int>(std::round(metrics.cpu.utilizationPercent))), cpuSev, SegmentType::Number, true, false });
        segs.push_back({ L"%", cpuSev, SegmentType::Unit, false, false });

        if (metrics.cpu.temperatureC.has_value()) {
            MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.cpu.temperatureC, 75.0, 85.0));
            segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.temperatureC))), tempSev, SegmentType::Number, true, false });
            segs.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
        }
        if (metrics.cpu.powerWatts.has_value()) {
            segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.powerWatts))), MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"W", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        if (metrics.cpu.fanRpm.has_value()) {
            segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ std::to_wstring(*metrics.cpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        return segs;
    }

    if (token == "cpu_util") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"CPU ", MetricSeverity::Normal, SegmentType::Label, false, false });
        MetricSeverity cpuSev = applySeverity(GetSeverity(metrics.cpu.utilizationPercent, 80.0, 90.0));
        segs.push_back({ std::to_wstring(static_cast<int>(std::round(metrics.cpu.utilizationPercent))), cpuSev, SegmentType::Number, true, false });
        segs.push_back({ L"%", cpuSev, SegmentType::Unit, false, false });
        return segs;
    }

    if (token == "cpu_temp") {
        std::vector<FormattedSegment> segs;
        if (metrics.cpu.temperatureC.has_value()) {
            MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.cpu.temperatureC, 75.0, 85.0));
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.temperatureC))), tempSev, SegmentType::Number, true, false });
            segs.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "cpu_power") {
        std::vector<FormattedSegment> segs;
        if (metrics.cpu.powerWatts.has_value()) {
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.cpu.powerWatts))), MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"W", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "cpu_fan") {
        std::vector<FormattedSegment> segs;
        if (metrics.cpu.fanRpm.has_value()) {
            segs.push_back({ std::to_wstring(*metrics.cpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    // RAM block
    if (token == "ram") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            AppendMemorySegments(segs, metrics.memory.usedBytes, metrics.memory.totalBytes, ramSev, showMaxMemory);
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "ram_used") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            AppendMemorySegments(segs, metrics.memory.usedBytes, metrics.memory.totalBytes, ramSev, false);
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "ram_pct") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(ramPct))), ramSev, SegmentType::Number, true, false });
            segs.push_back({ L"%", ramSev, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    // Network block
    if (token == "net") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"NET ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.network.isAvailable) {
            std::wstring rxStr = FormatNetworkRate(metrics.network.rxBytesPerSec);
            segs.push_back({ rxStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"\x2193", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            std::wstring txStr = FormatNetworkRate(metrics.network.txBytesPerSec);
            segs.push_back({ txStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"\x2191", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "net_rx") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"NET ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.network.isAvailable) {
            std::wstring rxStr = FormatNetworkRate(metrics.network.rxBytesPerSec);
            segs.push_back({ rxStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"\x2193", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    if (token == "net_tx") {
        std::vector<FormattedSegment> segs;
        segs.push_back({ L"NET ", MetricSeverity::Normal, SegmentType::Label, false, false });
        if (metrics.network.isAvailable) {
            std::wstring txStr = FormatNetworkRate(metrics.network.txBytesPerSec);
            segs.push_back({ txStr, MetricSeverity::Normal, SegmentType::Number, true, false });
            segs.push_back({ L"\x2191", MetricSeverity::Normal, SegmentType::Unit, false, false });
        } else {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    }

    auto formatSingleGpu = [&](uint32_t gpuIdx, const std::string& subMetric) -> std::vector<FormattedSegment> {
        const GpuMetrics* pGpu = nullptr;
        if (gpuIdx < metrics.gpus.size()) {
            pGpu = &metrics.gpus[gpuIdx];
        } else if (gpuIdx == 0 && metrics.gpu.isAvailable) {
            pGpu = &metrics.gpu;
        }

        uint32_t targetIdx = (config && gpuIdx < config->targetGpuIndices.size() && !config->targetAllGpus)
            ? config->targetGpuIndices[gpuIdx]
            : gpuIdx;
        PerGpuConfig gpuCfg = config ? config->GetGpuConfig(targetIdx) : PerGpuConfig{};
        std::wstring label = !gpuCfg.label.empty() 
            ? Utf8ToWide(gpuCfg.label) 
            : (L"GPU" + (metrics.gpus.size() > 1 || gpuIdx > 0 ? std::to_wstring(gpuIdx) : L""));

        if (!pGpu || !pGpu->isAvailable) {
            if (hideOffline) return {};
            std::vector<FormattedSegment> segs;
            segs.push_back({ label + L" ", MetricSeverity::Normal, SegmentType::Label, false, false });
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            return segs;
        }

        const GpuMetrics& gpu = *pGpu;
        std::vector<FormattedSegment> segs;

        if (subMetric == "util") {
            segs.push_back({ label + L" ", MetricSeverity::Normal, SegmentType::Label, false, false });
            if (gpu.utilizationPercent.has_value()) {
                MetricSeverity gpuSev = applySeverity(GetSeverity(*gpu.utilizationPercent, 80.0, 90.0));
                segs.push_back({ std::to_wstring(static_cast<int>(std::round(*gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
                segs.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });
            } else {
                segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
            return segs;
        }

        if (subMetric == "vram") {
            if (gpu.memoryTotalBytes > 0) {
                uint64_t totalBytes = gpu.memoryTotalBytes;
                if (config && config->vramMode == VramMode::Usable && gpu.memoryReservedBytes > 0 && totalBytes > gpu.memoryReservedBytes) {
                    totalBytes -= gpu.memoryReservedBytes;
                }
                double vramPct = (static_cast<double>(gpu.memoryUsedBytes) / totalBytes) * 100.0;
                MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
                AppendMemorySegments(segs, gpu.memoryUsedBytes, totalBytes, vramSev, showMaxMemory);
            } else {
                segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
            return segs;
        }

        if (subMetric == "temp") {
            if (gpu.temperatureC.has_value()) {
                double critLimit = gpu.tempSlowdownLimitC.has_value() ? static_cast<double>(*gpu.tempSlowdownLimitC - 2) : 83.0;
                if (critLimit > 83.0) critLimit = 83.0;
                double highLimit = critLimit - 8.0;
                MetricSeverity tempSev = applySeverity(GetSeverity(*gpu.temperatureC, highLimit, critLimit));
                segs.push_back({ std::to_wstring(*gpu.temperatureC), tempSev, SegmentType::Number, true, false });
                segs.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
            } else {
                segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
            return segs;
        }

        if (subMetric == "power") {
            if (gpu.powerWatts.has_value()) {
                MetricSeverity powerSev = MetricSeverity::Normal;
                if (gpu.powerLimitWatts.has_value() && *gpu.powerLimitWatts > 0) {
                    double powerPct = (static_cast<double>(*gpu.powerWatts) / *gpu.powerLimitWatts) * 100.0;
                    powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
                }
                segs.push_back({ std::to_wstring(*gpu.powerWatts), powerSev, SegmentType::Number, true, false });
                segs.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
            } else {
                segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
            return segs;
        }

        if (subMetric == "fan") {
            if (gpu.fanRpm.has_value()) {
                segs.push_back({ std::to_wstring(*gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                segs.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
            } else if (gpu.fanSpeedPercent.has_value()) {
                MetricSeverity fanSev = applySeverity(GetSeverity(*gpu.fanSpeedPercent, 80.0, 95.0));
                segs.push_back({ std::to_wstring(*gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
                segs.push_back({ L"%", fanSev, SegmentType::Unit, false, false });
            } else {
                segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
            }
            return segs;
        }

        // Full GPU block
        segs.push_back({ label + L" ", MetricSeverity::Normal, SegmentType::Label, false, false });
        bool hasData = false;

        if (gpuCfg.showUtil && gpu.utilizationPercent.has_value()) {
            MetricSeverity gpuSev = applySeverity(GetSeverity(*gpu.utilizationPercent, 80.0, 90.0));
            segs.push_back({ std::to_wstring(static_cast<int>(std::round(*gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
            segs.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });
            hasData = true;
        }

        if (gpuCfg.showVram && gpu.memoryTotalBytes > 0) {
            uint64_t totalBytes = gpu.memoryTotalBytes;
            if (config && config->vramMode == VramMode::Usable && gpu.memoryReservedBytes > 0 && totalBytes > gpu.memoryReservedBytes) {
                totalBytes -= gpu.memoryReservedBytes;
            }
            double vramPct = (static_cast<double>(gpu.memoryUsedBytes) / totalBytes) * 100.0;
            MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
            if (hasData) segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            AppendMemorySegments(segs, gpu.memoryUsedBytes, totalBytes, vramSev, showMaxMemory);
            hasData = true;
        }

        if (gpuCfg.showTemp && gpu.temperatureC.has_value()) {
            double critLimit = gpu.tempSlowdownLimitC.has_value() ? static_cast<double>(*gpu.tempSlowdownLimitC - 2) : 83.0;
            if (critLimit > 83.0) critLimit = 83.0;
            double highLimit = critLimit - 8.0;
            MetricSeverity tempSev = applySeverity(GetSeverity(*gpu.temperatureC, highLimit, critLimit));
            if (hasData) segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ std::to_wstring(*gpu.temperatureC), tempSev, SegmentType::Number, true, false });
            segs.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
            hasData = true;
        }

        if (gpuCfg.showPower && gpu.powerWatts.has_value()) {
            MetricSeverity powerSev = MetricSeverity::Normal;
            if (gpu.powerLimitWatts.has_value() && *gpu.powerLimitWatts > 0) {
                double powerPct = (static_cast<double>(*gpu.powerWatts) / *gpu.powerLimitWatts) * 100.0;
                powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
            }
            if (hasData) segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
            segs.push_back({ std::to_wstring(*gpu.powerWatts), powerSev, SegmentType::Number, true, false });
            segs.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
            hasData = true;
        }

        if (gpuCfg.showFan) {
            if (gpu.fanRpm.has_value()) {
                if (hasData) segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segs.push_back({ std::to_wstring(*gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                segs.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
            } else if (gpu.fanSpeedPercent.has_value()) {
                MetricSeverity fanSev = applySeverity(GetSeverity(*gpu.fanSpeedPercent, 80.0, 95.0));
                if (hasData) segs.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segs.push_back({ std::to_wstring(*gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
                segs.push_back({ L"%", fanSev, SegmentType::Unit, false, false });
            }
        }

        if (!hasData) {
            segs.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }
        return segs;
    };

    // GPU blocks & submetrics (gpu, gpu0, gpu1, gpu0_util, gpu0_vram, etc.)
    if (token.rfind("gpu", 0) == 0) {
        std::string rest = token.substr(3);

        // Case 1: Generic "gpu" or generic submetric "gpu_util", "gpu_vram", etc. with multiple GPUs present
        if ((rest.empty() || rest[0] == '_') && metrics.gpus.size() > 1) {
            std::string sub = rest.empty() ? "" : rest.substr(1);
            std::vector<FormattedSegment> combinedSegs;
            std::wstring sepStr = (config && !config->separator.empty()) ? config->separator : L"\x2502";
            bool hasAny = false;
            for (size_t g = 0; g < metrics.gpus.size(); ++g) {
                uint32_t targetIdx = (config && g < config->targetGpuIndices.size() && !config->targetAllGpus)
                    ? config->targetGpuIndices[g]
                    : static_cast<uint32_t>(g);
                PerGpuConfig gCfg = config ? config->GetGpuConfig(targetIdx) : PerGpuConfig{};
                if (!gCfg.enabled) continue;

                auto singleSegs = formatSingleGpu(static_cast<uint32_t>(g), sub);
                if (singleSegs.empty()) continue;

                if (hasAny) {
                    combinedSegs.push_back({ L" " + sepStr + L" ", MetricSeverity::Normal, SegmentType::Separator, false, true });
                }
                combinedSegs.insert(combinedSegs.end(), singleSegs.begin(), singleSegs.end());
                hasAny = true;
            }
            return combinedSegs;
        }

        // Case 2: Specific GPU index (gpu0, gpu1, gpu0_vram, etc.) or single GPU fallback
        uint32_t gpuIdx = 0;
        std::string subMetric = "";
        if (!rest.empty() && isdigit(static_cast<unsigned char>(rest[0]))) {
            size_t pos = 0;
            gpuIdx = std::stoul(rest, &pos);
            if (pos < rest.size() && rest[pos] == '_') {
                subMetric = rest.substr(pos + 1);
            }
        } else if (!rest.empty() && rest[0] == '_') {
            subMetric = rest.substr(1);
        }

        return formatSingleGpu(gpuIdx, subMetric);
    }

    // Explicit static text token (e.g. "text:SERVER" or "label:MAIN")
    if (token.rfind("text:", 0) == 0 || token.rfind("label:", 0) == 0) {
        size_t colon = rawToken.find(':');
        std::string customText = rawToken.substr(colon + 1);
        size_t f = customText.find_first_not_of(" \t\r\n");
        size_t l = customText.find_last_not_of(" \t\r\n");
        if (f != std::string::npos && l != std::string::npos) {
            customText = customText.substr(f, l - f + 1);
        }
        std::vector<FormattedSegment> segs;
        segs.push_back({ Utf8ToWide(customText), MetricSeverity::Normal, SegmentType::Label, false, false });
        return segs;
    }

    // Unrecognized metric token fallback (wrapped in brackets so user spots typos immediately)
    std::vector<FormattedSegment> segs;
    segs.push_back({ L"[" + Utf8ToWide(token) + L"]", MetricSeverity::Normal, SegmentType::Label, false, false });
    return segs;
}

std::wstring Formatter::Format(const SystemMetrics& metrics, const AppConfig* config) {
    auto rows = FormatRows(metrics, config);
    std::wstring result;
    for (size_t r = 0; r < rows.size(); ++r) {
        if (r > 0) result += L"\n";
        for (const auto& seg : rows[r]) {
            result += seg.text;
        }
    }
    return result;
}

std::string Formatter::FormatUtf8(const SystemMetrics& metrics, const AppConfig* config) {
    std::wstring wstr = Format(metrics, config);
    return WideToUtf8(wstr);
}

} // namespace BareStats
