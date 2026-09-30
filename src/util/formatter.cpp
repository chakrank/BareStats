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
    double usedGb = static_cast<double>(usedBytes) / (1024.0 * 1024.0 * 1024.0);
    double totalGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);

    std::wostringstream oss;
    oss << std::fixed << std::setprecision(1) << usedGb << L"/";
    
    // Check if total is integer-like (e.g. 16, 24, 32, 64)
    double rounded = std::round(totalGb);
    if (std::abs(totalGb - rounded) < 0.15) {
        oss << static_cast<uint32_t>(rounded) << L"GB";
    } else {
        oss << std::fixed << std::setprecision(1) << totalGb << L"GB";
    }

    return oss.str();
}

static void AppendMemorySegments(std::vector<FormattedSegment>& segments, uint64_t usedBytes, uint64_t totalBytes, MetricSeverity sev) {
    double usedGb = static_cast<double>(usedBytes) / (1024.0 * 1024.0 * 1024.0);
    double totalGb = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);

    std::wostringstream ossUsed;
    ossUsed << std::fixed << std::setprecision(1) << usedGb;
    segments.push_back({ ossUsed.str(), sev, SegmentType::Number, true, false }); // Used number: Prominent Bold

    segments.push_back({ L"/", sev, SegmentType::Unit, false, false });         // Slash: Normal

    std::wostringstream ossTotal;
    double rounded = std::round(totalGb);
    if (std::abs(totalGb - rounded) < 0.15) {
        ossTotal << static_cast<uint32_t>(rounded);
    } else {
        ossTotal << std::fixed << std::setprecision(1) << totalGb;
    }
    segments.push_back({ ossTotal.str(), sev, SegmentType::Number, true, false }); // Total number: Prominent Bold

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
    bool enableColors = !config || config->enableDynamicColors;

    auto applySeverity = [enableColors](MetricSeverity sev) -> MetricSeverity {
        return enableColors ? sev : MetricSeverity::Normal;
    };

    bool hasPreviousBlock = false;

    // CPU Block
    if (showCpu) {
        segments.push_back({ L"\x2502", MetricSeverity::Normal, SegmentType::Separator, false, true });
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
    }

    // GPU Block
    if (showGpu) {
        if (hasPreviousBlock) {
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        segments.push_back({ L"\x2502", MetricSeverity::Normal, SegmentType::Separator, false, true });
        segments.push_back({ L" GPU ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (metrics.gpu.isAvailable) {
            // GPU Util
            MetricSeverity gpuSev = applySeverity(GetSeverity(metrics.gpu.utilizationPercent, 80.0, 90.0));
            segments.push_back({ std::to_wstring(static_cast<int>(std::round(metrics.gpu.utilizationPercent))), gpuSev, SegmentType::Number, true, false });
            segments.push_back({ L"%", gpuSev, SegmentType::Unit, false, false });

            // VRAM
            if (metrics.gpu.memoryTotalBytes > 0) {
                double vramPct = (static_cast<double>(metrics.gpu.memoryUsedBytes) / metrics.gpu.memoryTotalBytes) * 100.0;
                MetricSeverity vramSev = applySeverity(GetSeverity(vramPct, 80.0, 90.0));
                segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                AppendMemorySegments(segments, metrics.gpu.memoryUsedBytes, metrics.gpu.memoryTotalBytes, vramSev);
            }

            // GPU Temp
            if (showGpuTemp && metrics.gpu.temperatureC.has_value()) {
                double critLimit = metrics.gpu.tempSlowdownLimitC.has_value() 
                    ? static_cast<double>(*metrics.gpu.tempSlowdownLimitC - 2) 
                    : 83.0;
                if (critLimit > 83.0) critLimit = 83.0;
                double highLimit = critLimit - 8.0;
                MetricSeverity tempSev = applySeverity(GetSeverity(*metrics.gpu.temperatureC, highLimit, critLimit));
                segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*metrics.gpu.temperatureC), tempSev, SegmentType::Number, true, false });
                segments.push_back({ L"\x00B0\x0043", tempSev, SegmentType::Unit, false, false });
            }

            // GPU Power
            if (showGpuPower && metrics.gpu.powerWatts.has_value()) {
                MetricSeverity powerSev = MetricSeverity::Normal;
                if (metrics.gpu.powerLimitWatts.has_value() && *metrics.gpu.powerLimitWatts > 0) {
                    double powerPct = (static_cast<double>(*metrics.gpu.powerWatts) / *metrics.gpu.powerLimitWatts) * 100.0;
                    powerSev = applySeverity(GetSeverity(powerPct, 85.0, 95.0));
                }
                segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                segments.push_back({ std::to_wstring(*metrics.gpu.powerWatts), powerSev, SegmentType::Number, true, false });
                segments.push_back({ L"W", powerSev, SegmentType::Unit, false, false });
            }

            // GPU Fan
            if (showGpuFan) {
                segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
                if (metrics.gpu.fanRpm.has_value()) {
                    segments.push_back({ std::to_wstring(*metrics.gpu.fanRpm), MetricSeverity::Normal, SegmentType::Number, true, false });
                    segments.push_back({ L"RPM", MetricSeverity::Normal, SegmentType::Unit, false, false });
                } else if (metrics.gpu.fanSpeedPercent.has_value()) {
                    MetricSeverity fanSev = applySeverity(GetSeverity(*metrics.gpu.fanSpeedPercent, 80.0, 95.0));
                    segments.push_back({ std::to_wstring(*metrics.gpu.fanSpeedPercent), fanSev, SegmentType::Number, true, false });
                    segments.push_back({ L"%", fanSev, SegmentType::Unit, false, false });
                }
            }
        } else {
            segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }

        hasPreviousBlock = true;
    }

    // RAM Block
    if (showRam) {
        if (hasPreviousBlock) {
            segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        }
        segments.push_back({ L"\x2502", MetricSeverity::Normal, SegmentType::Separator, false, true });
        segments.push_back({ L" RAM ", MetricSeverity::Normal, SegmentType::Label, false, false });

        if (metrics.memory.totalBytes > 0) {
            double ramPct = (static_cast<double>(metrics.memory.usedBytes) / metrics.memory.totalBytes) * 100.0;
            MetricSeverity ramSev = applySeverity(GetSeverity(ramPct, 80.0, 90.0));
            AppendMemorySegments(segments, metrics.memory.usedBytes, metrics.memory.totalBytes, ramSev);
        } else {
            segments.push_back({ L"N/A", MetricSeverity::Normal, SegmentType::Unit, true, false });
        }

        hasPreviousBlock = true;
    }

    if (hasPreviousBlock) {
        segments.push_back({ L" ", MetricSeverity::Normal, SegmentType::Unit, false, false });
        segments.push_back({ L"\x2502", MetricSeverity::Normal, SegmentType::Separator, false, true });
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
