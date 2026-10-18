#include "test_common.h"
#include "../src/metrics/linux/linux_proc_collector.h"
#include "../src/metrics/linux/linux_sysfs_gpu_collector.h"

// 1. Linux /proc/stat CPU Collector Synthetic Time-Slicing
bool Test_LinuxProc_CpuCollector_Synthetic() {
    std::string statPath = GetTestModuleDir() + "test_proc_stat";
    std::error_code ec;

    {
        std::ofstream out(statPath);
        out << "cpu  1000 0 200 4800 0 0 0 0 0 0\n"
            << "cpu0 500 0 100 2400 0 0 0 0 0 0\n";
    }

    BareStats::LinuxProcCpuCollector collector(statPath);
    auto m1 = collector.Collect();
    TEST_ASSERT(m1.utilizationPercent == 0.0, "First sample baseline must return 0.0% utilization");

    {
        std::ofstream out(statPath);
        out << "cpu  1150 0 250 4800 0 0 0 0 0 0\n";
    }

    auto m2 = collector.Collect();
    TEST_ASSERT(std::abs(m2.utilizationPercent - 100.0) < 0.1, "100% active delta must yield 100.0% utilization");

    {
        std::ofstream out(statPath);
        out << "cpu  1200 0 250 4950 0 0 0 0 0 0\n";
    }

    auto m3 = collector.Collect();
    TEST_ASSERT(std::abs(m3.utilizationPercent - 25.0) < 0.1, "50 active / 200 total ticks must yield 25.0% utilization");

    std::filesystem::remove(statPath, ec);
    return true;
}

// 2. Linux /proc/meminfo Memory Collector Synthetic Parsing
bool Test_LinuxProc_MemoryCollector_Synthetic() {
    std::string meminfoPath = GetTestModuleDir() + "test_proc_meminfo";
    std::error_code ec;

    {
        std::ofstream out(meminfoPath);
        out << "MemTotal:       32800000 kB\n"
            << "MemFree:         8000000 kB\n"
            << "MemAvailable:   16400000 kB\n"
            << "Buffers:          400000 kB\n"
            << "Cached:          8000000 kB\n";
    }

    BareStats::LinuxProcMemoryCollector collector(meminfoPath);
    auto m = collector.Collect();

    TEST_ASSERT(m.totalBytes == 32800000ULL * 1024ULL, "Total memory must equal 32800000 kB * 1024");
    TEST_ASSERT(m.usedBytes == 16400000ULL * 1024ULL, "Used memory must equal (MemTotal - MemAvailable) * 1024");

    std::filesystem::remove(meminfoPath, ec);
    return true;
}

// 3. Linux /proc/net Gateway Detection and Network Throughput
bool Test_LinuxProc_NetworkCollector_Synthetic() {
    std::string routePath = GetTestModuleDir() + "test_proc_route";
    std::string devPath = GetTestModuleDir() + "test_proc_dev";
    std::error_code ec;

    {
        std::ofstream out(routePath);
        out << "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
            << "docker0\t00000000\t0101A8C0\t0003\t0\t0\t10\t00000000\t0\t0\t0\n"
            << "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
            << "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n";
    }

    std::string detected = BareStats::LinuxProcNetworkCollector::DetectDefaultGatewayInterface(routePath);
    TEST_ASSERT(detected == "eth0", "DetectDefaultGatewayInterface must ignore docker0 and select eth0");

    {
        std::ofstream out(devPath);
        out << "Inter-|   Receive                                                |  Transmit\n"
            << " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
            << "    lo: 1000000       0    0    0    0     0          0         0  1000000       0    0    0     0       0          0\n"
            << "  eth0: 50000000      0    0    0    0     0          0         0 20000000      0    0    0     0       0          0\n";
    }

    BareStats::LinuxProcNetworkCollector collector(devPath, routePath);
    auto m1 = collector.Collect();
    TEST_ASSERT(m1.isAvailable, "Network metrics must be available");
    TEST_ASSERT(m1.primaryAdapterName == "eth0", "Primary adapter name must be eth0");
    TEST_ASSERT(m1.primaryAdapter == L"eth0", "Primary adapter wide string must match eth0");
    TEST_ASSERT(m1.totalRxBytes == 50000000ULL, "Total RX bytes mismatch");
    TEST_ASSERT(m1.totalTxBytes == 20000000ULL, "Total TX bytes mismatch");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::ofstream out(devPath);
        out << "Inter-|   Receive                                                |  Transmit\n"
            << " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
            << "  eth0: 60000000      0    0    0    0     0          0         0 25000000      0    0    0     0       0          0\n";
    }

    auto m2 = collector.Collect();
    TEST_ASSERT(m2.rxBytesPerSec > 0, "RX rate delta must be non-zero");
    TEST_ASSERT(m2.txBytesPerSec > 0, "TX rate delta must be non-zero");
    TEST_ASSERT(m2.totalRxBytes == 60000000ULL, "Total RX bytes updated");

    std::filesystem::remove(routePath, ec);
    std::filesystem::remove(devPath, ec);
    return true;
}

