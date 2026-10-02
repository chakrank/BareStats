#include "formatter.h"
#include "../config/config.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <windows.h>

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
    bool showGpuTemp = !config || config->showGpuTemp;
    bool showGpuPower = !config || config->showGpuPower;
    bool showGpuFan = !config || config->showGpuFan;
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

    auto appendGpu = [&]() {
        appendSeparator();
        segments.push_back({ L" GPU ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (metrics.gpu.isAvailable) {
            bool hasPrintedData = false;

            // GPU Util (only render if known - never output false 0%)
            if (metrics.gpu.utilizationPercent.has_value()) {
                MetricSeverity gpuSev = applySeverity(GetSeverity(*metrics.gpu.utilizationPercent, 80.0, 90.0));
                segments.push_back({ std::to_wstring(static_cast<int>(std::round(*metrics.gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
                segments.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // VRAM
            if (metrics.gpu.memoryTotalBytes > 0) {
                double vramPct = (static_cast<double>(metrics.gpu.memoryUsedBytes) / metrics.gpu.memoryTotalBytes) * 100.0;
                MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                AppendMemorySegments(segments, metrics.gpu.memoryUsedBytes, metrics.gpu.memoryTotalBytes, vramSev, showMaxMemory);
                hasPrintedData = true;
            }

            // GPU Temp
            if (showGpuTemp && metrics.gpu.temperatureC.has_value()) {
                double critLimit = metrics.gpu.tempSlowdownLimitC.has_value() 
                    ? static_cast<double>(*metrics.gpu.tempSlowdownLimitC - 2) 
                    : 83.0;
                if (critLimit > 83.0) critLimit = 83.0;
                double highLimit = critLimit - 8.0;
                MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.gpu.temperatureC, highLimit, critLimit));
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*metrics.gpu.temperatureC), tempSev, SegmentType::Number, true, false });
                segments.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // GPU Power
            if (showGpuPower && metrics.gpu.powerWatts.has_value()) {
                MetricSeverity powerSev = MetricSeverity::Normal;
                if (metrics.gpu.powerLimitWatts.has_value() && *metrics.gpu.powerLimitWatts > 0) {
                    double powerPct = (static_cast<double>(*metrics.gpu.powerWatts) / *metrics.gpu.powerLimitWatts) * 100.0;
                    powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
                }
                if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*metrics.gpu.powerWatts), powerSev, SegmentType::Number, true, false });
                segments.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
                hasPrintedData = true;
            }

            // GPU Fan
            if (showGpuFan) {
                if (metrics.gpu.fanRpm.has_value()) {
                    if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    segments.push_back({ std::to_wstring(*metrics.gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                    segments.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
                } else if (metrics.gpu.fanSpeedPercent.has_value()) {
                    MetricSeverity fanSev = applySeverity(GetSeverity(*metrics.gpu.fanSpeedPercent, 80.0, 95.0));
                    if (hasPrintedData) segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                    segments.push_back({ std::to_wstring(*metrics.gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
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

    std::vector<std::string> defaultOrder = { "CPU", "GPU", "RAM" };
    const std::vector<std::string>& activeOrder = (config && !config->metricOrder.empty()) ? config->metricOrder : defaultOrder;

    for (const auto& item : activeOrder) {
        if (item == "CPU" && showCpu) {
            appendCpu();
        } else if (item == "GPU" && showGpu && (!hideOffline || metrics.gpu.isAvailable)) {
            appendGpu();
        } else if (item == "RAM" && showRam) {
            appendRam();
        }
    }

    if (hasPreviousBlock) {
        segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        segments.push_back({ sepStr, MetricSeverity::Normal, SegmentType::Separator, false, true });
    }

    return segments;
}

std::wstring Formatter::Format(const SystemMetrics& metrics, const AppConfig* config) {
    auto segments = FormatSegments(metrics, config);
    std::wstring result;
    for (const auto& seg : segments) {
        result += seg.text;
    }
    return result;
}

std::string Formatter::FormatUtf8(const SystemMetrics& metrics, const AppConfig* config) {
    std::wstring wstr = Format(metrics, config);
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return "";
    std::string str(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, nullptr, nullptr);
    return str;
}

} // namespace BareStats
