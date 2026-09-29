#include "formatter.h"
#include "../config/config.h"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <windows.h>

namespace BareStats {

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

std::wstring Formatter::Format(const SystemMetrics& metrics, const AppConfig* config) {
    bool showCpu = !config || config->showCpu;
    bool showGpu = !config || config->showGpu;
    bool showRam = !config || config->showRam;
    bool showGpuTemp = !config || config->showGpuTemp;
    bool showGpuPower = !config || config->showGpuPower;
    bool showGpuFan = !config || config->showGpuFan;

    std::wostringstream oss;
    bool hasPreviousBlock = false;

    // CPU Block
    if (showCpu) {
        oss << L"\x2502 CPU " << static_cast<int>(std::round(metrics.cpu.utilizationPercent)) << L"%";
        if (metrics.cpu.temperatureC.has_value()) {
            oss << L" " << static_cast<int>(std::round(*metrics.cpu.temperatureC)) << L"\x00B0\x0043";
        }
        if (metrics.cpu.powerWatts.has_value()) {
            oss << L" " << static_cast<int>(std::round(*metrics.cpu.powerWatts)) << L"W";
        }
        if (metrics.cpu.fanRpm.has_value()) {
            oss << L" " << *metrics.cpu.fanRpm << L"RPM";
        }
        hasPreviousBlock = true;
    }

    // GPU Block
    if (showGpu) {
        if (hasPreviousBlock) oss << L" ";
        oss << L"\x2502 GPU ";
        if (metrics.gpu.isAvailable) {
            oss << static_cast<int>(std::round(metrics.gpu.utilizationPercent)) << L"%";
            
            if (metrics.gpu.memoryTotalBytes > 0) {
                oss << L" " << FormatBytesGb(metrics.gpu.memoryUsedBytes, metrics.gpu.memoryTotalBytes);
            }

            if (showGpuTemp && metrics.gpu.temperatureC.has_value()) {
                oss << L" " << *metrics.gpu.temperatureC << L"\x00B0\x0043";
            }

            if (showGpuPower && metrics.gpu.powerWatts.has_value()) {
                oss << L" " << *metrics.gpu.powerWatts << L"W";
            }

            if (showGpuFan) {
                if (metrics.gpu.fanRpm.has_value()) {
                    oss << L" " << *metrics.gpu.fanRpm << L"RPM";
                } else if (metrics.gpu.fanSpeedPercent.has_value()) {
                    oss << L" " << *metrics.gpu.fanSpeedPercent << L"%";
                }
            }
        } else {
            oss << L"N/A";
        }
        hasPreviousBlock = true;
    }

    // RAM Block
    if (showRam) {
        if (hasPreviousBlock) oss << L" ";
        oss << L"\x2502 RAM ";
        if (metrics.memory.totalBytes > 0) {
            oss << FormatBytesGb(metrics.memory.usedBytes, metrics.memory.totalBytes);
        } else {
            oss << L"N/A";
        }
        hasPreviousBlock = true;
    }

    if (hasPreviousBlock) {
        oss << L" \x2502";
    }

    return oss.str();
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