// 4. Linux Sysfs AMD Radeon GPU Collector Synthetic Parsing
bool Test_LinuxSysfs_AmdGpu_Synthetic() {
    std::string mockDrm = GetTestModuleDir() + "mock_sysfs_drm_amd";
    std::error_code ec;
    std::filesystem::remove_all(mockDrm, ec);

    std::string card0 = mockDrm + "/card0";
    std::string devDir = card0 + "/device";
    std::string hwmonDir = devDir + "/hwmon/hwmon0";
    std::filesystem::create_directories(hwmonDir, ec);

    {
        std::ofstream(devDir + "/vendor") << "0x1002\n";
        std::ofstream(devDir + "/device") << "0x73df\n";
        std::ofstream(devDir + "/gpu_busy_percent") << "85\n";
        std::ofstream(devDir + "/mem_info_vram_total") << "17179869184\n";
        std::ofstream(devDir + "/mem_info_vram_used") << "8589934592\n";

        std::ofstream(hwmonDir + "/temp1_input") << "67000\n";
        std::ofstream(hwmonDir + "/temp1_crit") << "105000\n";
        std::ofstream(hwmonDir + "/power1_average") << "195000000\n";
        std::ofstream(hwmonDir + "/power1_cap") << "250000000\n";
        std::ofstream(hwmonDir + "/fan1_input") << "1520\n";
        std::ofstream(hwmonDir + "/pwm1") << "128\n";
    }

    BareStats::LinuxSysfsGpuCollector collector(mockDrm);
    BareStats::GpuMetrics m;
    bool ok = collector.Collect(0, m);

    TEST_ASSERT(ok, "Collector must successfully collect AMD GPU metrics");
    TEST_ASSERT(m.isAvailable, "AMD GPU must be reported as available");
    TEST_ASSERT(m.name.find("AMD") != std::string::npos, "GPU name must identify as AMD");
    TEST_ASSERT(m.utilizationPercent.has_value() && std::abs(*m.utilizationPercent - 85.0) < 0.01,
                "Utilization must match gpu_busy_percent (85%)");
    TEST_ASSERT(m.memoryTotalBytes == 17179869184ULL, "Total VRAM must match mem_info_vram_total (16 GB)");
    TEST_ASSERT(m.memoryUsedBytes == 8589934592ULL, "Used VRAM must match mem_info_vram_used (8 GB)");
    TEST_ASSERT(m.temperatureC.has_value() && *m.temperatureC == 67, "Temperature must be 67 C (from 67000 mC)");
    TEST_ASSERT(m.tempSlowdownLimitC.has_value() && *m.tempSlowdownLimitC == 105, "Critical temperature must be 105 C");
    TEST_ASSERT(m.powerWatts.has_value() && *m.powerWatts == 195, "Power must be 195 W (from 195000000 uW)");
    TEST_ASSERT(m.powerLimitWatts.has_value() && *m.powerLimitWatts == 250, "Power limit must be 250 W");
    TEST_ASSERT(m.fanRpm.has_value() && *m.fanRpm == 1520, "Fan speed must be 1520 RPM");
    TEST_ASSERT(m.fanSpeedPercent.has_value() && *m.fanSpeedPercent == 50, "PWM 128/255 must map to 50%");

    std::filesystem::remove_all(mockDrm, ec);
    return true;
}

// 5. Linux Sysfs Intel GPU Collector Synthetic Parsing
bool Test_LinuxSysfs_IntelGpu_Synthetic() {
    std::string mockDrm = GetTestModuleDir() + "mock_sysfs_drm_intel";
    std::error_code ec;
    std::filesystem::remove_all(mockDrm, ec);

    std::string card0 = mockDrm + "/card0";
    std::string devDir = card0 + "/device";
    std::string hwmonDir = devDir + "/hwmon/hwmon1";
    std::filesystem::create_directories(hwmonDir, ec);

    {
        std::ofstream(devDir + "/vendor") << "0x8086\n";
        std::ofstream(devDir + "/device") << "0x4680\n";
        std::ofstream(devDir + "/lmem_total_bytes") << "8589934592\n";
        std::ofstream(card0 + "/gt_act_freq_mhz") << "1600\n";
        std::ofstream(card0 + "/gt_max_freq_mhz") << "2000\n";

        std::ofstream(hwmonDir + "/temp1_input") << "58000\n";
        std::ofstream(hwmonDir + "/power1_input") << "45000000\n";
    }

    BareStats::LinuxSysfsGpuCollector collector(mockDrm);
    BareStats::GpuMetrics m;
    bool ok = collector.Collect(0, m);

    TEST_ASSERT(ok, "Collector must successfully collect Intel GPU metrics");
    TEST_ASSERT(m.isAvailable, "Intel GPU must be reported as available");
    TEST_ASSERT(m.name.find("Intel") != std::string::npos, "GPU name must identify as Intel");
    TEST_ASSERT(m.utilizationPercent.has_value() && std::abs(*m.utilizationPercent - 80.0) < 0.01,
                "Utilization must match 1600 / 2000 * 100 = 80.0%");
    TEST_ASSERT(m.memoryTotalBytes == 8589934592ULL, "Total VRAM must match lmem_total_bytes");
    TEST_ASSERT(m.temperatureC.has_value() && *m.temperatureC == 58, "Temperature must be 58 C");
    TEST_ASSERT(m.powerWatts.has_value() && *m.powerWatts == 45, "Power must be 45 W");

    std::filesystem::remove_all(mockDrm, ec);
    return true;
}

// 6. Linux Sysfs Multi-Card Enumeration Synthetic
bool Test_LinuxSysfs_MultiCardEnumeration_Synthetic() {
    std::string mockDrm = GetTestModuleDir() + "mock_sysfs_drm_multi";
    std::error_code ec;
    std::filesystem::remove_all(mockDrm, ec);

    std::string card0 = mockDrm + "/card0/device";
    std::string card1 = mockDrm + "/card1/device";
    std::string cardNonCard = mockDrm + "/card0-DP-1";
    std::filesystem::create_directories(card0, ec);
    std::filesystem::create_directories(card1, ec);
    std::filesystem::create_directories(cardNonCard, ec);

    {
        std::ofstream(card0 + "/vendor") << "0x1002\n";
        std::ofstream(card0 + "/device") << "0x73df\n";
        std::ofstream(card0 + "/label") << "AMD Radeon RX 7900 XTX\n";

        std::ofstream(card1 + "/vendor") << "0x8086\n";
        std::ofstream(card1 + "/device") << "0x4680\n";
        std::ofstream(card1 + "/label") << "Intel Arc A770\n";
    }

    BareStats::LinuxSysfsGpuCollector collector(mockDrm);
    auto cards = collector.EnumerateCards();

    TEST_ASSERT(cards.size() == 2, "EnumerateCards must find exactly 2 DRM cards, ignoring connectors");
    TEST_ASSERT(cards[0].index == 0, "Card 0 index must be 0");
    TEST_ASSERT(cards[0].isAmd, "Card 0 must be identified as AMD");
    TEST_ASSERT(cards[0].name == "AMD Radeon RX 7900 XTX", "Card 0 custom label must be read");

    TEST_ASSERT(cards[1].index == 1, "Card 1 index must be 1");
    TEST_ASSERT(cards[1].isIntel, "Card 1 must be identified as Intel");
    TEST_ASSERT(cards[1].name == "Intel Arc A770", "Card 1 custom label must be read");

    std::filesystem::remove_all(mockDrm, ec);
    return true;
}

// 7. Linux /proc Truncated and Corrupted Files Resilience
bool Test_LinuxProc_TruncatedAndCorrupted_Resilience() {
    std::string testStat = GetTestModuleDir() + "test_proc_stat_corrupt";
    std::string testMem = GetTestModuleDir() + "test_proc_mem_corrupt";
    std::string testNetDev = GetTestModuleDir() + "test_proc_netdev_corrupt";
    std::string testNetRoute = GetTestModuleDir() + "test_proc_route_corrupt";
    std::error_code ec;

    // 1. Truncated and corrupted /proc/stat
    {
        std::ofstream out(testStat);
        out << "cpu  incomplete\n";
    }
    BareStats::LinuxProcCpuCollector cpuColl(testStat);
    auto cpuM = cpuColl.Collect();
    TEST_ASSERT(cpuM.utilizationPercent == 0.0, "Corrupt /proc/stat must yield safe 0.0% utilization");

    // Overwrite with empty file
    {
        std::ofstream out(testStat, std::ios::trunc);
    }
    cpuColl.Reset();
    cpuM = cpuColl.Collect();
    TEST_ASSERT(cpuM.utilizationPercent == 0.0, "Empty /proc/stat must not crash and yield 0.0%");

    // 2. Corrupted /proc/meminfo
    {
        std::ofstream out(testMem);
        out << "MemTotal:       invalid_text kB\n"
            << "GarbageLine\n"
            << "MemAvailable:   more_garbage\n";
    }
    BareStats::LinuxProcMemoryCollector memColl(testMem);
    auto memM = memColl.Collect();
    TEST_ASSERT(memM.totalBytes == 0, "Corrupt /proc/meminfo must yield 0 bytes total without crash");

    // 3. Corrupted /proc/net/dev and route
    {
        std::ofstream out(testNetDev);
        out << "Corrupt Header\n"
            << "bad_entry_with_no_colon\n"
            << "eth0: not_numbers_here\n";
    }
    {
        std::ofstream out(testNetRoute);
        out << "Iface\tDestination\n"
            << "garbage line without enough columns\n";
    }
    BareStats::LinuxProcNetworkCollector netColl(testNetDev, testNetRoute);
    auto netM = netColl.Collect();
    TEST_ASSERT(netM.primaryAdapterName.empty() || !netM.isAvailable,
                "Corrupted net files must fail safely without crash");

    std::filesystem::remove(testStat, ec);
    std::filesystem::remove(testMem, ec);
    std::filesystem::remove(testNetDev, ec);
    std::filesystem::remove(testNetRoute, ec);
    return true;
}

// 8. Linux Sysfs Missing Hwmon and Multi-Gateway Resilience
bool Test_LinuxSysfs_MissingHwmonAndMultiGateway_Resilience() {
    std::string mockDrm = GetTestModuleDir() + "mock_sysfs_missing_hwmon";
    std::string testRoute = GetTestModuleDir() + "test_proc_route_multigateway";
    std::error_code ec;
    std::filesystem::remove_all(mockDrm, ec);

    // 1. AMD GPU with NO hwmon directory (e.g. headless node, compute VM, missing sensor driver)
    std::string card0 = mockDrm + "/card0";
    std::string devDir = card0 + "/device";
    std::filesystem::create_directories(devDir, ec);

    {
        std::ofstream(devDir + "/vendor") << "0x1002\n";
        std::ofstream(devDir + "/device") << "0x73df\n";
        std::ofstream(devDir + "/gpu_busy_percent") << "72\n";
        std::ofstream(devDir + "/mem_info_vram_total") << "8589934592\n";
        std::ofstream(devDir + "/mem_info_vram_used") << "2147483648\n";
    }

    BareStats::LinuxSysfsGpuCollector collector(mockDrm);
    BareStats::GpuMetrics m;
    bool ok = collector.Collect(0, m);

    TEST_ASSERT(ok, "Collector must succeed even if hwmon sensors are absent");
    TEST_ASSERT(m.isAvailable, "GPU must be available");
    TEST_ASSERT(m.utilizationPercent.has_value() && std::abs(*m.utilizationPercent - 72.0) < 0.01,
                "Utilization must still be read when hwmon is absent");
    TEST_ASSERT(m.memoryTotalBytes == 8589934592ULL, "VRAM total must be read");
    TEST_ASSERT(m.memoryUsedBytes == 2147483648ULL, "VRAM used must be read");
    TEST_ASSERT(!m.temperatureC.has_value(), "Temperature must be nullopt when hwmon is absent");
    TEST_ASSERT(!m.powerWatts.has_value(), "Power must be nullopt when hwmon is absent");
    TEST_ASSERT(!m.fanRpm.has_value(), "Fan RPM must be nullopt when hwmon is absent");

    // 2. Multi-Gateway Route Table with metric priority and virtual interface filtering
    {
        std::ofstream out(testRoute);
        out << "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\tMTU\tWindow\tIRTT\n"
            << "virbr0\t00000000\t0101A8C0\t0003\t0\t0\t5\t00000000\t0\t0\t0\n"
            << "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t300\t00000000\t0\t0\t0\n"
            << "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
            << "tailscale0\t00000000\t0101A8C0\t0003\t0\t0\t50\t00000000\t0\t0\t0\n";
    }

    std::string best = BareStats::LinuxProcNetworkCollector::DetectDefaultGatewayInterface(testRoute);
    TEST_ASSERT(best == "eth0",
                "DetectDefaultGatewayInterface must filter virbr0/tailscale0 and pick eth0 (metric 100 < 300)");

    // 3. Nonexistent DRM base path
    BareStats::LinuxSysfsGpuCollector badCollector("nonexistent_drm_path");
    auto emptyCards = badCollector.EnumerateCards();
    TEST_ASSERT(emptyCards.empty(), "Nonexistent DRM directory must safely return empty card list");

    std::filesystem::remove_all(mockDrm, ec);
    std::filesystem::remove(testRoute, ec);
    return true;
}

